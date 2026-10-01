// Targets branches in risk_manager.cpp not exercised by the existing
// test_risk_manager.cpp suite: process_positions early returns, update_config,
// create_market_data edge cases, calculate_99th_percentile, and the
// catch-all error path.

#include <gtest/gtest.h>
#include <arrow/api.h>
#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/util/logging.h>
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>
// Pre-load every std/project header that risk_manager.hpp transitively
// includes, so the `private public` redefinition below only affects
// risk_manager.hpp itself (and not stdlib internals that would explode).
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/portfolio/component_book.hpp"
#include "../core/test_base.hpp"

#define private public
#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

RiskConfig default_config() {
    RiskConfig c;
    c.var_limit = 0.15;
    c.jump_risk_limit = 0.10;
    c.max_correlation = 0.7;
    c.max_gross_leverage = 4.0;
    c.max_net_leverage = 2.0;
    c.capital = 1'000'000.0;
    c.confidence_level = 0.99;
    c.lookback_period = 252;
    return c;
}

Bar make_bar(const std::string& symbol, double close,
              std::chrono::system_clock::time_point ts) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = ts;
    b.open = b.close = Decimal(close);
    b.high = Decimal(close * 1.01);
    b.low = Decimal(close * 0.99);
    b.volume = 10000.0;
    return b;
}

std::unordered_map<std::string, Position> single_position(const std::string& sym, double qty,
                                                          double price) {
    return {{sym, Position(sym, Quantity(qty), Price(price), Decimal(0.0), Decimal(0.0),
                            Timestamp{})}};
}

class ScopedDiagnosticInstruments {
public:
    ScopedDiagnosticInstruments() {
        auto& instruments = InstrumentRegistry::instance().instruments_;
        for (const auto& symbol : {"DIAG_FUTURE", "DIAG_EQUITY"}) {
            auto it = instruments.find(symbol);
            if (it != instruments.end()) previous_.emplace(symbol, it->second);
        }

        FuturesSpec future{};
        future.root_symbol = "DIAG_FUTURE";
        future.exchange = "TEST";
        future.currency = "USD";
        future.multiplier = 10.0;
        future.tick_size = 0.25;
        instruments["DIAG_FUTURE"] =
            std::make_shared<FuturesInstrument>("DIAG_FUTURE", future);

        EquitySpec equity;
        equity.exchange = "TEST";
        equity.currency = "USD";
        instruments["DIAG_EQUITY"] =
            std::make_shared<EquityInstrument>("DIAG_EQUITY", equity);
    }

    ~ScopedDiagnosticInstruments() {
        auto& instruments = InstrumentRegistry::instance().instruments_;
        for (const auto& symbol : {"DIAG_FUTURE", "DIAG_EQUITY"}) {
            auto it = previous_.find(symbol);
            if (it == previous_.end()) instruments.erase(symbol);
            else instruments[symbol] = it->second;
        }
    }

private:
    std::unordered_map<std::string, std::shared_ptr<Instrument>> previous_;
};

class ScopedSameRootInstruments {
public:
    static constexpr const char* root = "DIAG_COLLISION";

    ScopedSameRootInstruments() {
        auto& registry = InstrumentRegistry::instance();
        const auto generic = registry.instruments_.find(root);
        if (generic != registry.instruments_.end()) {
            had_generic_ = true;
            previous_generic_ = generic->second;
        }
        const auto future = registry.futures_.find(root);
        if (future != registry.futures_.end()) {
            had_future_ = true;
            previous_future_ = future->second;
        }
        const auto equity = registry.equities_.find(root);
        if (equity != registry.equities_.end()) {
            had_equity_ = true;
            previous_equity_ = equity->second;
        }

        FuturesSpec future_spec{};
        future_spec.root_symbol = root;
        future_spec.exchange = "TEST";
        future_spec.currency = "USD";
        future_spec.multiplier = 10.0;
        future_spec.tick_size = 0.25;
        auto registered_future = std::make_shared<FuturesInstrument>(root, future_spec);
        EquitySpec equity_spec;
        equity_spec.exchange = "TEST";
        equity_spec.currency = "USD";
        auto registered_equity = std::make_shared<EquityInstrument>(root, equity_spec);
        registry.futures_[root] = registered_future;
        registry.equities_[root] = registered_equity;
        registry.instruments_[root] = registered_equity;
    }

    ~ScopedSameRootInstruments() {
        auto& registry = InstrumentRegistry::instance();
        if (had_generic_) registry.instruments_[root] = previous_generic_;
        else registry.instruments_.erase(root);
        if (had_future_) registry.futures_[root] = previous_future_;
        else registry.futures_.erase(root);
        if (had_equity_) registry.equities_[root] = previous_equity_;
        else registry.equities_.erase(root);
    }

private:
    bool had_generic_{false};
    bool had_future_{false};
    bool had_equity_{false};
    std::shared_ptr<Instrument> previous_generic_;
    std::shared_ptr<FuturesInstrument> previous_future_;
    std::shared_ptr<EquityInstrument> previous_equity_;
};

MarketData same_root_market_data() {
    MarketData data;
    data.ordered_symbols = {"DIAG_COLLISION.v.0", "DIAG_COLLISION.c.0"};
    data.symbol_indices = {{data.ordered_symbols[0], 0}, {data.ordered_symbols[1], 1}};
    data.returns = {{0.01, -0.02}, {-0.01, 0.02}};
    data.covariance = {{0.04, 0.0}, {0.0, 0.16}};
    return data;
}

std::unordered_map<std::string, Position> same_root_positions() {
    auto positions = single_position("DIAG_COLLISION.v.0", 1.0, 100.0);
    positions.emplace("DIAG_COLLISION.c.0",
                      Position("DIAG_COLLISION.c.0", Quantity(1), Price(50), Decimal(0),
                               Decimal(0), Timestamp{}));
    return positions;
}

MarketData diagnostic_market_data() {
    MarketData md;
    md.ordered_symbols = {"DIAG_FUTURE", "DIAG_EQUITY"};
    md.symbol_indices = {{"DIAG_FUTURE", 0}, {"DIAG_EQUITY", 1}};
    md.covariance = {{0.04, 0.0}, {0.0, 0.16}};
    md.returns = {{0.0, 0.0}};
    return md;
}

std::unordered_map<std::string, Position> diagnostic_positions() {
    auto positions = single_position("DIAG_FUTURE", 1.0, 100.0);
    positions.emplace("DIAG_EQUITY",
                      Position("DIAG_EQUITY", Quantity(1.0), Price(100.0), Decimal(0.0),
                               Decimal(0.0), Timestamp{}));
    return positions;
}

RiskConfig diagnostic_config(double var_limit) {
    auto config = default_config();
    config.var_limit = var_limit;
    config.jump_risk_limit = 100.0;
    config.max_correlation = 1.0;
    config.max_gross_leverage = 100.0;
    config.max_net_leverage = 100.0;
    return config;
}

void expect_all_risk_fields(const RiskResult& actual, const RiskResult& expected) {
    EXPECT_EQ(actual.risk_exceeded, expected.risk_exceeded);
    EXPECT_DOUBLE_EQ(actual.recommended_scale, expected.recommended_scale);
    EXPECT_DOUBLE_EQ(actual.portfolio_var, expected.portfolio_var);
    EXPECT_DOUBLE_EQ(actual.jump_risk, expected.jump_risk);
    EXPECT_DOUBLE_EQ(actual.correlation_risk, expected.correlation_risk);
    EXPECT_DOUBLE_EQ(actual.gross_leverage, expected.gross_leverage);
    EXPECT_DOUBLE_EQ(actual.net_leverage, expected.net_leverage);
    EXPECT_DOUBLE_EQ(actual.max_portfolio_risk, expected.max_portfolio_risk);
    EXPECT_DOUBLE_EQ(actual.max_jump_risk, expected.max_jump_risk);
    EXPECT_DOUBLE_EQ(actual.max_leverage_risk, expected.max_leverage_risk);
    EXPECT_DOUBLE_EQ(actual.portfolio_multiplier, expected.portfolio_multiplier);
    EXPECT_DOUBLE_EQ(actual.jump_multiplier, expected.jump_multiplier);
    EXPECT_DOUBLE_EQ(actual.correlation_multiplier, expected.correlation_multiplier);
    EXPECT_DOUBLE_EQ(actual.leverage_multiplier, expected.leverage_multiplier);
    EXPECT_DOUBLE_EQ(actual.portfolio_var_gate, expected.portfolio_var_gate);
}

void expect_all_risk_fields_near(const RiskResult& actual, const RiskResult& expected) {
    EXPECT_EQ(actual.risk_exceeded, expected.risk_exceeded);
    EXPECT_NEAR(actual.recommended_scale, expected.recommended_scale, 1e-12);
    EXPECT_NEAR(actual.portfolio_var, expected.portfolio_var, 1e-12);
    EXPECT_NEAR(actual.jump_risk, expected.jump_risk, 1e-12);
    EXPECT_NEAR(actual.correlation_risk, expected.correlation_risk, 1e-12);
    EXPECT_NEAR(actual.gross_leverage, expected.gross_leverage, 1e-12);
    EXPECT_NEAR(actual.net_leverage, expected.net_leverage, 1e-12);
    EXPECT_NEAR(actual.max_portfolio_risk, expected.max_portfolio_risk, 1e-12);
    EXPECT_NEAR(actual.max_jump_risk, expected.max_jump_risk, 1e-12);
    EXPECT_NEAR(actual.max_leverage_risk, expected.max_leverage_risk, 1e-12);
    EXPECT_NEAR(actual.portfolio_multiplier, expected.portfolio_multiplier, 1e-12);
    EXPECT_NEAR(actual.jump_multiplier, expected.jump_multiplier, 1e-12);
    EXPECT_NEAR(actual.correlation_multiplier, expected.correlation_multiplier, 1e-12);
    EXPECT_NEAR(actual.leverage_multiplier, expected.leverage_multiplier, 1e-12);
    EXPECT_NEAR(actual.portfolio_var_gate, expected.portfolio_var_gate, 1e-12);
}

}  // namespace

class RiskManagerExtendedTest : public TestBase {};

namespace {
void expect_no_risk_reads(const RiskConfigConsumption& read) {
    EXPECT_FALSE(read.var_limit.has_value());
    EXPECT_FALSE(read.jump_risk_limit.has_value());
    EXPECT_FALSE(read.max_correlation.has_value());
    EXPECT_FALSE(read.max_gross_leverage.has_value());
    EXPECT_FALSE(read.max_net_leverage.has_value());
    EXPECT_FALSE(read.confidence_level.has_value());
    EXPECT_FALSE(read.capital.has_value());
}

void expect_all_risk_reads(const RiskConfigConsumption& read, const RiskConfig& config) {
    ASSERT_TRUE(read.var_limit.has_value());
    ASSERT_TRUE(read.jump_risk_limit.has_value());
    ASSERT_TRUE(read.max_correlation.has_value());
    ASSERT_TRUE(read.max_gross_leverage.has_value());
    ASSERT_TRUE(read.max_net_leverage.has_value());
    ASSERT_TRUE(read.confidence_level.has_value());
    ASSERT_TRUE(read.capital.has_value());
    EXPECT_DOUBLE_EQ(*read.var_limit, config.var_limit);
    EXPECT_DOUBLE_EQ(*read.jump_risk_limit, config.jump_risk_limit);
    EXPECT_DOUBLE_EQ(*read.max_correlation, config.max_correlation);
    EXPECT_DOUBLE_EQ(*read.max_gross_leverage, config.max_gross_leverage);
    EXPECT_DOUBLE_EQ(*read.max_net_leverage, config.max_net_leverage);
    EXPECT_DOUBLE_EQ(*read.confidence_level, config.confidence_level);
    EXPECT_EQ(read.capital->raw_value(), config.capital.raw_value());
}

void expect_full_error_parity(const Result<RiskResult>& observed,
                              const Result<RiskResult>& unobserved) {
    ASSERT_TRUE(observed.is_error());
    ASSERT_TRUE(unobserved.is_error());
    ASSERT_NE(observed.error(), nullptr);
    ASSERT_NE(unobserved.error(), nullptr);
    EXPECT_EQ(observed.error()->code(), unobserved.error()->code());
    EXPECT_EQ(std::string(observed.error()->what()),
              std::string(unobserved.error()->what()));
}

std::vector<Timestamp> frozen_times() {
    const auto start = Timestamp{} + std::chrono::hours(24);
    return {start, start + std::chrono::hours(24), start + std::chrono::hours(48)};
}

std::vector<RiskCloseInput> frozen_closes() {
    const auto times = frozen_times();
    return {{"risk:0", times[0], 100.0}, {"risk:0", times[1], 101.0},
            {"risk:0", times[2], 100.0}, {"risk:1", times[0], 200.0},
            {"risk:1", times[1], 204.0}, {"risk:1", times[2], 200.0}};
}

std::vector<RiskValuationInput> frozen_valuations() {
    return {{"risk:0", 100.0, 10.0}, {"risk:1", 100.0, 1.0}};
}

template <typename T>
void expect_argument_error(const Result<T>& result, const std::string& prefix) {
    ASSERT_TRUE(result.is_error());
    ASSERT_NE(result.error(), nullptr);
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_EQ(std::string(result.error()->what()).find(prefix), 0u);
}
}  // namespace

TEST_F(RiskManagerExtendedTest, ModelObservesExactOperationalValuesWithoutChangingRiskFields) {
    ScopedDiagnosticInstruments instruments;
    auto config = diagnostic_config(0.173);
    config.jump_risk_limit = 0.0075;
    config.max_correlation = 0.625;  // The observed comparison does not bind.
    config.max_gross_leverage = 0.0007;
    config.max_net_leverage = 0.00123;
    config.confidence_level = 0.875;
    config.capital = Decimal(1234567.125);
    RiskManager mgr(config);
    auto data = diagnostic_market_data();
    data.covariance = {{0.04, 0.04}, {0.04, 0.16}};
    data.returns = {{0.01, 0.02}, {-0.01, -0.02}};
    const auto positions = diagnostic_positions();

    const auto without = mgr.process_positions(positions, data);
    RiskConfigConsumption read;
    const auto with = mgr.process_positions(positions, data, {}, &read);
    ASSERT_TRUE(without.is_ok());
    ASSERT_TRUE(with.is_ok());
    expect_all_risk_fields(with.value(), without.value());
    expect_all_risk_reads(read, config);

    const double gate = std::sqrt(4.96 / 121.0);
    const double report = std::sqrt(0.07);
    const double weighted_return = 0.01 * 10.0 / 11.0 + 0.02 / 11.0;
    EXPECT_NEAR(with.value().portfolio_var_gate, gate, 1e-12);
    EXPECT_NEAR(with.value().portfolio_var, report, 1e-12);
    EXPECT_NEAR(with.value().portfolio_multiplier, 0.173 / gate, 1e-12);
    EXPECT_NEAR(with.value().max_portfolio_risk, weighted_return, 1e-12);
    EXPECT_NEAR(with.value().jump_risk, weighted_return, 1e-12);
    EXPECT_NEAR(with.value().jump_multiplier, 0.0075 / weighted_return, 1e-12);
    EXPECT_NEAR(with.value().max_jump_risk, weighted_return, 1e-12);
    EXPECT_NEAR(with.value().correlation_risk, 0.5, 1e-12);
    EXPECT_DOUBLE_EQ(with.value().correlation_multiplier, 1.0);
    EXPECT_NEAR(with.value().gross_leverage, 1100.0 / 1234567.125, 1e-12);
    EXPECT_NEAR(with.value().net_leverage, 1100.0 / 1234567.125, 1e-12);
    EXPECT_NEAR(with.value().max_leverage_risk, 0.03 / 1234567.125, 1e-12);
    EXPECT_NEAR(with.value().leverage_multiplier,
                0.0007 / (1100.0 / 1234567.125), 1e-12);
    EXPECT_NEAR(with.value().recommended_scale, 0.0075 / weighted_return, 1e-12);
    EXPECT_TRUE(with.value().risk_exceeded);

    config.max_correlation = 0.25;
    ASSERT_TRUE(mgr.update_config(config).is_ok());
    const auto binding_without = mgr.process_positions(positions, data);
    const auto binding_with = mgr.process_positions(positions, data, {}, &read);
    ASSERT_TRUE(binding_without.is_ok());
    ASSERT_TRUE(binding_with.is_ok());
    expect_all_risk_fields(binding_with.value(), binding_without.value());
    expect_all_risk_reads(read, config);
    EXPECT_NEAR(binding_with.value().correlation_multiplier, 0.5, 1e-12);
}

TEST_F(RiskManagerExtendedTest, FrozenAndModelObserveSameNumericalTail) {
    ScopedDiagnosticInstruments instruments;
    auto config = diagnostic_config(0.153);
    config.jump_risk_limit = 0.0091;
    config.max_correlation = 0.63;
    config.max_gross_leverage = 0.0021;
    config.max_net_leverage = 0.0017;
    config.confidence_level = 0.84;
    config.capital = Decimal(765432.125);
    RiskManager mgr(config);
    auto times = frozen_times();
    auto valuations = frozen_valuations();
    valuations[0].calculation_id = "DIAG_FUTURE";
    valuations[1].calculation_id = "DIAG_EQUITY";
    auto closes = frozen_closes();
    for (auto& close : closes) {
        close.calculation_id = close.calculation_id == "risk:0" ? "DIAG_FUTURE" : "DIAG_EQUITY";
    }
    auto frozen = mgr.make_frozen_snapshot(times.back(), valuations, times, closes);
    ASSERT_TRUE(frozen.is_ok());
    const auto quantities = std::unordered_map<RiskCalculationId, Quantity>{
        {"DIAG_FUTURE", Quantity(1)}, {"DIAG_EQUITY", Quantity(1)}};
    RiskConfigConsumption frozen_read;
    const auto strict = mgr.process_positions_frozen(quantities, *frozen.value(), &frozen_read);
    const auto strict_without = mgr.process_positions_frozen(quantities, *frozen.value());
    RiskConfigConsumption model_read;
    const auto model = mgr.process_positions(diagnostic_positions(), frozen.value()->market_data_,
                                             {}, &model_read);
    ASSERT_TRUE(strict.is_ok());
    ASSERT_TRUE(strict_without.is_ok());
    ASSERT_TRUE(model.is_ok());
    expect_all_risk_fields(strict.value(), strict_without.value());
    expect_all_risk_fields(strict.value(), model.value());
    expect_all_risk_reads(frozen_read, config);
    expect_all_risk_reads(model_read, config);
    EXPECT_EQ(frozen_read.capital->raw_value(), model_read.capital->raw_value());
}

TEST_F(RiskManagerExtendedTest, ConditionalReadsAndDefaultPathsClearReusedOutput) {
    ScopedDiagnosticInstruments instruments;
    auto config = diagnostic_config(0.151);
    config.jump_risk_limit = 0.091;
    config.max_correlation = 0.67;
    config.max_gross_leverage = 3.7;
    config.max_net_leverage = 1.7;
    config.confidence_level = 0.91;
    config.capital = Decimal(876543.125);
    RiskManager mgr(config);
    auto data = diagnostic_market_data();
    data.returns = {{0.01, 0.02}};
    RiskConfigConsumption read;
    ASSERT_TRUE(mgr.process_positions(diagnostic_positions(), data, {}, &read).is_ok());
    expect_all_risk_reads(read, config);

    auto zero = data;
    zero.covariance = {{0.0, 0.0}, {0.0, 0.0}};
    zero.returns = {{0.0, 0.0}};
    ASSERT_TRUE(mgr.process_positions(diagnostic_positions(), zero, {}, &read).is_ok());
    EXPECT_FALSE(read.var_limit.has_value());
    EXPECT_FALSE(read.jump_risk_limit.has_value());
    ASSERT_TRUE(read.max_correlation.has_value());
    EXPECT_DOUBLE_EQ(*read.max_correlation, config.max_correlation);
    ASSERT_TRUE(read.confidence_level.has_value());
    EXPECT_DOUBLE_EQ(*read.confidence_level, config.confidence_level);
    ASSERT_TRUE(read.capital.has_value());
    EXPECT_EQ(read.capital->raw_value(), config.capital.raw_value());
    ASSERT_TRUE(read.max_gross_leverage.has_value());
    ASSERT_TRUE(read.max_net_leverage.has_value());

    ASSERT_TRUE(mgr.process_positions({}, data, {}, &read).is_ok());
    expect_no_risk_reads(read);
    auto missing = data;
    missing.returns.clear();
    read.var_limit = -1.0;
    ASSERT_TRUE(mgr.process_positions(diagnostic_positions(), missing, {}, &read).is_ok());
    expect_no_risk_reads(read);
    read.capital = Decimal(1);
    ASSERT_TRUE(mgr.process_positions(single_position("UNKNOWN", 1.0, 100.0), data,
                                      {}, &read).is_ok());
    expect_no_risk_reads(read);

    config.var_limit = 0.127;
    config.jump_risk_limit = 0.037;
    config.max_correlation = 0.57;
    config.max_gross_leverage = 2.7;
    config.max_net_leverage = 1.3;
    config.confidence_level = 0.81;
    config.capital = Decimal(654321.875);
    ASSERT_TRUE(mgr.update_config(config).is_ok());
    ASSERT_TRUE(mgr.process_positions(diagnostic_positions(), data, {}, &read).is_ok());
    expect_all_risk_reads(read, config);
}

TEST_F(RiskManagerExtendedTest, FrozenRejectionsClearOutputBeforeValidationAndRetainPartialReads) {
    RiskManager mgr(diagnostic_config(0.15));
    const auto times = frozen_times();
    auto snap = mgr.make_frozen_snapshot(times.back(), frozen_valuations(), times,
                                         frozen_closes());
    ASSERT_TRUE(snap.is_ok());
    const auto book = std::unordered_map<RiskCalculationId, Quantity>{
        {"risk:0", Quantity(1)}, {"risk:1", Quantity(1)}};
    RiskConfigConsumption read;
    ASSERT_TRUE(mgr.process_positions_frozen(book, *snap.value(), &read).is_ok());
    ASSERT_TRUE(read.capital.has_value());

    auto bad = mgr.config_;
    bad.var_limit = std::numeric_limits<double>::quiet_NaN();
    mgr.config_ = bad;
    const auto invalid_limit = mgr.process_positions_frozen(book, *snap.value(), &read);
    const auto invalid_limit_without = mgr.process_positions_frozen(book, *snap.value());
    expect_argument_error(invalid_limit, "invalid_var_limit");
    expect_full_error_parity(invalid_limit, invalid_limit_without);
    expect_no_risk_reads(read);
    mgr.config_ = diagnostic_config(0.15);
    read.jump_risk_limit = -1.0;
    const auto empty_book = mgr.process_positions_frozen({}, *snap.value(), &read);
    const auto empty_book_without = mgr.process_positions_frozen({}, *snap.value());
    expect_argument_error(empty_book, "empty_risk_book");
    expect_full_error_parity(empty_book, empty_book_without);
    expect_no_risk_reads(read);
    read.confidence_level = -1.0;
    const auto incomplete_book = mgr.process_positions_frozen({{"risk:0", Quantity(1)}},
                                                               *snap.value(), &read);
    const auto incomplete_book_without = mgr.process_positions_frozen(
        {{"risk:0", Quantity(1)}}, *snap.value());
    expect_argument_error(incomplete_book, "quantity_id_set_size");
    expect_full_error_parity(incomplete_book, incomplete_book_without);
    expect_no_risk_reads(read);

    auto values = frozen_valuations();
    values[0].mark = 1e301;
    values[0].price_multiplier = 1.0;
    auto huge = mgr.make_frozen_snapshot(times.back(), values, times, frozen_closes());
    ASSERT_TRUE(huge.is_ok());
    bad = diagnostic_config(0.15);
    bad.capital = Decimal::from_raw(1);
    mgr.config_ = bad;
    auto failure = mgr.process_positions_frozen(book, *huge.value(), &read);
    const auto failure_without = mgr.process_positions_frozen(book, *huge.value());
    ASSERT_TRUE(failure.is_error());
    ASSERT_NE(failure.error(), nullptr);
    EXPECT_EQ(failure.error()->code(), ErrorCode::INVALID_RISK_CALCULATION);
    EXPECT_EQ(std::string(failure.error()->what()).find("nonfinite_risk_output"), 0u);
    expect_full_error_parity(failure, failure_without);
    ASSERT_TRUE(read.confidence_level.has_value());
    ASSERT_TRUE(read.capital.has_value());
    EXPECT_EQ(read.capital->raw_value(), 1);
    ASSERT_TRUE(read.var_limit.has_value());
    ASSERT_TRUE(read.jump_risk_limit.has_value());
    ASSERT_TRUE(read.max_correlation.has_value());
    ASSERT_TRUE(read.max_gross_leverage.has_value());
    ASSERT_TRUE(read.max_net_leverage.has_value());
}

TEST_F(RiskManagerExtendedTest, FrozenTwoColumnOracleUsesDistinctValuesAndSharedEquations) {
    ScopedDiagnosticInstruments instruments;
    auto config = diagnostic_config(0.15);
    config.jump_risk_limit = 100.0;
    RiskManager mgr(config);
    auto times = frozen_times();
    auto snap = mgr.make_frozen_snapshot(times.back(), frozen_valuations(), times, frozen_closes());
    ASSERT_TRUE(snap.is_ok()) << (snap.error() ? snap.error()->what() : "");
    ASSERT_TRUE(snap.value());
    EXPECT_EQ(snap.value()->market_data_.ordered_symbols,
              (std::vector<std::string>{"risk:0", "risk:1"}));
    EXPECT_EQ(snap.value()->market_data_.returns.size(), 2u);
    EXPECT_DOUBLE_EQ(snap.value()->market_data_.returns[0][0], 0.01);
    EXPECT_DOUBLE_EQ(snap.value()->market_data_.returns[0][1], 0.02);
    const auto quantities = std::unordered_map<RiskCalculationId, Quantity>{
        {"risk:0", Quantity(1)}, {"risk:1", Quantity(1)}};
    auto actual = mgr.process_positions_frozen(quantities, *snap.value());
    ASSERT_TRUE(actual.is_ok()) << (actual.error() ? actual.error()->what() : "");
    const double da = 1.0 / 100.0 + 1.0 / 101.0;
    const double db = 1.0 / 50.0 + 1.0 / 51.0;
    const double gate = std::sqrt(126.0) * (10.0 * da + db) / 11.0;
    const double report = std::sqrt(126.0) * (da + db) / 2.0;
    EXPECT_NEAR(actual.value().portfolio_var_gate, gate, 1e-12);
    EXPECT_NEAR(actual.value().portfolio_var, report, 1e-12);
    EXPECT_NEAR(actual.value().portfolio_multiplier, 0.15 / gate, 1e-12);
    EXPECT_DOUBLE_EQ(actual.value().gross_leverage, 0.0011);
    EXPECT_DOUBLE_EQ(actual.value().net_leverage, 0.0011);
    EXPECT_TRUE(actual.value().risk_exceeded);
}

TEST_F(RiskManagerExtendedTest, FrozenRejectsMissingOrDuplicatedInputInsteadOfDefaultSuccess) {
    RiskManager mgr(default_config());
    const auto times = frozen_times();
    auto values = frozen_valuations();
    auto closes = frozen_closes();
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), {}, times, closes), "empty_risk_snapshot");
    values.push_back(values.front());
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, times, closes), "duplicate_calculation_id");
    values.pop_back();
    closes.pop_back();
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, times, closes), "missing_close");
    closes = frozen_closes();
    closes.push_back(closes.front());
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, times, closes), "duplicate_close");
}

TEST_F(RiskManagerExtendedTest, FrozenRejectsUnsupportedPricePrecisionAndBadDenominator) {
    RiskManager mgr(default_config());
    auto times = frozen_times();
    auto closes = frozen_closes();
    closes[2].close = 1.000000001;
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), frozen_valuations(), times, closes),
                          "close_price_precision");
    closes = frozen_closes();
    closes[0].close = 0.0;
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), frozen_valuations(), times, closes),
                          "zero_close_denominator");
    closes = frozen_closes();
    closes[0].close = 1e100;
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), frozen_valuations(), times, closes),
                          "close_price_range");
}

TEST_F(RiskManagerExtendedTest, FrozenValidatesEveryGridCellAndPriceBoundary) {
    RiskManager mgr(default_config());
    const auto times = frozen_times();
    auto values = frozen_valuations();
    auto closes = frozen_closes();
    auto bad_times = times;
    bad_times[1] = bad_times[0];
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, bad_times, closes),
                          "unordered_observation_time");
    bad_times = times;
    bad_times[1] = bad_times[2] + std::chrono::hours(24);
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, bad_times, closes),
                          "future_observation_time");
    bad_times = {times[0], times[1]};
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, bad_times, closes),
                          "insufficient_observation_times");
    closes.erase(closes.begin() + 1);
    closes.erase(closes.begin() + 3);
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, times, closes),
                          "missing_close");
    closes = frozen_closes();
    closes[0].timestamp += std::chrono::hours(1);
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, times, closes),
                          "extra_close_time");
    closes = frozen_closes();
    closes[0].calculation_id = "extra";
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, times, closes),
                          "extra_close_id");
    closes = frozen_closes();
    closes[0].close = std::numeric_limits<double>::infinity();
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, times, closes),
                          "nonfinite_close");
    closes[0].close = std::numeric_limits<double>::quiet_NaN();
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, times, closes),
                          "nonfinite_close");
    closes = frozen_closes();
    closes[2].close = 1e-10;
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, times, closes),
                          "close_price_precision");
    closes[2].close = 0.0;
    closes[0].close = 0.00000001;
    closes[1].close = 0.00000002;
    closes[3].close = -0.00000001;
    closes[4].close = -0.00000002;
    closes[5].close = -0.00000001;
    auto accepted = mgr.make_frozen_snapshot(times.back(), values, times, closes);
    ASSERT_TRUE(accepted.is_ok()) << (accepted.error() ? accepted.error()->what() : "");
    EXPECT_DOUBLE_EQ(accepted.value()->market_data_.returns[0][0], 1.0);
    EXPECT_DOUBLE_EQ(accepted.value()->market_data_.returns[0][1], 1.0);
    EXPECT_DOUBLE_EQ(accepted.value()->market_data_.returns[1][0], -1.0);
}

TEST_F(RiskManagerExtendedTest, FrozenValuationAndQuantitySetsAreExactEvenForZeroBook) {
    RiskManager mgr(default_config());
    const auto times = frozen_times();
    auto values = frozen_valuations();
    auto closes = frozen_closes();
    values[0].calculation_id.clear();
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, times, closes),
                          "empty_calculation_id");
    values = frozen_valuations();
    values[0].mark = std::numeric_limits<double>::quiet_NaN();
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, times, closes),
                          "invalid_mark");
    values[0].mark = 100.0;
    values[0].price_multiplier = 0.0;
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, times, closes),
                          "invalid_price_multiplier");
    values[0].price_multiplier = -1.0;
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, times, closes),
                          "invalid_price_multiplier");
    values[0].price_multiplier = std::numeric_limits<double>::infinity();
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), values, times, closes),
                          "invalid_price_multiplier");
    auto snap = mgr.make_frozen_snapshot(times.back(), frozen_valuations(), times, closes);
    ASSERT_TRUE(snap.is_ok());
    expect_argument_error(mgr.process_positions_frozen({}, *snap.value()), "empty_risk_book");
    expect_argument_error(mgr.process_positions_frozen({{"risk:0", Quantity(0)}}, *snap.value()),
                          "quantity_id_set_size");
    expect_argument_error(mgr.process_positions_frozen(
                              {{"risk:0", Quantity(0)}, {"extra", Quantity(0)}}, *snap.value()),
                          "missing_quantity");
    auto zero = mgr.process_positions_frozen(
        {{"risk:0", Quantity(0)}, {"risk:1", Quantity(0)}}, *snap.value());
    ASSERT_TRUE(zero.is_ok());
    EXPECT_DOUBLE_EQ(zero.value().portfolio_var_gate, 0.0);
    EXPECT_DOUBLE_EQ(zero.value().portfolio_var, 0.0);
    EXPECT_DOUBLE_EQ(zero.value().jump_risk, 0.0);
    EXPECT_DOUBLE_EQ(zero.value().correlation_risk, 0.0);
    EXPECT_DOUBLE_EQ(zero.value().gross_leverage, 0.0);
    EXPECT_DOUBLE_EQ(zero.value().net_leverage, 0.0);
    EXPECT_DOUBLE_EQ(zero.value().recommended_scale, 1.0);
    EXPECT_NEAR(zero.value().max_leverage_risk, 0.03 / 1'000'000.0, 1e-18);
}

TEST_F(RiskManagerExtendedTest, FrozenPrivateMatrixValidatorRejectsUnsafeLayouts) {
    RiskManager mgr(default_config());
    const auto times = frozen_times();
    auto snap = mgr.make_frozen_snapshot(times.back(), frozen_valuations(), times,
                                         frozen_closes());
    ASSERT_TRUE(snap.is_ok());
    auto md = snap.value()->market_data_;
    const auto valid = md;
    const std::vector<RiskCalculationId> ids{"risk:0", "risk:1"};
    ASSERT_TRUE(mgr.validate_frozen_market_data(md, ids, 2).is_ok());
    md.returns.clear();
    expect_argument_error(mgr.validate_frozen_market_data(md, ids, 2), "return_row_count");
    md = valid;
    md.returns[0].pop_back();
    expect_argument_error(mgr.validate_frozen_market_data(md, ids, 2), "return_width");
    md = valid;
    md.returns[0][0] = std::numeric_limits<double>::infinity();
    expect_argument_error(mgr.validate_frozen_market_data(md, ids, 2), "return_nonfinite");
    md = valid;
    md.returns[0] = {std::numeric_limits<double>::max(),
                     std::numeric_limits<double>::max()};
    expect_argument_error(mgr.validate_frozen_market_data(md, ids, 2), "return_abs_sum");
    md = valid;
    md.symbol_indices.erase("risk:1");
    expect_argument_error(mgr.validate_frozen_market_data(md, ids, 2), "market_index_count");
    md = valid;
    md.symbol_indices["extra"] = 0;
    expect_argument_error(mgr.validate_frozen_market_data(md, ids, 2), "market_index_count");
    md = valid;
    md.symbol_indices["risk:1"] = 0;
    expect_argument_error(mgr.validate_frozen_market_data(md, ids, 2), "market_symbol_index");
    md = valid;
    md.symbol_indices["risk:1"] = 3;
    expect_argument_error(mgr.validate_frozen_market_data(md, ids, 2), "market_symbol_index");
    md = valid;
    md.ordered_symbols[1] = "risk:0";
    expect_argument_error(mgr.validate_frozen_market_data(md, ids, 2), "market_symbol_order");
    md = valid;
    md.covariance.clear();
    expect_argument_error(mgr.validate_frozen_market_data(md, ids, 2), "covariance_row_count");
    md = valid;
    md.covariance[0].pop_back();
    expect_argument_error(mgr.validate_frozen_market_data(md, ids, 2), "covariance_width");
    md = valid;
    md.covariance[0][0] = -1.0;
    expect_argument_error(mgr.validate_frozen_market_data(md, ids, 2),
                          "covariance_negative_diagonal");
    md = valid;
    md.covariance[0][1] = std::numeric_limits<double>::quiet_NaN();
    expect_argument_error(mgr.validate_frozen_market_data(md, ids, 2), "covariance_nonfinite");
    md = valid;
    for (auto& row : md.covariance) {
        for (auto& value : row) value = std::numeric_limits<double>::max();
    }
    expect_argument_error(mgr.validate_frozen_market_data(md, ids, 2), "covariance_abs_sum");
    md = valid;
    md.covariance = {{1e-300, 1e300}, {1e300, 1e-300}};
    expect_argument_error(mgr.validate_frozen_market_data(md, ids, 2),
                          "correlation_ratio");
}

TEST_F(RiskManagerExtendedTest, FrozenSnapshotOwnsInputsAndIgnoresRegistryChanges) {
    RiskManager mgr(diagnostic_config(0.15));
    const auto times = frozen_times();
    auto valuations = frozen_valuations();
    auto closes = frozen_closes();
    auto frozen = mgr.make_frozen_snapshot(times.back(), valuations, times, closes);
    ASSERT_TRUE(frozen.is_ok());
    const auto book = std::unordered_map<RiskCalculationId, Quantity>{
        {"risk:0", Quantity(1)}, {"risk:1", Quantity(1)}};
    auto first = mgr.process_positions_frozen(book, *frozen.value());
    ASSERT_TRUE(first.is_ok());
    valuations[0].mark = 500.0;
    closes[0].close = 50.0;
    const auto original = InstrumentRegistry::instance().instruments_.find("risk:0");
    const bool existed = original != InstrumentRegistry::instance().instruments_.end();
    auto previous = existed ? original->second : std::shared_ptr<Instrument>{};
    FuturesSpec spec{};
    spec.root_symbol = "risk:0";
    spec.exchange = "TEST";
    spec.currency = "USD";
    spec.multiplier = 500.0;
    spec.tick_size = 0.25;
    InstrumentRegistry::instance().instruments_["risk:0"] =
        std::make_shared<FuturesInstrument>("risk:0", spec);
    auto second = mgr.process_positions_frozen(book, *frozen.value());
    if (existed) InstrumentRegistry::instance().instruments_["risk:0"] = previous;
    else InstrumentRegistry::instance().instruments_.erase("risk:0");
    ASSERT_TRUE(second.is_ok());
    expect_all_risk_fields(second.value(), first.value());

    auto changed = frozen_valuations();
    changed[0].mark = 200.0;
    auto alternative = mgr.make_frozen_snapshot(times.back(), changed, times, frozen_closes());
    ASSERT_TRUE(alternative.is_ok());
    auto other = mgr.process_positions_frozen(book, *alternative.value());
    ASSERT_TRUE(other.is_ok());
    EXPECT_GT(other.value().gross_leverage, first.value().gross_leverage);
    auto legacy = mgr.process_positions(diagnostic_positions(), diagnostic_market_data());
    ASSERT_TRUE(legacy.is_ok());
    auto again = mgr.process_positions_frozen(book, *frozen.value());
    ASSERT_TRUE(again.is_ok());
    expect_all_risk_fields(again.value(), first.value());
}

TEST_F(RiskManagerExtendedTest, FrozenPermutationsAndTypedSameSymbolMappingStayDistinct) {
    const InstrumentIdentity future{AssetType::FUTURE, "XYZ"};
    const InstrumentIdentity equity{AssetType::EQUITY, "XYZ"};
    const ComponentPositionKey future_key{"P", "F", "Future", "2026-09-23", "XYZ", "Q"};
    const ComponentPositionKey equity_key{"P", "E", "Equity", "2026-09-23", "XYZ", "Q"};
    const std::map<InstrumentIdentity, RiskCalculationId> forward{
        {future, "risk:0"}, {equity, "risk:1"}};
    const std::map<RiskCalculationId, InstrumentIdentity> reverse{
        {"risk:0", future}, {"risk:1", equity}};
    const std::map<RiskCalculationId, std::vector<ComponentPositionKey>> members{
        {"risk:0", {future_key}}, {"risk:1", {equity_key}}};
    ASSERT_EQ(forward.size(), reverse.size());
    for (const auto& [identity, id] : forward) {
        EXPECT_EQ(reverse.at(id), identity);
        EXPECT_EQ(members.at(id).size(), 1u);
    }
    EXPECT_NE(forward.at(future), forward.at(equity));
    EXPECT_EQ(members.at("risk:0").front(), future_key);
    EXPECT_EQ(members.at("risk:1").front(), equity_key);

    RiskManager mgr(diagnostic_config(0.15));
    const auto times = frozen_times();
    auto first = mgr.make_frozen_snapshot(times.back(), frozen_valuations(), times,
                                          frozen_closes());
    ASSERT_TRUE(first.is_ok());
    auto values = frozen_valuations();
    auto closes = frozen_closes();
    std::reverse(values.begin(), values.end());
    std::reverse(closes.begin(), closes.end());
    auto permuted = mgr.make_frozen_snapshot(times.back(), values, times, closes);
    ASSERT_TRUE(permuted.is_ok());
    EXPECT_EQ(first.value()->market_data_.ordered_symbols,
              permuted.value()->market_data_.ordered_symbols);
    EXPECT_EQ(first.value()->market_data_.returns, permuted.value()->market_data_.returns);
    EXPECT_EQ(first.value()->market_data_.covariance, permuted.value()->market_data_.covariance);
    const auto book = std::unordered_map<RiskCalculationId, Quantity>{
        {"risk:1", Quantity(1)}, {"risk:0", Quantity(1)}};
    auto a = mgr.process_positions_frozen(book, *first.value());
    auto b = mgr.process_positions_frozen(book, *permuted.value());
    ASSERT_TRUE(a.is_ok());
    ASSERT_TRUE(b.is_ok());
    expect_all_risk_fields(a.value(), b.value());
    EXPECT_DOUBLE_EQ(first.value()->valuations_.at("risk:0").price_multiplier, 10.0);
    EXPECT_DOUBLE_EQ(first.value()->valuations_.at("risk:1").price_multiplier, 1.0);
}

TEST_F(RiskManagerExtendedTest, FrozenRejectsInvalidConfigAndFiniteInputOverflow) {
    auto config = default_config();
    RiskManager mgr(config);
    const auto times = frozen_times();
    auto frozen = mgr.make_frozen_snapshot(times.back(), frozen_valuations(), times,
                                           frozen_closes());
    ASSERT_TRUE(frozen.is_ok());
    const auto book = std::unordered_map<RiskCalculationId, Quantity>{
        {"risk:0", Quantity(1)}, {"risk:1", Quantity(1)}};
    config.var_limit = std::numeric_limits<double>::quiet_NaN();
    mgr.config_ = config;
    expect_argument_error(mgr.process_positions_frozen(book, *frozen.value()), "invalid_var_limit");
    config = default_config();
    config.confidence_level = 1.0;
    mgr.config_ = config;
    expect_argument_error(mgr.process_positions_frozen(book, *frozen.value()),
                          "invalid_confidence_level");
    mgr.config_ = default_config();

    auto values = frozen_valuations();
    values[0].mark = 1e300;
    values[0].price_multiplier = 1e300;
    auto large = mgr.make_frozen_snapshot(times.back(), values, times, frozen_closes());
    ASSERT_TRUE(large.is_ok());
    expect_argument_error(mgr.process_positions_frozen(book, *large.value()), "position_product");
    values[0].mark = 1e308;
    values[0].price_multiplier = 1.0;
    values[1].mark = 1e308;
    auto gross = mgr.make_frozen_snapshot(times.back(), values, times, frozen_closes());
    ASSERT_TRUE(gross.is_ok());
    expect_argument_error(mgr.process_positions_frozen(book, *gross.value()), "gross_sum");
}

TEST_F(RiskManagerExtendedTest, FrozenRejectsUnderflowThatErasesNonzeroExposure) {
    RiskManager mgr(default_config());
    const auto times = frozen_times();
    auto values = frozen_valuations();
    const auto closes = frozen_closes();
    const auto book = std::unordered_map<RiskCalculationId, Quantity>{
        {"risk:0", Quantity(0.00000001)}, {"risk:1", Quantity(1)}};
    values[0].mark = 1e-320;
    auto base = mgr.make_frozen_snapshot(times.back(), values, times, closes);
    ASSERT_TRUE(base.is_ok());
    expect_argument_error(mgr.process_positions_frozen(book, *base.value()),
                          "position_product_underflow");
    values[0].mark = 1.0;
    values[0].price_multiplier = 1e-320;
    auto multiplied = mgr.make_frozen_snapshot(times.back(), values, times, closes);
    ASSERT_TRUE(multiplied.is_ok());
    expect_argument_error(mgr.process_positions_frozen(book, *multiplied.value()),
                          "position_product_underflow");
}

TEST_F(RiskManagerExtendedTest, FrozenAndModelUseIdenticalNumericalTailForCompleteValues) {
    ScopedDiagnosticInstruments instruments;
    const auto config = diagnostic_config(0.15);
    RiskManager frozen_manager(config);
    RiskManager model_manager(config);
    const auto times = frozen_times();
    auto valuations = frozen_valuations();
    valuations[0].calculation_id = "DIAG_FUTURE";
    valuations[1].calculation_id = "DIAG_EQUITY";
    auto closes = frozen_closes();
    for (auto& item : closes) {
        item.calculation_id = item.calculation_id == "risk:0" ? "DIAG_FUTURE" : "DIAG_EQUITY";
    }
    auto frozen = frozen_manager.make_frozen_snapshot(times.back(), valuations, times, closes);
    ASSERT_TRUE(frozen.is_ok());
    LoggerConfig log_config;
    log_config.min_level = LogLevel::DEBUG;
    log_config.destination = LogDestination::CONSOLE;
    log_config.include_timestamp = false;
    Logger::instance().initialize(log_config);
    ::testing::internal::CaptureStdout();
    auto strict = frozen_manager.process_positions_frozen(
        {{"DIAG_FUTURE", Quantity(1)}, {"DIAG_EQUITY", Quantity(1)}}, *frozen.value());
    const std::string strict_logs = ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(strict.is_ok());
    auto original_positions = diagnostic_positions();
    ::testing::internal::CaptureStdout();
    auto model = model_manager.process_positions(
        original_positions, frozen.value()->market_data_,
        {{"DIAG_FUTURE", 100.0}, {"DIAG_EQUITY", 100.0}});
    const std::string model_logs = ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(model.is_ok());
    expect_all_risk_fields(strict.value(), model.value());
    EXPECT_EQ(strict_logs, model_logs);
    EXPECT_NE(strict_logs.find("VAR_DEBUG: gate_sigma="), std::string::npos);
    EXPECT_DOUBLE_EQ(static_cast<double>(original_positions.at("DIAG_FUTURE").average_price),
                     100.0);
    EXPECT_DOUBLE_EQ(static_cast<double>(original_positions.at("DIAG_FUTURE").quantity), 1.0);
    Logger::reset_for_tests();
    Logger::instance().set_level(LogLevel::INFO);
}

TEST_F(RiskManagerExtendedTest, FrozenQuantityExtremesAndSignedMarksRemainFinite) {
    RiskManager mgr(diagnostic_config(1000.0));
    const auto times = frozen_times();
    auto values = frozen_valuations();
    values[0].mark = -1.0;
    values[0].price_multiplier = 1.0;
    values[1].mark = 0.0;
    auto snap = mgr.make_frozen_snapshot(times.back(), values, times, frozen_closes());
    ASSERT_TRUE(snap.is_ok());
    auto fractional = mgr.process_positions_frozen(
        {{"risk:0", Quantity(0.5)}, {"risk:1", Quantity(-0.25)}}, *snap.value());
    ASSERT_TRUE(fractional.is_ok());
    EXPECT_DOUBLE_EQ(fractional.value().gross_leverage, 0.5 / 1'000'000.0);
    EXPECT_DOUBLE_EQ(fractional.value().net_leverage, -0.5 / 1'000'000.0);
    auto high = mgr.process_positions_frozen(
        {{"risk:0", Quantity::from_raw(INT64_MAX)}, {"risk:1", Quantity(0)}}, *snap.value());
    ASSERT_TRUE(high.is_ok());
    EXPECT_NEAR(high.value().gross_leverage,
                static_cast<double>(Quantity::from_raw(INT64_MAX)) / 1'000'000.0, 1e-10);
    auto low = mgr.process_positions_frozen(
        {{"risk:0", Quantity::from_raw(INT64_MIN)}, {"risk:1", Quantity(0)}}, *snap.value());
    ASSERT_TRUE(low.is_ok());
    EXPECT_GT(low.value().net_leverage, 0.0);
    EXPECT_THROW(Quantity(std::numeric_limits<double>::infinity()), std::invalid_argument);
    EXPECT_THROW(Quantity(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
}

TEST_F(RiskManagerExtendedTest, FrozenRejectsScaledIntegerEndpointsAndEveryConsumedLimit) {
    RiskManager mgr(default_config());
    const auto times = frozen_times();
    auto closes = frozen_closes();
    closes[2].close = std::ldexp(1.0, 63) / 100000000.0;
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), frozen_valuations(), times, closes),
                          "close_price_range");
    closes[2].close = -std::ldexp(1.0, 63) / 100000000.0;
    expect_argument_error(mgr.make_frozen_snapshot(times.back(), frozen_valuations(), times, closes),
                          "close_price_range");
    auto snap = mgr.make_frozen_snapshot(times.back(), frozen_valuations(), times,
                                         frozen_closes());
    ASSERT_TRUE(snap.is_ok());
    const auto book = std::unordered_map<RiskCalculationId, Quantity>{
        {"risk:0", Quantity(1)}, {"risk:1", Quantity(1)}};
    auto cfg = default_config();
    cfg.capital = Quantity(0);
    mgr.config_ = cfg;
    expect_argument_error(mgr.process_positions_frozen(book, *snap.value()), "invalid_capital");
    for (const auto name : {"jump_risk_limit", "max_correlation", "max_gross_leverage",
                            "max_net_leverage"}) {
        cfg = default_config();
        if (std::string(name) == "jump_risk_limit") cfg.jump_risk_limit = 0.0;
        if (std::string(name) == "max_correlation") cfg.max_correlation = 0.0;
        if (std::string(name) == "max_gross_leverage") cfg.max_gross_leverage = 0.0;
        if (std::string(name) == "max_net_leverage") cfg.max_net_leverage = 0.0;
        mgr.config_ = cfg;
        expect_argument_error(mgr.process_positions_frozen(book, *snap.value()),
                              std::string("invalid_") + name);
    }
}

TEST_F(RiskManagerExtendedTest, FrozenModelDefaultsAndNumericalFieldsBeforeExtraction) {
    ScopedDiagnosticInstruments instruments;
    RiskManager mgr(diagnostic_config(0.15));
    const MarketData data = diagnostic_market_data();
    const RiskResult defaults{};
    EXPECT_TRUE(mgr.process_positions({}, data).is_ok());
    expect_all_risk_fields(mgr.process_positions({}, data).value(), defaults);
    MarketData missing = data;
    missing.returns.clear();
    expect_all_risk_fields(mgr.process_positions(diagnostic_positions(), missing).value(), defaults);
    missing = data;
    missing.covariance.clear();
    expect_all_risk_fields(mgr.process_positions(diagnostic_positions(), missing).value(), defaults);
    missing = data;
    missing.symbol_indices.clear();
    expect_all_risk_fields(mgr.process_positions(diagnostic_positions(), missing).value(), defaults);
    missing = data;
    missing.ordered_symbols.clear();
    expect_all_risk_fields(mgr.process_positions(diagnostic_positions(), missing).value(), defaults);
    expect_all_risk_fields(mgr.process_positions(single_position("UNKNOWN", 1.0, 50.0), data).value(), defaults);

    RiskResult expected{};
    expected.portfolio_var_gate = std::sqrt(4.16 / 121.0);
    expected.portfolio_var = std::sqrt(0.05);
    expected.gross_leverage = 0.0011;
    expected.net_leverage = 0.0011;
    expected.portfolio_multiplier = 0.15 / expected.portfolio_var_gate;
    expected.recommended_scale = expected.portfolio_multiplier;
    expected.risk_exceeded = true;
    expect_all_risk_fields(mgr.process_positions(diagnostic_positions(), data).value(), expected);
}

TEST_F(RiskManagerExtendedTest, ModelVariantRootsPartialMarksAndHistoryKeepAllFields) {
    ScopedDiagnosticInstruments instruments;
    RiskManager mgr(diagnostic_config(0.15));
    MarketData data;
    data.ordered_symbols = {"DIAG_FUTURE.v.0", "DIAG_FUTURE.c.0"};
    data.symbol_indices = {{data.ordered_symbols[0], 0}, {data.ordered_symbols[1], 1}};
    data.returns = {{0.01, -0.02}, {-0.01, 0.02}};
    data.covariance = {{0.04, 0.0}, {0.0, 0.16}};
    std::unordered_map<std::string, Position> positions;
    positions.emplace(data.ordered_symbols[0], Position(data.ordered_symbols[0], Quantity(1),
                             Price(100), Decimal(0), Decimal(0), Timestamp{}));
    positions.emplace(data.ordered_symbols[1], Position(data.ordered_symbols[1], Quantity(1),
                             Price(50), Decimal(0), Decimal(0), Timestamp{}));
    const auto partial = mgr.process_positions(positions, data, {{data.ordered_symbols[0], 200.0}});
    ASSERT_TRUE(partial.is_ok());
    RiskResult expected{};
    expected.portfolio_var_gate = std::sqrt(0.032);
    expected.portfolio_var = expected.portfolio_var_gate;
    expected.jump_risk = 0.012;
    expected.gross_leverage = 0.0025;
    expected.net_leverage = 0.0025;
    expected.max_portfolio_risk = 0.004;
    expected.max_jump_risk = 0.012;
    expected.max_leverage_risk = 0.01 / 1'000'000.0;
    expected.portfolio_multiplier = 0.15 / expected.portfolio_var_gate;
    expected.recommended_scale = expected.portfolio_multiplier;
    expected.risk_exceeded = true;
    expect_all_risk_fields_near(partial.value(), expected);

    const auto full = mgr.process_positions(positions, data,
                                           {{data.ordered_symbols[0], 200.0},
                                            {data.ordered_symbols[1], 100.0}});
    ASSERT_TRUE(full.is_ok());
    EXPECT_DOUBLE_EQ(full.value().gross_leverage, 0.003);
    const auto empty = mgr.process_positions(positions, data);
    ASSERT_TRUE(empty.is_ok());
    EXPECT_DOUBLE_EQ(empty.value().gross_leverage, 0.0015);
    EXPECT_DOUBLE_EQ(full.value().portfolio_var_gate, empty.value().portfolio_var_gate);
}

TEST_F(RiskManagerExtendedTest, ModelAliasIndexKeepsGrossAndOverwriteBehavior) {
    ScopedDiagnosticInstruments instruments;
    RiskManager mgr(diagnostic_config(0.15));
    MarketData data = diagnostic_market_data();
    data.symbol_indices["DIAG_EQUITY"] = 0;
    data.returns = {{0.01, 0.02}, {-0.01, -0.02}};
    auto positions = diagnostic_positions();
    positions.at("DIAG_EQUITY").quantity = Quantity(10);
    auto result = mgr.process_positions(positions, data);
    ASSERT_TRUE(result.is_ok());
    // Both values are 1000. The second map iteration overwrites index zero,
    // while the gross still accumulates both values.
    EXPECT_DOUBLE_EQ(result.value().gross_leverage, 0.002);
    EXPECT_DOUBLE_EQ(result.value().net_leverage, 0.001);
    EXPECT_NEAR(result.value().portfolio_var_gate, 0.1, 1e-12);
    EXPECT_NEAR(result.value().portfolio_var, 0.2, 1e-12);
    EXPECT_NEAR(result.value().max_leverage_risk, 0.03 / 1'000'000.0, 1e-18);
    data.symbol_indices["DIAG_EQUITY"] = 100;
    auto out_of_range = mgr.process_positions(positions, data);
    ASSERT_TRUE(out_of_range.is_ok());
    EXPECT_DOUBLE_EQ(out_of_range.value().gross_leverage, 0.001);
}

TEST_F(RiskManagerExtendedTest, ModelSameBareRootCollisionUsesGenericEquityForBothVariants) {
    ScopedSameRootInstruments instruments;
    auto& registry = InstrumentRegistry::instance();
    ASSERT_TRUE(registry.get_futures_instrument(ScopedSameRootInstruments::root));
    ASSERT_TRUE(registry.get_equity_instrument(ScopedSameRootInstruments::root));
    ASSERT_TRUE(registry.get_instrument(ScopedSameRootInstruments::root));
    EXPECT_EQ(registry.get_futures_instrument(ScopedSameRootInstruments::root)->get_type(),
              AssetType::FUTURE);
    EXPECT_DOUBLE_EQ(registry.get_futures_instrument(ScopedSameRootInstruments::root)->get_multiplier(),
                     10.0);
    EXPECT_EQ(registry.get_instrument(ScopedSameRootInstruments::root)->get_type(), AssetType::EQUITY);
    EXPECT_DOUBLE_EQ(registry.get_instrument(ScopedSameRootInstruments::root)->get_multiplier(), 1.0);

    RiskManager mgr(diagnostic_config(0.15));
    const auto result = mgr.process_positions(same_root_positions(), same_root_market_data());
    ASSERT_TRUE(result.is_ok());
    RiskResult expected{};
    expected.portfolio_var_gate = std::sqrt(0.04 * 4.0 / 9.0 + 0.16 / 9.0);
    expected.portfolio_var = expected.portfolio_var_gate;
    expected.jump_risk = 0.01 * 2.0 / 3.0 + 0.02 / 3.0;
    expected.gross_leverage = 150.0 / 1'000'000.0;
    expected.net_leverage = expected.gross_leverage;
    expected.max_jump_risk = expected.jump_risk;
    expected.max_leverage_risk = 0.01 / 1'000'000.0;
    expected.portfolio_multiplier = 0.15 / expected.portfolio_var_gate;
    expected.recommended_scale = expected.portfolio_multiplier;
    expected.risk_exceeded = true;
    expect_all_risk_fields_near(result.value(), expected);
}

TEST_F(RiskManagerExtendedTest, ModelParityDigestAcrossLegacyPreparationAndBindingBranches) {
    Logger::reset_for_tests();
    ScopedDiagnosticInstruments instruments;
    ScopedSameRootInstruments collision_instruments;
    auto data = diagnostic_market_data();
    data.returns = {{0.01, 0.02}, {-0.01, -0.02}};
    auto positions = diagnostic_positions();
    auto record = [](const char* name, RiskManager& mgr,
                     const std::unordered_map<std::string, Position>& book,
                     const MarketData& md,
                     const std::unordered_map<std::string, double>& marks = {}) {
        ::testing::internal::CaptureStderr();
        auto result = mgr.process_positions(book, md, marks);
        const std::string logs = ::testing::internal::GetCapturedStderr();
        ASSERT_TRUE(result.is_ok()) << name;
        const auto& r = result.value();
        std::ostringstream values;
        values << std::hexfloat << r.recommended_scale << ',' << r.portfolio_var << ','
               << r.jump_risk << ',' << r.correlation_risk << ',' << r.gross_leverage << ','
               << r.net_leverage << ',' << r.max_portfolio_risk << ',' << r.max_jump_risk << ','
               << r.max_leverage_risk << ',' << r.portfolio_multiplier << ','
               << r.jump_multiplier << ',' << r.correlation_multiplier << ','
               << r.leverage_multiplier << ',' << r.portfolio_var_gate << ','
               << r.risk_exceeded;
        std::cout << "MODEL_PARITY|" << name << "|" << values.str() << '\n';
        std::istringstream lines(logs);
        std::string line;
        while (std::getline(lines, line)) {
            std::cout << "MODEL_LOG|" << name << "|" << line << '\n';
        }
    };
    RiskManager normal(diagnostic_config(0.15));
    record("empty_book", normal, {}, data);
    auto missing = data;
    missing.returns.clear();
    record("empty_returns", normal, positions, missing);
    missing = data;
    missing.covariance.clear();
    record("empty_covariance", normal, positions, missing);
    missing = data;
    missing.symbol_indices.clear();
    record("empty_indices", normal, positions, missing);
    missing = data;
    missing.ordered_symbols.clear();
    record("empty_symbols", normal, positions, missing);
    record("unmapped", normal, single_position("UNKNOWN", 1.0, 100.0), data);
    record("basis", normal, positions, data);
    record("partial_marks", normal, positions, data, {{"DIAG_FUTURE", 200.0}});
    record("full_marks", normal, positions, data,
           {{"DIAG_FUTURE", 200.0}, {"DIAG_EQUITY", 50.0}});
    auto negative = positions;
    negative.at("DIAG_FUTURE").quantity = Quantity(-1);
    record("negative", normal, negative, data);
    auto offset = positions;
    offset.at("DIAG_EQUITY").quantity = Quantity(-10);
    record("offset", normal, offset, data);
    auto zero = positions;
    zero.at("DIAG_FUTURE").quantity = Quantity(0);
    zero.at("DIAG_EQUITY").quantity = Quantity(0);
    record("zero", normal, zero, data);
    auto variant = data;
    variant.ordered_symbols = {"DIAG_FUTURE.v.0", "DIAG_FUTURE.c.0"};
    variant.symbol_indices = {{variant.ordered_symbols[0], 0}, {variant.ordered_symbols[1], 1}};
    auto variant_book = single_position(variant.ordered_symbols[0], 1.0, 100.0);
    variant_book.emplace(variant.ordered_symbols[1],
        Position(variant.ordered_symbols[1], Quantity(1), Price(50), Decimal(0),
                 Decimal(0), Timestamp{}));
    record("variant_roots", normal, variant_book, variant);
    Logger::instance().set_level(LogLevel::DEBUG);
    record("same_bare_root_equity_future_collision", normal, same_root_positions(),
           same_root_market_data());
    Logger::instance().set_level(LogLevel::INFO);
    auto absent = data;
    absent.ordered_symbols[0] = "ABSENT";
    absent.symbol_indices.erase("DIAG_FUTURE");
    absent.symbol_indices["ABSENT"] = 0;
    record("absent_instrument", normal, single_position("ABSENT", 1.0, 100.0), absent);
    auto alias = data;
    alias.symbol_indices["DIAG_EQUITY"] = 0;
    auto equal_values = positions;
    equal_values.at("DIAG_EQUITY").quantity = Quantity(10);
    record("alias_index", normal, equal_values, alias);
    alias.symbol_indices["DIAG_EQUITY"] = 99;
    record("out_of_range", normal, equal_values, alias);
    RiskManager var_binding(diagnostic_config(0.01));
    record("var_heavy", var_binding, positions, data);
    auto jump_cfg = diagnostic_config(100.0);
    jump_cfg.jump_risk_limit = 0.001;
    RiskManager jump_binding(jump_cfg);
    record("jump_binding", jump_binding, positions, data);
    auto correlated = data;
    correlated.covariance = {{0.04, 0.08}, {0.08, 0.16}};
    auto corr_cfg = diagnostic_config(100.0);
    corr_cfg.max_correlation = 0.2;
    RiskManager corr_binding(corr_cfg);
    record("corr_binding", corr_binding, positions, correlated);
    auto lev_cfg = diagnostic_config(100.0);
    lev_cfg.max_gross_leverage = 0.0001;
    lev_cfg.max_net_leverage = 0.0001;
    RiskManager lev_binding(lev_cfg);
    record("leverage_binding", lev_binding, positions, data);
    Logger::reset_for_tests();
}

TEST_F(RiskManagerExtendedTest, LegacyPositionalResultInitializationKeepsOriginalFieldOrder) {
    RiskResult result{false, 1.0, 0.2, 0.3};
    EXPECT_FALSE(result.risk_exceeded);
    EXPECT_DOUBLE_EQ(result.recommended_scale, 1.0);
    EXPECT_DOUBLE_EQ(result.portfolio_var, 0.2);
    EXPECT_DOUBLE_EQ(result.jump_risk, 0.3);
    EXPECT_DOUBLE_EQ(result.portfolio_var_gate, 0.0);
}

TEST_F(RiskManagerExtendedTest, CharacterizesDistinctGateAndReportVolatility) {
    ScopedDiagnosticInstruments instruments;
    RiskManager mgr(diagnostic_config(0.15));
    auto r = mgr.process_positions(diagnostic_positions(), diagnostic_market_data());
    ASSERT_TRUE(r.is_ok());

    // Dollar notional weights are 10/11 and 1/11. Report weights are 1/2 each.
    const double gate = std::sqrt(4.16 / 121.0);
    EXPECT_NEAR(r.value().portfolio_var_gate, gate, 1e-12);
    EXPECT_NEAR(r.value().portfolio_var, std::sqrt(0.05), 1e-12);
    EXPECT_NEAR(r.value().portfolio_multiplier, 0.15 / gate, 1e-12);
    EXPECT_NEAR(r.value().recommended_scale, 0.15 / gate, 1e-12);
    EXPECT_TRUE(r.value().risk_exceeded);
}

TEST_F(RiskManagerExtendedTest, GateValueSurvivesNonbindingLimit) {
    ScopedDiagnosticInstruments instruments;
    RiskManager mgr(diagnostic_config(0.5));
    auto r = mgr.process_positions(diagnostic_positions(), diagnostic_market_data());
    ASSERT_TRUE(r.is_ok());
    EXPECT_NEAR(r.value().portfolio_var_gate, std::sqrt(4.16 / 121.0), 1e-12);
    EXPECT_NEAR(r.value().portfolio_var, std::sqrt(0.05), 1e-12);
    EXPECT_DOUBLE_EQ(r.value().portfolio_multiplier, 1.0);
    EXPECT_DOUBLE_EQ(r.value().recommended_scale, 1.0);
    EXPECT_FALSE(r.value().risk_exceeded);
}

TEST_F(RiskManagerExtendedTest, CurrentPriceMarksFeedGateAndReportValues) {
    ScopedDiagnosticInstruments instruments;
    RiskManager mgr(diagnostic_config(0.5));
    auto r = mgr.process_positions(diagnostic_positions(), diagnostic_market_data(),
                                   {{"DIAG_FUTURE", 200.0}, {"DIAG_EQUITY", 100.0}});
    ASSERT_TRUE(r.is_ok());
    EXPECT_NEAR(r.value().portfolio_var_gate, std::sqrt(16.16 / 441.0), 1e-12);
    EXPECT_NEAR(r.value().portfolio_var, std::sqrt(0.32 / 9.0), 1e-12);
    EXPECT_DOUBLE_EQ(r.value().gross_leverage, 0.0021);
}

TEST_F(RiskManagerExtendedTest, ZeroValueBookHasZeroGateAndReportValues) {
    ScopedDiagnosticInstruments instruments;
    RiskManager mgr(diagnostic_config(0.15));
    auto positions = diagnostic_positions();
    positions.at("DIAG_FUTURE").quantity = Quantity(0.0);
    positions.at("DIAG_EQUITY").quantity = Quantity(0.0);
    auto r = mgr.process_positions(positions, diagnostic_market_data());
    ASSERT_TRUE(r.is_ok());
    EXPECT_DOUBLE_EQ(r.value().portfolio_var_gate, 0.0);
    EXPECT_DOUBLE_EQ(r.value().portfolio_var, 0.0);
    EXPECT_DOUBLE_EQ(r.value().portfolio_multiplier, 1.0);
}

// ===== process_positions early-return branches =====

TEST_F(RiskManagerExtendedTest, ProcessPositionsEmptyPositionsReturnsDefaultResult) {
    RiskManager mgr(default_config());
    MarketData md;
    auto r = mgr.process_positions({}, md);
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(r.value().risk_exceeded);
    EXPECT_DOUBLE_EQ(r.value().recommended_scale, 1.0);
    EXPECT_DOUBLE_EQ(r.value().portfolio_multiplier, 1.0);
    EXPECT_DOUBLE_EQ(r.value().portfolio_var_gate, 0.0);
}

TEST_F(RiskManagerExtendedTest, ProcessPositionsEmptyMarketDataReturnsDefaultResult) {
    RiskManager mgr(default_config());
    MarketData md;  // all empty
    auto positions = single_position("ES", 1.0, 4000.0);
    auto r = mgr.process_positions(positions, md);
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(r.value().risk_exceeded);
    EXPECT_DOUBLE_EQ(r.value().recommended_scale, 1.0);
    EXPECT_DOUBLE_EQ(r.value().portfolio_var_gate, 0.0);
}

TEST_F(RiskManagerExtendedTest, ProcessPositionsNoSymbolsMappedReturnsDefaultResult) {
    RiskManager mgr(default_config());
    // Build a market_data via create_market_data so it has the structure populated,
    // but for symbols that don't match the positions.
    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars;
    for (int i = 0; i < 10; ++i) {
        bars.push_back(make_bar("AAPL", 100.0 + i, t0 + std::chrono::hours(24 * i)));
    }
    MarketData md = mgr.create_market_data(bars);
    auto r = mgr.process_positions(single_position("UNKNOWN", 1.0, 100.0), md);
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(r.value().risk_exceeded);
    EXPECT_DOUBLE_EQ(r.value().portfolio_var_gate, 0.0);
}

// ===== update_config =====

TEST_F(RiskManagerExtendedTest, UpdateConfigReplacesConfigInPlace) {
    RiskManager mgr(default_config());
    EXPECT_DOUBLE_EQ(mgr.get_config().var_limit, 0.15);

    RiskConfig new_cfg = default_config();
    new_cfg.var_limit = 0.20;
    new_cfg.max_correlation = 0.8;
    auto r = mgr.update_config(new_cfg);
    ASSERT_TRUE(r.is_ok());
    EXPECT_DOUBLE_EQ(mgr.get_config().var_limit, 0.20);
    EXPECT_DOUBLE_EQ(mgr.get_config().max_correlation, 0.8);
}

// ===== create_market_data =====

TEST_F(RiskManagerExtendedTest, CreateMarketDataEmptyBarsProducesEmptyStructures) {
    RiskManager mgr(default_config());
    auto md = mgr.create_market_data({});
    EXPECT_TRUE(md.returns.empty());
    EXPECT_TRUE(md.covariance.empty());
    EXPECT_TRUE(md.symbol_indices.empty());
    EXPECT_TRUE(md.ordered_symbols.empty());
}

TEST_F(RiskManagerExtendedTest, CreateMarketDataSingleSymbolAggregatesBars) {
    RiskManager mgr(default_config());
    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars;
    for (int i = 0; i < 30; ++i) {
        bars.push_back(make_bar("AAPL", 100.0 + i, t0 + std::chrono::hours(24 * i)));
    }
    auto md = mgr.create_market_data(bars);
    EXPECT_EQ(md.ordered_symbols.size(), 1u);
    EXPECT_EQ(md.symbol_indices.count("AAPL"), 1u);
    EXPECT_FALSE(md.returns.empty());
}

TEST_F(RiskManagerExtendedTest, CreateMarketDataMultipleSymbolsBuildsCovariance) {
    RiskManager mgr(default_config());
    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars;
    for (int i = 0; i < 30; ++i) {
        auto ts = t0 + std::chrono::hours(24 * i);
        bars.push_back(make_bar("AAPL", 100.0 + i * 0.5, ts));
        bars.push_back(make_bar("MSFT", 200.0 + i * 0.3, ts));
    }
    auto md = mgr.create_market_data(bars);
    EXPECT_EQ(md.ordered_symbols.size(), 2u);
    ASSERT_FALSE(md.covariance.empty());
    EXPECT_EQ(md.covariance.size(), 2u);
    EXPECT_EQ(md.covariance[0].size(), 2u);
}

// ===== calculate_99th_percentile =====

TEST_F(RiskManagerExtendedTest, Percentile99EmptyReturnsZero) {
    RiskManager mgr(default_config());
    EXPECT_DOUBLE_EQ(mgr.calculate_99th_percentile({}), 0.0);
}

TEST_F(RiskManagerExtendedTest, Percentile99SingleValueReturnsThatValue) {
    RiskManager mgr(default_config());
    EXPECT_DOUBLE_EQ(mgr.calculate_99th_percentile({0.05}), 0.05);
}

TEST_F(RiskManagerExtendedTest, Percentile99HundredValuesReturnsTopValue) {
    RiskManager mgr(default_config());
    std::vector<double> values;
    for (int i = 0; i < 100; ++i) values.push_back(i / 100.0);
    double p = mgr.calculate_99th_percentile(values);
    // 99th percentile should be near the top (0.99)
    EXPECT_GT(p, 0.95);
    EXPECT_LE(p, 0.99);
}

// ===== calculate_var =====

TEST_F(RiskManagerExtendedTest, CalculateVarReturnsFiniteForReasonableInputs) {
    RiskManager mgr(default_config());
    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars;
    for (int i = 0; i < 30; ++i) {
        bars.push_back(make_bar("AAPL", 100.0 + std::sin(i * 0.5) * 5.0,
                                  t0 + std::chrono::hours(24 * i)));
    }
    auto md = mgr.create_market_data(bars);
    auto positions = single_position("AAPL", 100.0, 100.0);
    auto r = mgr.process_positions(positions, md);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(std::isfinite(r.value().portfolio_var));
    EXPECT_GE(r.value().portfolio_var, 0.0);
}

// ===== calculate_weights (public-ish helper) =====

TEST_F(RiskManagerExtendedTest, CalculateWeightsZeroPositionsReturnsEmpty) {
    RiskManager mgr(default_config());
    auto w = mgr.calculate_weights({});
    EXPECT_TRUE(w.empty());
}

TEST_F(RiskManagerExtendedTest, CalculateWeightsSinglePositionReturnsUnitWeight) {
    RiskManager mgr(default_config());
    auto pos = single_position("ES", 5.0, 100.0);
    auto w = mgr.calculate_weights(pos);
    ASSERT_EQ(w.size(), 1u);
    EXPECT_NEAR(std::abs(w[0]), 1.0, 1e-9);
}

TEST_F(RiskManagerExtendedTest, CalculateWeightsZeroValuePositionsReturnsZeroVector) {
    RiskManager mgr(default_config());
    auto pos = single_position("ES", 0.0, 100.0);
    auto w = mgr.calculate_weights(pos);
    ASSERT_EQ(w.size(), 1u);
    EXPECT_DOUBLE_EQ(w[0], 0.0);
}

// ===== Current price override path =====

TEST_F(RiskManagerExtendedTest, ProcessPositionsUsesCurrentPriceWhenProvided) {
    RiskManager mgr(default_config());
    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars;
    for (int i = 0; i < 30; ++i) {
        bars.push_back(make_bar("AAPL", 100.0, t0 + std::chrono::hours(24 * i)));
    }
    auto md = mgr.create_market_data(bars);
    auto positions = single_position("AAPL", 10.0, 100.0);

    // Without current price → uses average_price
    auto r1 = mgr.process_positions(positions, md);
    ASSERT_TRUE(r1.is_ok());

    // With current price (much higher) → leverage calculation differs
    std::unordered_map<std::string, double> prices = {{"AAPL", 200.0}};
    auto r2 = mgr.process_positions(positions, md, prices);
    ASSERT_TRUE(r2.is_ok());
    EXPECT_GE(r2.value().gross_leverage, r1.value().gross_leverage);
}

// ===== RiskResult fields populated =====

TEST_F(RiskManagerExtendedTest, ResultFieldsPopulatedWhenPositionsProcessed) {
    RiskManager mgr(default_config());
    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars;
    for (int i = 0; i < 30; ++i) {
        bars.push_back(make_bar("AAPL", 100.0 + i * 0.1, t0 + std::chrono::hours(24 * i)));
    }
    auto md = mgr.create_market_data(bars);
    auto positions = single_position("AAPL", 100.0, 100.0);
    auto r = mgr.process_positions(positions, md);
    ASSERT_TRUE(r.is_ok());
    auto& res = r.value();
    EXPECT_TRUE(std::isfinite(res.portfolio_multiplier));
    EXPECT_TRUE(std::isfinite(res.jump_multiplier));
    EXPECT_TRUE(std::isfinite(res.correlation_multiplier));
    EXPECT_TRUE(std::isfinite(res.leverage_multiplier));
    EXPECT_TRUE(std::isfinite(res.recommended_scale));
    EXPECT_TRUE(std::isfinite(res.gross_leverage));
    EXPECT_TRUE(std::isfinite(res.net_leverage));
}

TEST_F(RiskManagerExtendedTest, RecommendedScaleIsMinimumOfMultipliers) {
    RiskManager mgr(default_config());
    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars;
    for (int i = 0; i < 30; ++i) {
        bars.push_back(make_bar("AAPL", 100.0 + std::sin(i * 0.3) * 10.0,
                                  t0 + std::chrono::hours(24 * i)));
    }
    auto md = mgr.create_market_data(bars);
    auto positions = single_position("AAPL", 1000.0, 100.0);  // large position to stress limits
    auto r = mgr.process_positions(positions, md);
    ASSERT_TRUE(r.is_ok());
    auto& res = r.value();
    double min_mult = std::min({res.portfolio_multiplier, res.jump_multiplier,
                                 res.correlation_multiplier, res.leverage_multiplier});
    EXPECT_DOUBLE_EQ(res.recommended_scale, min_mult);
    EXPECT_EQ(res.risk_exceeded, res.recommended_scale < 1.0);
}
