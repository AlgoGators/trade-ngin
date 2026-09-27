#include "trade_ngin/portfolio/component_risk_evaluator.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace trade_ngin {
namespace {

const auto t0 = Timestamp{} + std::chrono::hours(24);
const auto t1 = t0 + std::chrono::hours(24);
const auto t2 = t1 + std::chrono::hours(24);

ComponentPositionKey key(const std::string& strategy, const std::string& symbol = "ES") {
    return {"portfolio-a", strategy, "desk", "2026-09-23", symbol, "QT"};
}

Position prior(const std::string& symbol, int quantity) {
    return {symbol, Quantity(quantity), Price(82), Decimal(7), Decimal(-3), t0};
}

ComponentBookContext two_component_context() {
    return {"portfolio-a", "2026-09-23", "QT", "revision-1",
            {{key("b"), {AssetType::FUTURE, "ES"}, false, prior("ES", 5)},
             {key("a"), {AssetType::FUTURE, "ES"}, true, prior("ES", 8)}}};
}

ComponentBookProposal proposal(std::vector<ComponentQuantityEntry> quantities = {}) {
    return {"portfolio-a", "2026-09-23", "QT", "revision-1", std::move(quantities)};
}

ComponentRiskInputs inputs_for(const std::vector<InstrumentIdentity>& instruments) {
    ComponentRiskInputs inputs{"portfolio-a", "2026-09-23", "QT", "revision-1",
                                "snapshot-a", t2, "USD", {}, {t0, t1, t2}, {}};
    for (const auto& identity : instruments) {
        inputs.valuations.push_back({identity, t2, 100.0, 1.0, "USD"});
        for (const auto time : inputs.expected_observation_times) {
            inputs.closes.push_back({identity, time, 100.0});
        }
    }
    return inputs;
}

RiskConfig config() {
    RiskConfig config;
    config.capital = Decimal(1000);
    config.var_limit = 100;
    config.jump_risk_limit = 100;
    config.max_correlation = 1;
    config.max_gross_leverage = 0.5;
    config.max_net_leverage = 100;
    return config;
}

void expect_error(const Result<ComponentRiskEvaluation>& result, ErrorCode code,
                  const std::string& prefix, const std::string& component) {
    ASSERT_TRUE(result.is_error());
    ASSERT_NE(result.error(), nullptr);
    EXPECT_EQ(result.error()->code(), code);
    EXPECT_EQ(std::string(result.error()->what()).find(prefix), 0u)
        << result.error()->what();
    EXPECT_EQ(result.error()->component(), component);
}

TEST(ComponentRiskEvaluatorTest, BreachEvaluatesCompleteCandidateWithoutChangingComponents) {
    auto context = two_component_context();
    auto edit = proposal({{key("a"), Quantity(10)}});
    auto inputs = inputs_for({{AssetType::FUTURE, "ES"}});
    ComponentRiskEvaluator evaluator(config());

    auto result = evaluator.evaluate(context, edit, inputs);

    ASSERT_TRUE(result.is_ok()) << (result.error() ? result.error()->what() : "");
    const auto& out = result.value();
    ASSERT_EQ(out.evaluated_book.components.size(), 2u);
    EXPECT_EQ(out.evaluated_book.components[0].key, key("a"));
    EXPECT_EQ(out.evaluated_book.components[0].position.quantity.raw_value(), Quantity(10).raw_value());
    EXPECT_EQ(out.evaluated_book.components[0].position.average_price, Price(82));
    EXPECT_EQ(out.evaluated_book.components[0].position.unrealized_pnl, Decimal(7));
    EXPECT_EQ(out.evaluated_book.components[0].position.realized_pnl, Decimal(-3));
    EXPECT_EQ(out.evaluated_book.components[0].position.last_update, t0);
    EXPECT_TRUE(out.evaluated_book.components[0].editable);
    EXPECT_EQ(out.evaluated_book.components[1].key, key("b"));
    EXPECT_EQ(out.evaluated_book.components[1].position.quantity.raw_value(), Quantity(5).raw_value());
    EXPECT_EQ(out.evaluated_book.components[1].position.average_price, Price(82));
    EXPECT_EQ(out.evaluated_book.components[1].position.unrealized_pnl, Decimal(7));
    EXPECT_EQ(out.evaluated_book.components[1].position.realized_pnl, Decimal(-3));
    EXPECT_EQ(out.evaluated_book.components[1].position.last_update, t0);
    EXPECT_FALSE(out.evaluated_book.components[1].editable);
    ASSERT_EQ(out.evaluated_book.instruments.size(), 1u);
    EXPECT_EQ(out.evaluated_book.instruments[0].net_quantity.raw_value(), Quantity(15).raw_value());
    EXPECT_EQ(out.evaluated_book.instruments[0].members,
              (std::vector<ComponentPositionKey>{key("a"), key("b")}));
    ASSERT_EQ(out.bindings.size(), 1u);
    EXPECT_EQ(out.bindings[0].instrument, (InstrumentIdentity{AssetType::FUTURE, "ES"}));
    EXPECT_EQ(out.bindings[0].calculation_id, "risk:0");
    EXPECT_DOUBLE_EQ(out.risk.gross_leverage, 1.5);
    EXPECT_TRUE(out.risk.risk_exceeded);
    EXPECT_NEAR(out.risk.recommended_scale, 1.0 / 3.0, 1e-12);
    EXPECT_EQ(out.evaluated_proposal.quantities[0].quantity.raw_value(), Quantity(10).raw_value());
    EXPECT_EQ(out.evaluated_inputs.market_snapshot_id, "snapshot-a");
    EXPECT_EQ(out.evaluated_config.capital, Decimal(1000));
}

TEST(ComponentRiskEvaluatorTest, SameSymbolDifferentTypesHaveDistinctBindingAndOracleMetrics) {
    ComponentBookContext context{"portfolio-a", "2026-09-23", "QT", "revision-1",
                                 {{key("future", "XYZ"), {AssetType::FUTURE, "XYZ"}, false,
                                   prior("XYZ", 1)},
                                  {key("equity", "XYZ"), {AssetType::EQUITY, "XYZ"}, false,
                                   prior("XYZ", 1)}}};
    auto market = inputs_for({{AssetType::FUTURE, "XYZ"}, {AssetType::EQUITY, "XYZ"}});
    market.valuations[0].price_multiplier = 10;
    for (auto& row : market.closes) {
        if (row.instrument.type == AssetType::FUTURE) {
            row.close = row.timestamp == t1 ? 101 : 100;
        } else {
            row.close = row.timestamp == t1 ? 204 : 200;
        }
    }
    auto cfg = config();
    cfg.capital = Decimal(1'000'000);
    cfg.var_limit = 0.15;
    cfg.max_gross_leverage = 100;
    ComponentRiskEvaluator evaluator(cfg);
    auto result = evaluator.evaluate(context, proposal(), market);
    ASSERT_TRUE(result.is_ok()) << (result.error() ? result.error()->what() : "");
    const auto& out = result.value();
    ASSERT_EQ(out.bindings.size(), 2u);
    EXPECT_EQ(out.bindings[0].instrument, (InstrumentIdentity{AssetType::FUTURE, "XYZ"}));
    EXPECT_EQ(out.bindings[1].instrument, (InstrumentIdentity{AssetType::EQUITY, "XYZ"}));
    EXPECT_NE(out.bindings[0].calculation_id, out.bindings[1].calculation_id);
    EXPECT_DOUBLE_EQ(out.risk.gross_leverage, 0.0011);
    const double da = 1.0 / 100.0 + 1.0 / 101.0;
    const double db = 1.0 / 50.0 + 1.0 / 51.0;
    const double gate = std::sqrt(126.0) * (10.0 * da + db) / 11.0;
    const double report = std::sqrt(126.0) * (da + db) / 2.0;
    EXPECT_NEAR(out.risk.portfolio_var_gate, gate, 1e-12);
    EXPECT_NEAR(out.risk.portfolio_var, report, 1e-12);
    EXPECT_NEAR(out.risk.portfolio_multiplier, 0.15 / gate, 1e-12);
    EXPECT_EQ(out.evaluated_inputs.valuations[0].instrument,
              (InstrumentIdentity{AssetType::FUTURE, "XYZ"}));
    EXPECT_EQ(out.evaluated_inputs.valuations[1].instrument,
              (InstrumentIdentity{AssetType::EQUITY, "XYZ"}));
}

TEST(ComponentRiskEvaluatorTest, TwelveBindingsKeepLexicalIdOrderingFromMixingInputs) {
    ComponentBookContext context{"portfolio-a", "2026-09-23", "QT", "revision-1", {}};
    std::vector<InstrumentIdentity> identities;
    for (int i = 0; i < 12; ++i) {
        const std::string symbol = std::string("SYM") + (i < 10 ? "0" : "") + std::to_string(i);
        context.slots.push_back({key(symbol, symbol), {AssetType::EQUITY, symbol},
                                 false, prior(symbol, i + 1)});
        identities.push_back({AssetType::EQUITY, symbol});
    }
    auto market = inputs_for(identities);
    for (int i = 0; i < 12; ++i) {
        market.valuations[i].mark = 100 + i;
        market.valuations[i].price_multiplier = i + 1;
        for (auto& close : market.closes) {
            if (close.instrument == identities[i]) {
                close.close = i == 10 && close.timestamp == t1 ? 111 : 100 + i;
            }
        }
    }
    std::reverse(market.valuations.begin(), market.valuations.end());
    std::reverse(market.closes.begin(), market.closes.end());
    const auto original_valuations = market.valuations;
    const auto original_closes = market.closes;
    auto cfg = config();
    cfg.capital = Decimal(1'000'000);
    cfg.max_gross_leverage = 100;
    ComponentRiskEvaluator evaluator(cfg);
    auto result = evaluator.evaluate(context, proposal(), market);
    ASSERT_TRUE(result.is_ok()) << (result.error() ? result.error()->what() : "");
    ASSERT_EQ(result.value().bindings.size(), 12u);
    for (int i = 0; i < 12; ++i) {
        EXPECT_EQ(result.value().bindings[i].calculation_id, "risk:" + std::to_string(i));
        EXPECT_EQ(result.value().bindings[i].instrument, identities[i]);
    }
    EXPECT_NEAR(result.value().risk.gross_leverage, 70434.0 / 1'000'000, 1e-12);
    const double swing = 1.0 / 110.0 + 1.0 / 111.0;
    EXPECT_NEAR(result.value().risk.portfolio_var_gate,
                std::sqrt(126.0) * 13310.0 / 70434.0 * swing, 1e-12);
    EXPECT_NEAR(result.value().risk.portfolio_var,
                std::sqrt(126.0) * 1210.0 / 8372.0 * swing, 1e-12);
    EXPECT_EQ(result.value().evaluated_inputs.valuations.front().instrument, identities.front());
    EXPECT_EQ(result.value().evaluated_inputs.closes.front().instrument, identities.front());
    EXPECT_EQ(result.value().evaluated_inputs.closes.front().timestamp, t0);
    EXPECT_EQ(market.valuations.front().instrument, original_valuations.front().instrument);
    EXPECT_EQ(market.closes.front().instrument, original_closes.front().instrument);
    std::reverse(market.valuations.begin(), market.valuations.end());
    std::reverse(market.closes.begin(), market.closes.end());
    auto repeated = evaluator.evaluate(context, proposal(), market);
    ASSERT_TRUE(repeated.is_ok());
    EXPECT_DOUBLE_EQ(repeated.value().risk.gross_leverage, result.value().risk.gross_leverage);
    EXPECT_EQ(repeated.value().evaluated_inputs.valuations.front().instrument,
              result.value().evaluated_inputs.valuations.front().instrument);
}

TEST(ComponentRiskEvaluatorTest, OffsettingAndEmptyProposalRetainMembersAndRequireMarket) {
    auto book = two_component_context();
    book.slots[0].previous = prior("ES", -5);
    auto market = inputs_for({{AssetType::FUTURE, "ES"}});
    ComponentRiskEvaluator evaluator(config());
    auto zero = evaluator.evaluate(book, proposal({{key("a"), Quantity(5)}}), market);
    ASSERT_TRUE(zero.is_ok()) << (zero.error() ? zero.error()->what() : "");
    EXPECT_EQ(zero.value().evaluated_book.instruments[0].net_quantity.raw_value(), 0);
    EXPECT_EQ(zero.value().evaluated_book.instruments[0].members.size(), 2u);
    EXPECT_DOUBLE_EQ(zero.value().risk.gross_leverage, 0);
    EXPECT_DOUBLE_EQ(zero.value().risk.recommended_scale, 1);
    auto missing = market;
    missing.valuations.clear();
    expect_error(evaluator.evaluate(book, proposal({{key("a"), Quantity(5)}}), missing),
                 ErrorCode::INVALID_ARGUMENT, "risk_valuation_coverage", "ComponentRiskEvaluator");
    missing = market;
    missing.closes.pop_back();
    expect_error(evaluator.evaluate(book, proposal({{key("a"), Quantity(5)}}), missing),
                 ErrorCode::INVALID_ARGUMENT, "missing_close", "RiskManager");
    missing = market;
    missing.valuations.front().quote_currency = "EUR";
    expect_error(evaluator.evaluate(book, proposal({{key("a"), Quantity(5)}}), missing),
                 ErrorCode::INVALID_ARGUMENT, "risk_currency_mismatch", "ComponentRiskEvaluator");
    auto empty = evaluator.evaluate(book, proposal(), market);
    ASSERT_TRUE(empty.is_ok());
    EXPECT_EQ(empty.value().evaluated_book.components[0].position.quantity.raw_value(), 0);
    EXPECT_EQ(empty.value().evaluated_book.components[1].position.quantity.raw_value(),
              Quantity(-5).raw_value());
    EXPECT_EQ(empty.value().evaluated_book.instruments[0].net_quantity.raw_value(),
              Quantity(-5).raw_value());
    book.slots[0].previous = prior("ES", 0);
    auto all_zero = evaluator.evaluate(book, proposal(), market);
    ASSERT_TRUE(all_zero.is_ok());
    EXPECT_EQ(all_zero.value().evaluated_book.components.size(), 2u);
    EXPECT_EQ(all_zero.value().evaluated_book.instruments[0].members.size(), 2u);
    EXPECT_EQ(all_zero.value().evaluated_book.instruments[0].net_quantity.raw_value(), 0);
    EXPECT_DOUBLE_EQ(all_zero.value().risk.gross_leverage, 0);
}

TEST(ComponentRiskEvaluatorTest, NewUnfilledSlotAndExistingZeroKeepDifferentMetadata) {
    auto book = two_component_context();
    book.slots = {{key("new", "NEW"), {AssetType::EQUITY, "NEW"}, true, std::nullopt},
                  {key("zero", "ZERO"), {AssetType::EQUITY, "ZERO"}, false,
                   prior("ZERO", 0)}};
    auto market = inputs_for({{AssetType::EQUITY, "NEW"}, {AssetType::EQUITY, "ZERO"}});
    ComponentRiskEvaluator evaluator(config());
    auto result = evaluator.evaluate(book, proposal({{key("new", "NEW"), Quantity(1)}}), market);
    ASSERT_TRUE(result.is_ok()) << (result.error() ? result.error()->what() : "");
    ASSERT_EQ(result.value().evaluated_book.components.size(), 2u);
    const auto& fresh = result.value().evaluated_book.components[0];
    EXPECT_TRUE(fresh.unfilled);
    EXPECT_FALSE(fresh.previous.has_value());
    EXPECT_EQ(fresh.position.average_price.raw_value(), 0);
    EXPECT_EQ(fresh.position.last_update, Timestamp{});
    const auto& existing = result.value().evaluated_book.components[1];
    EXPECT_FALSE(existing.unfilled);
    ASSERT_TRUE(existing.previous.has_value());
    EXPECT_EQ(existing.position.average_price, Price(82));
    EXPECT_EQ(existing.position.quantity.raw_value(), 0);
}

TEST(ComponentRiskEvaluatorTest, RejectsContextBindingAndForwardsOverlayErrors) {
    auto book = two_component_context();
    auto market = inputs_for({{AssetType::FUTURE, "ES"}});
    ComponentRiskEvaluator evaluator(config());
    for (int field = 0; field < 4; ++field) {
        auto bad = market;
        if (field == 0) bad.expected_portfolio_id = "other";
        if (field == 1) bad.expected_date = "2026-09-22";
        if (field == 2) bad.expected_portfolio_type = "SYSTEM";
        if (field == 3) bad.expected_revision = "stale";
        expect_error(evaluator.evaluate(book, proposal(), bad), ErrorCode::INVALID_ARGUMENT,
                     "risk_context_mismatch", "ComponentRiskEvaluator");
        const auto mismatch = evaluator.evaluate(book, proposal(), bad);
        ASSERT_TRUE(mismatch.is_error());
        EXPECT_NE(std::string(mismatch.error()->what()).find("revision-1"), std::string::npos);
        if (field == 3) {
            EXPECT_NE(std::string(mismatch.error()->what()).find("stale"), std::string::npos);
        }
    }
    auto stale = proposal();
    stale.expected_revision = "old";
    expect_error(evaluator.evaluate(book, stale, market), ErrorCode::INVALID_ARGUMENT,
                 "stale or mismatched proposal context", "component_book");
    expect_error(evaluator.evaluate(book, proposal({{key("b"), Quantity(3)}}), market),
                 ErrorCode::INVALID_ARGUMENT, "proposal key outside editable scope", "component_book");
    expect_error(evaluator.evaluate(book, proposal({{key("a"), Quantity(3)},
                                                   {key("a"), Quantity(4)}}), market),
                 ErrorCode::INVALID_ARGUMENT, "duplicate proposal component key", "component_book");
    book.slots[0].instrument.symbol = "BAD";
    expect_error(evaluator.evaluate(book, proposal(), market), ErrorCode::INVALID_DATA,
                 "inconsistent component slot", "component_book");
    book = two_component_context();
    book.slots.clear();
    expect_error(evaluator.evaluate(book, proposal(), market), ErrorCode::INVALID_ARGUMENT,
                 "empty_component_risk_book", "ComponentRiskEvaluator");
}

TEST(ComponentRiskEvaluatorTest, RejectsTypedCoverageProvenanceAsOfAndCurrency) {
    auto book = two_component_context();
    auto market = inputs_for({{AssetType::FUTURE, "ES"}});
    ComponentRiskEvaluator evaluator(config());
    auto bad = market;
    bad.market_snapshot_id.clear();
    expect_error(evaluator.evaluate(book, proposal(), bad), ErrorCode::INVALID_ARGUMENT,
                 "risk_market_snapshot_id", "ComponentRiskEvaluator");
    bad = market;
    bad.capital_currency = " ";
    expect_error(evaluator.evaluate(book, proposal(), bad), ErrorCode::INVALID_ARGUMENT,
                 "risk_capital_currency", "ComponentRiskEvaluator");
    bad = market;
    bad.valuations.push_back(bad.valuations.front());
    expect_error(evaluator.evaluate(book, proposal(), bad), ErrorCode::INVALID_ARGUMENT,
                 "duplicate_risk_valuation", "ComponentRiskEvaluator");
    bad = market;
    bad.valuations[0].instrument.type = AssetType::EQUITY;
    expect_error(evaluator.evaluate(book, proposal(), bad), ErrorCode::INVALID_ARGUMENT,
                 "risk_valuation_coverage", "ComponentRiskEvaluator");
    bad = market;
    bad.valuations.push_back({{AssetType::EQUITY, "ES"}, t2, 1, 1, "USD"});
    expect_error(evaluator.evaluate(book, proposal(), bad), ErrorCode::INVALID_ARGUMENT,
                 "risk_valuation_coverage", "ComponentRiskEvaluator");
    bad = market;
    bad.closes[0].instrument.type = AssetType::EQUITY;
    expect_error(evaluator.evaluate(book, proposal(), bad), ErrorCode::INVALID_ARGUMENT,
                 "risk_history_identity", "ComponentRiskEvaluator");
    bad = market;
    bad.valuations[0].mark_as_of = t1;
    expect_error(evaluator.evaluate(book, proposal(), bad), ErrorCode::INVALID_ARGUMENT,
                 "risk_mark_asof_mismatch", "ComponentRiskEvaluator");
    bad = market;
    bad.valuations[0].quote_currency = "EUR";
    expect_error(evaluator.evaluate(book, proposal(), bad), ErrorCode::INVALID_ARGUMENT,
                 "risk_currency_mismatch", "ComponentRiskEvaluator");
    bad = market;
    bad.valuations[0].quote_currency.clear();
    expect_error(evaluator.evaluate(book, proposal(), bad), ErrorCode::INVALID_ARGUMENT,
                 "risk_currency_mismatch", "ComponentRiskEvaluator");
}

TEST(ComponentRiskEvaluatorTest, ForwardsFrozenSnapshotAndStrictCalculationErrors) {
    auto book = two_component_context();
    auto market = inputs_for({{AssetType::FUTURE, "ES"}});
    ComponentRiskEvaluator evaluator(config());
    auto bad = market;
    bad.closes.pop_back();
    expect_error(evaluator.evaluate(book, proposal(), bad), ErrorCode::INVALID_ARGUMENT,
                 "missing_close", "RiskManager");
    bad = market;
    bad.closes.push_back(bad.closes.front());
    expect_error(evaluator.evaluate(book, proposal(), bad), ErrorCode::INVALID_ARGUMENT,
                 "duplicate_close", "RiskManager");
    bad = market;
    bad.expected_observation_times[1] = t2 + std::chrono::hours(24);
    expect_error(evaluator.evaluate(book, proposal(), bad), ErrorCode::INVALID_ARGUMENT,
                 "future_observation_time", "RiskManager");
    bad = market;
    bad.closes.back().close = 1.000000001;
    expect_error(evaluator.evaluate(book, proposal(), bad), ErrorCode::INVALID_ARGUMENT,
                 "close_price_precision", "RiskManager");
    bad = market;
    bad.valuations.front().mark = std::numeric_limits<double>::quiet_NaN();
    expect_error(evaluator.evaluate(book, proposal(), bad), ErrorCode::INVALID_ARGUMENT,
                 "invalid_mark", "RiskManager");
    auto cfg = config();
    cfg.var_limit = 0;
    ComponentRiskEvaluator invalid(cfg);
    expect_error(invalid.evaluate(book, proposal(), market), ErrorCode::INVALID_ARGUMENT,
                 "invalid_var_limit", "RiskManager");
    cfg = config();
    cfg.capital = Decimal::from_raw(1);
    ComponentRiskEvaluator overflowing(cfg);
    bad = market;
    bad.valuations.front().mark = 1e300;
    expect_error(overflowing.evaluate(book, proposal({{key("a"), Quantity(10)}}), bad),
                 ErrorCode::INVALID_RISK_CALCULATION, "nonfinite_risk_output", "RiskManager");
    bad = market;
    bad.valuations.front().mark = 1e308;
    expect_error(evaluator.evaluate(book, proposal({{key("a"), Quantity(10)}}), bad),
                 ErrorCode::INVALID_ARGUMENT, "position_product", "RiskManager");
}

TEST(ComponentRiskEvaluatorTest, RepeatedPermutedAndDestroyedInputsStayOwnedAndCanonical) {
    ComponentRiskEvaluator evaluator(config());
    ComponentRiskEvaluation saved;
    {
        auto book = two_component_context();
        auto market = inputs_for({{AssetType::FUTURE, "ES"}});
        auto edit = proposal({{key("a"), Quantity(10)}});
        auto first = evaluator.evaluate(book, edit, market);
        ASSERT_TRUE(first.is_ok());
        saved = first.value();
        book.portfolio_id = "mutated";
        edit.quantities.clear();
        market.valuations.clear();
    }
    EXPECT_EQ(saved.evaluated_book.portfolio_id, "portfolio-a");
    EXPECT_EQ(saved.evaluated_proposal.quantities[0].quantity, Quantity(10));
    EXPECT_EQ(saved.evaluated_inputs.valuations.size(), 1u);
    EXPECT_DOUBLE_EQ(saved.risk.gross_leverage, 1.5);

    auto first_book = two_component_context();
    auto second_book = first_book;
    second_book.portfolio_id = "portfolio-b";
    second_book.revision = "revision-b";
    for (auto& slot : second_book.slots) {
        slot.key.portfolio_id = "portfolio-b";
        slot.previous->quantity = Quantity(1);
    }
    auto second_market = inputs_for({{AssetType::FUTURE, "ES"}});
    second_market.expected_portfolio_id = "portfolio-b";
    second_market.expected_revision = "revision-b";
    auto second_edit = proposal();
    second_edit.expected_portfolio_id = "portfolio-b";
    second_edit.expected_revision = "revision-b";
    auto second = evaluator.evaluate(second_book, second_edit, second_market);
    ASSERT_TRUE(second.is_ok());
    EXPECT_EQ(second.value().bindings[0].calculation_id, saved.bindings[0].calculation_id);
    EXPECT_EQ(second.value().evaluated_book.portfolio_id, "portfolio-b");
    EXPECT_DOUBLE_EQ(second.value().risk.gross_leverage, 0.1);
    EXPECT_EQ(saved.evaluated_book.portfolio_id, "portfolio-a");

    auto permuted_book = two_component_context();
    permuted_book.slots[0].editable = true;
    auto permuted_edit = proposal({{key("b"), Quantity(5)}, {key("a"), Quantity(10)}});
    auto permuted = evaluator.evaluate(
        permuted_book, permuted_edit, inputs_for({{AssetType::FUTURE, "ES"}}));
    ASSERT_TRUE(permuted.is_ok());
    ASSERT_EQ(permuted.value().evaluated_proposal.quantities.size(), 2u);
    EXPECT_EQ(permuted.value().evaluated_proposal.quantities[0].key, key("a"));
    EXPECT_EQ(permuted.value().evaluated_proposal.quantities[1].key, key("b"));
    EXPECT_EQ(permuted_edit.quantities[0].key, key("b"));
    EXPECT_DOUBLE_EQ(permuted.value().risk.gross_leverage, 1.5);
}

}  // namespace
}  // namespace trade_ngin
