// Targets branches in risk_manager.cpp not exercised by the existing
// test_risk_manager.cpp suite: process_positions early returns, update_config,
// create_market_data edge cases, calculate_99th_percentile, and the
// catch-all error path.

#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
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
#include "../core/test_base.hpp"

#define private public
#include "trade_ngin/risk/risk_manager.hpp"
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

}  // namespace

class RiskManagerExtendedTest : public TestBase {};

// ===== process_positions early-return branches =====

TEST_F(RiskManagerExtendedTest, ProcessPositionsEmptyPositionsReturnsDefaultResult) {
    RiskManager mgr(default_config());
    MarketData md;
    auto r = mgr.process_positions({}, md);
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(r.value().risk_exceeded);
    EXPECT_DOUBLE_EQ(r.value().recommended_scale, 1.0);
    EXPECT_DOUBLE_EQ(r.value().portfolio_multiplier, 1.0);
}

TEST_F(RiskManagerExtendedTest, ProcessPositionsEmptyMarketDataReturnsDefaultResult) {
    RiskManager mgr(default_config());
    MarketData md;  // all empty
    auto positions = single_position("ES", 1.0, 4000.0);
    auto r = mgr.process_positions(positions, md);
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(r.value().risk_exceeded);
    EXPECT_DOUBLE_EQ(r.value().recommended_scale, 1.0);
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
