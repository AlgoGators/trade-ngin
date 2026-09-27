#include "trade_ngin/portfolio/qt_evaluation.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

namespace trade_ngin {
namespace {

Timestamp day(int n) { return Timestamp{} + std::chrono::hours(24 * n); }

ComponentPositionKey key(const char* owner, const char* symbol = "XYZ") {
    return {"book", owner, owner, "2026-09-25", symbol, "qt_proposal"};
}

Position prior(const char* symbol, Quantity quantity) {
    return {symbol, quantity, Price(100), Decimal(0), Decimal(0), day(2)};
}

QtEvaluationRequest request() {
    QtEvaluationRequest out;
    out.operation = QtEvaluationOperation::SelectedBook;
    out.evaluator_build = "local-qt-controlled";
    out.context_fingerprint = std::string(64, 'a');
    out.risk_config_source_id = "synthetic-risk-policy";
    out.context = {"book", "2026-09-25", "qt_proposal", "revision", {
        {key("alpha"), {AssetType::FUTURE, "XYZ"}, true, prior("XYZ", Quantity(5))},
        {key("beta"), {AssetType::FUTURE, "XYZ"}, true, prior("XYZ", Quantity(5))},
        {key("immutable"), {AssetType::FUTURE, "XYZ"}, false,
         prior("XYZ", Quantity(-3))},
    }};
    out.proposal = {"book", "2026-09-25", "qt_proposal", "revision", {
        {key("alpha"), Quantity(5)}, {key("beta"), Quantity(1)},
    }};
    out.risk_inputs = {"book", "2026-09-25", "qt_proposal", "revision",
                       "synthetic-market", day(2), "USD", {}, {day(0), day(1), day(2)}, {}};
    out.risk_inputs.valuations.push_back({{AssetType::FUTURE, "XYZ"}, day(2),
                                          100.0, 1.0, "USD"});
    for (int n = 0; n < 3; ++n)
        out.risk_inputs.closes.push_back({{AssetType::FUTURE, "XYZ"}, day(n),
                                          n == 0 ? 98.0 : n == 1 ? 99.0 : 100.0});
    out.risk_config.capital = Decimal(1000);
    out.risk_config.var_limit = 100;
    out.risk_config.jump_risk_limit = 100;
    out.risk_config.max_correlation = 1;
    out.risk_config.max_gross_leverage = 0.5;
    out.risk_config.max_net_leverage = 100;
    out.risk_config.version = "synthetic-risk-config";
    out.quantity_rules.emplace(InstrumentIdentity{AssetType::FUTURE, "XYZ"},
        QuantityRule{Quantity(1), QuantityRoundingMode::reject_off_increment,
                     Quantity(-100), Quantity(100)});
    for (const char* owner : {"alpha", "beta"})
        out.component_cost_inputs.push_back({key(owner), {AssetType::FUTURE, "XYZ"},
            Quantity(1), Quantity(0.25), "linear-cash-per-increment-v1",
            "synthetic-cost-source", "USD"});
    return out;
}

const ComponentPositionCandidate& component(const QtEvaluation& evaluation,
                                            const char* owner) {
    auto it = std::find_if(evaluation.evaluated_book.components.begin(),
                           evaluation.evaluated_book.components.end(),
                           [owner](const auto& row) { return row.key.strategy_id == owner; });
    EXPECT_NE(it, evaluation.evaluated_book.components.end());
    return *it;
}

TEST(QtEvaluationTest, SelectedFiveOneIsPreservedAndRiskSeesCompleteBook) {
    auto result = evaluate_qt_request(request());
    ASSERT_TRUE(result.is_ok()) << (result.error() ? result.error()->what() : "");
    const auto& out = result.value();
    EXPECT_EQ(component(out, "alpha").position.quantity.to_string(), "5");
    EXPECT_EQ(component(out, "beta").position.quantity.to_string(), "1");
    EXPECT_EQ(component(out, "immutable").position.quantity.to_string(), "-3");
    EXPECT_EQ(out.evaluated_book.instruments[0].net_quantity.to_string(), "3");
    EXPECT_EQ(out.optimizer_status, QtEvidenceStatus::Disabled);
    EXPECT_FALSE(out.optimizer.has_value());
    EXPECT_EQ(out.risk_status, QtEvidenceStatus::Evaluated);
    ASSERT_TRUE(out.risk.has_value());
    EXPECT_EQ(out.risk->evaluated_book.instruments[0].net_quantity.to_string(), "3");
    EXPECT_EQ(out.cost_status, QtEvidenceStatus::Evaluated);
    ASSERT_TRUE(out.selected_costs.has_value());
    ASSERT_EQ(out.selected_costs->by_component.size(), 2U);
    EXPECT_EQ(out.selected_costs->by_component[0].estimated_cash_cost.to_string(), "0");
    EXPECT_EQ(out.selected_costs->by_component[1].estimated_cash_cost.to_string(), "1");
    EXPECT_EQ(out.selected_costs->evaluated_book_digest, out.evaluated_book_digest);
    EXPECT_EQ(out.risk_config_source_id, "synthetic-risk-policy");
}

TEST(QtEvaluationTest, OmittedEditableKeyCannotBecomeImplicitZero) {
    auto input = request();
    input.proposal.quantities.pop_back();
    auto result = evaluate_qt_request(input);
    ASSERT_TRUE(result.is_error());
    EXPECT_NE(std::string(result.error()->what()).find("incomplete_editable_selection"),
              std::string::npos);

    input.proposal.quantities.push_back({key("beta"), Quantity(0)});
    auto explicit_zero = evaluate_qt_request(input);
    ASSERT_TRUE(explicit_zero.is_ok());
    EXPECT_EQ(component(explicit_zero.value(), "beta").position.quantity.to_string(), "0");
}

TEST(QtEvaluationTest, OffsetOwnersRemainSeparateAtZeroNet) {
    auto input = request();
    input.context.slots[1].previous->quantity = Quantity(-5);
    input.context.slots[2].previous->quantity = Quantity(0);
    input.proposal.quantities[1].quantity = Quantity(-5);
    auto result = evaluate_qt_request(input);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().evaluated_book.instruments[0].net_quantity.to_string(), "0");
    EXPECT_EQ(component(result.value(), "alpha").position.quantity.to_string(), "5");
    EXPECT_EQ(component(result.value(), "beta").position.quantity.to_string(), "-5");
    EXPECT_EQ(result.value().evaluated_book.instruments[0].members.size(), 3U);
}

TEST(QtEvaluationTest, MissingCostAndMarketEvidenceRemainUnavailable) {
    auto input = request();
    input.component_cost_inputs.pop_back();
    input.risk_inputs.valuations.clear();
    auto result = evaluate_qt_request(input);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().cost_status, QtEvidenceStatus::Unavailable);
    EXPECT_FALSE(result.value().selected_costs.has_value());
    EXPECT_EQ(result.value().risk_status, QtEvidenceStatus::Unavailable);
    EXPECT_FALSE(result.value().risk.has_value());
    EXPECT_NE(result.value().evaluated_book_digest, "");
}

TEST(QtEvaluationTest, MissingRiskPolicyIsUnavailableEvenWithMarketRows) {
    auto input = request();
    input.risk_config_source_id.clear();
    auto result = evaluate_qt_request(input);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().risk_status, QtEvidenceStatus::Unavailable);
    EXPECT_FALSE(result.value().risk.has_value());
    EXPECT_EQ(result.value().cost_status, QtEvidenceStatus::Evaluated);
}

TEST(QtEvaluationTest, MissingMarketSourceOrCapitalCurrencyWithholdsRisk) {
    auto input = request();
    input.risk_inputs.market_snapshot_id.clear();
    auto source = evaluate_qt_request(input);
    ASSERT_TRUE(source.is_ok());
    EXPECT_EQ(source.value().risk_status, QtEvidenceStatus::Unavailable);
    EXPECT_FALSE(source.value().risk.has_value());

    input = request();
    input.risk_inputs.capital_currency.clear();
    auto currency = evaluate_qt_request(input);
    ASSERT_TRUE(currency.is_ok());
    EXPECT_EQ(currency.value().risk_status, QtEvidenceStatus::Unavailable);
    EXPECT_FALSE(currency.value().risk.has_value());
    EXPECT_EQ(currency.value().cost_status, QtEvidenceStatus::Unavailable);
    EXPECT_FALSE(currency.value().selected_costs.has_value());
}

TEST(QtEvaluationTest, DraftMissingSharedMarketSourceWithholdsOptimizer) {
    auto input = request();
    input.operation = QtEvaluationOperation::DraftDiagnostic;
    input.optimizer_enabled = true;
    input.optimizer_config_source_id = "synthetic-optimizer-policy";
    ComponentOptimizerInputs optimizer_inputs;
    optimizer_inputs.market_snapshot_id = "synthetic-market";
    optimizer_inputs.capital_currency = "USD";
    optimizer_inputs.valuation_time = day(2);
    input.optimizer_inputs = optimizer_inputs;
    input.optimizer_config = DynamicOptConfig{};
    input.risk_inputs.market_snapshot_id.clear();
    auto result = evaluate_qt_request(input);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().optimizer_status, QtEvidenceStatus::Unavailable);
    EXPECT_FALSE(result.value().optimizer.has_value());
    EXPECT_EQ(result.value().risk_status, QtEvidenceStatus::Unavailable);
    EXPECT_FALSE(result.value().risk.has_value());
}

TEST(QtEvaluationTest, MissingCostCurrencyWithholdsEstimateButMismatchRejects) {
    auto input = request();
    input.component_cost_inputs[0].currency.clear();
    auto missing = evaluate_qt_request(input);
    ASSERT_TRUE(missing.is_ok());
    EXPECT_EQ(missing.value().cost_status, QtEvidenceStatus::Unavailable);
    EXPECT_FALSE(missing.value().selected_costs.has_value());
    EXPECT_EQ(missing.value().risk_status, QtEvidenceStatus::Evaluated);

    input.component_cost_inputs[0].currency = "EUR";
    auto mismatch = evaluate_qt_request(input);
    ASSERT_TRUE(mismatch.is_ok());
    EXPECT_EQ(mismatch.value().cost_status, QtEvidenceStatus::Rejected);
    EXPECT_FALSE(mismatch.value().selected_costs.has_value());
}

TEST(QtEvaluationTest, SuppliedWrongMarketScopeRemainsRejected) {
    auto input = request();
    input.risk_inputs.valuations[0].mark_as_of = day(1);
    auto result = evaluate_qt_request(input);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().risk_status, QtEvidenceStatus::Rejected);
    EXPECT_FALSE(result.value().risk.has_value());
}

TEST(QtEvaluationTest, MissingHistoryAndQuantityRuleAreUnavailable) {
    auto input = request();
    input.risk_inputs.closes.clear();
    auto history = evaluate_qt_request(input);
    ASSERT_TRUE(history.is_ok());
    EXPECT_EQ(history.value().risk_status, QtEvidenceStatus::Unavailable);
    EXPECT_FALSE(history.value().risk.has_value());

    input = request();
    input.quantity_rules.clear();
    auto rule = evaluate_qt_request(input);
    ASSERT_TRUE(rule.is_ok());
    EXPECT_EQ(rule.value().risk_status, QtEvidenceStatus::Unavailable);
    EXPECT_EQ(rule.value().cost_status, QtEvidenceStatus::Unavailable);
    EXPECT_FALSE(rule.value().risk.has_value());
    EXPECT_FALSE(rule.value().selected_costs.has_value());
}

TEST(QtEvaluationTest, KnownRiskBreachRemainsEvaluated) {
    auto input = request();
    input.risk_config.max_gross_leverage = 0.01;
    auto result = evaluate_qt_request(input);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().risk_status, QtEvidenceStatus::Evaluated);
    ASSERT_TRUE(result.value().risk.has_value());
    EXPECT_TRUE(result.value().risk->risk.risk_exceeded);
}

TEST(QtEvaluationTest, RejectOnlyRulesRejectFractionalFutureAndAllowEquityDecimal8) {
    auto input = request();
    input.proposal.quantities[0].quantity = Quantity(2.5);
    auto future = evaluate_qt_request(input);
    ASSERT_TRUE(future.is_ok());
    EXPECT_EQ(future.value().risk_status, QtEvidenceStatus::Rejected);
    EXPECT_EQ(future.value().cost_status, QtEvidenceStatus::Rejected);

    for (auto& slot : input.context.slots) slot.instrument.type = AssetType::EQUITY;
    input.quantity_rules.clear();
    input.quantity_rules.emplace(InstrumentIdentity{AssetType::EQUITY, "XYZ"},
        QuantityRule{Quantity::from_raw(1), QuantityRoundingMode::reject_off_increment,
                     Quantity(-100), Quantity(100)});
    for (auto& valuation : input.risk_inputs.valuations)
        valuation.instrument.type = AssetType::EQUITY;
    for (auto& close : input.risk_inputs.closes) close.instrument.type = AssetType::EQUITY;
    for (auto& cost : input.component_cost_inputs) {
        cost.instrument.type = AssetType::EQUITY;
        cost.calculation_increment = Quantity(0.5);
    }
    auto equity = evaluate_qt_request(input);
    ASSERT_TRUE(equity.is_ok());
    EXPECT_EQ(equity.value().risk_status, QtEvidenceStatus::Evaluated);
    EXPECT_EQ(equity.value().cost_status, QtEvidenceStatus::Evaluated);
    EXPECT_EQ(component(equity.value(), "alpha").position.quantity.to_string(), "2.5");
}

TEST(QtEvaluationTest, FutureRemainsWholeWhenSuppliedRuleAllowsHalves) {
    auto input = request();
    input.quantity_rules.at({AssetType::FUTURE, "XYZ"}).increment = Quantity(0.5);
    input.proposal.quantities[0].quantity = Quantity(2.5);
    auto result = evaluate_qt_request(input);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().risk_status, QtEvidenceStatus::Rejected);
    EXPECT_EQ(result.value().cost_status, QtEvidenceStatus::Rejected);
}

TEST(QtEvaluationTest, SameSymbolDifferentTypesRemainTwoRiskAxes) {
    auto input = request();
    input.context.slots[1].instrument.type = AssetType::EQUITY;
    input.context.slots[2].instrument.type = AssetType::EQUITY;
    input.quantity_rules.emplace(InstrumentIdentity{AssetType::EQUITY, "XYZ"},
        QuantityRule{Quantity::from_raw(1), QuantityRoundingMode::reject_off_increment,
                     Quantity(-100), Quantity(100)});
    input.risk_inputs.valuations.push_back({{AssetType::EQUITY, "XYZ"}, day(2),
                                            100.0, 1.0, "USD"});
    for (int n = 0; n < 3; ++n)
        input.risk_inputs.closes.push_back({{AssetType::EQUITY, "XYZ"}, day(n),
                                            n == 0 ? 98.0 : n == 1 ? 99.0 : 100.0});
    input.component_cost_inputs[1].instrument.type = AssetType::EQUITY;
    auto result = evaluate_qt_request(input);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().evaluated_book.instruments.size(), 2U);
    EXPECT_EQ(result.value().risk_status, QtEvidenceStatus::Evaluated);
    EXPECT_EQ(result.value().cost_status, QtEvidenceStatus::Evaluated);
}

TEST(QtEvaluationTest, NewUnfilledPositionIsKeptWithoutFabricatedBasis) {
    auto input = request();
    input.context.slots[0].previous.reset();
    auto result = evaluate_qt_request(input);
    ASSERT_TRUE(result.is_ok());
    EXPECT_TRUE(component(result.value(), "alpha").unfilled);
    EXPECT_EQ(component(result.value(), "alpha").position.average_price.to_string(), "0");
    ASSERT_TRUE(result.value().selected_costs.has_value());
    EXPECT_EQ(result.value().selected_costs->by_component[0].previous_quantity.to_string(), "0");
    EXPECT_EQ(result.value().selected_costs->by_component[0].estimated_cash_cost.to_string(), "1.25");
}

TEST(QtEvaluationTest, ExplicitZeroAndImmutableRowRemainInSelectedDigest) {
    auto input = request();
    input.proposal.quantities[1].quantity = Quantity(0);
    auto with_immutable = evaluate_qt_request(input);
    ASSERT_TRUE(with_immutable.is_ok());
    EXPECT_EQ(component(with_immutable.value(), "beta").position.quantity.to_string(), "0");
    EXPECT_EQ(component(with_immutable.value(), "immutable").position.quantity.to_string(), "-3");

    input.context.slots.pop_back();
    auto without_immutable = evaluate_qt_request(input);
    ASSERT_TRUE(without_immutable.is_ok());
    EXPECT_NE(with_immutable.value().evaluated_book_digest,
              without_immutable.value().evaluated_book_digest);
}

TEST(QtEvaluationTest, MissingOptimizerInputsAreUnavailableAndSelectedNeverCallsIt) {
    auto input = request();
    input.optimizer_enabled = true;
    input.optimizer_config_source_id = "synthetic-optimizer-policy";
    auto selected = evaluate_qt_request(input);
    ASSERT_TRUE(selected.is_ok());
    EXPECT_EQ(selected.value().optimizer_status, QtEvidenceStatus::Disabled);
    input.operation = QtEvaluationOperation::DraftDiagnostic;
    auto diagnostic = evaluate_qt_request(input);
    ASSERT_TRUE(diagnostic.is_ok());
    EXPECT_EQ(diagnostic.value().optimizer_status, QtEvidenceStatus::Unavailable);
    input.optimizer_enabled = false;
    auto disabled = evaluate_qt_request(input);
    ASSERT_TRUE(disabled.is_ok());
    EXPECT_EQ(disabled.value().optimizer_status, QtEvidenceStatus::Disabled);
}

TEST(QtEvaluationTest, EachEnabledKernelIsInvokedAtMostOnce) {
    int risk_calls = 0;
    int optimizer_calls = 0;
    QtEvaluationKernels kernels;
    kernels.risk = [&](const RiskConfig& config, const ComponentBookContext& context,
                       const ComponentBookProposal& proposal, const ComponentRiskInputs& inputs) {
        ++risk_calls;
        return ComponentRiskEvaluator(config).evaluate(context, proposal, inputs);
    };
    kernels.optimizer = [&](const DynamicOptConfig& config,
                            const ComponentBookContext& context,
                            const ComponentBookProposal& proposal,
                            const ComponentOptimizerInputs& inputs) {
        ++optimizer_calls;
        return ComponentOptimizerEvaluator(config).evaluate(context, proposal, inputs);
    };
    auto input = request();
    auto selected = evaluate_qt_request(input, kernels);
    ASSERT_TRUE(selected.is_ok());
    EXPECT_EQ(risk_calls, 1);
    EXPECT_EQ(optimizer_calls, 0);

    risk_calls = 0;
    input.operation = QtEvaluationOperation::DraftDiagnostic;
    input.optimizer_enabled = true;
    input.optimizer_config_source_id = "synthetic-optimizer-policy";
    input.risk_inputs.valuation_time = day(20);
    input.risk_inputs.valuations[0].mark_as_of = day(20);
    input.risk_inputs.expected_observation_times.clear();
    input.risk_inputs.closes.clear();
    for (int n = 0; n <= 20; ++n) {
        input.risk_inputs.expected_observation_times.push_back(day(n));
        input.risk_inputs.closes.push_back({{AssetType::FUTURE, "XYZ"}, day(n),
            n % 2 == 0 ? 100.0 : 110.0});
    }
    ComponentOptimizerInputs optimizer_inputs;
    optimizer_inputs.expected_portfolio_id = "book";
    optimizer_inputs.expected_date = "2026-09-25";
    optimizer_inputs.expected_portfolio_type = "qt_proposal";
    optimizer_inputs.expected_revision = "revision";
    optimizer_inputs.market_snapshot_id = "synthetic-market";
    optimizer_inputs.valuation_time = day(20);
    optimizer_inputs.capital_currency = "USD";
    optimizer_inputs.instruments.push_back({{AssetType::FUTURE, "XYZ"}, day(20),
        100.0, 1.0, "USD", Quantity(1), 0.25, "USD", "synthetic-increment",
        "synthetic-cost"});
    optimizer_inputs.expected_observation_times = input.risk_inputs.expected_observation_times;
    for (int n = 0; n <= 20; ++n)
        optimizer_inputs.closes.push_back({{AssetType::FUTURE, "XYZ"}, day(n),
            n % 2 == 0 ? 100.0 : 110.0});
    input.optimizer_inputs = optimizer_inputs;
    DynamicOptConfig config;
    config.capital = 1000;
    config.tau = 1;
    config.cost_penalty_scalar = 1;
    config.max_iterations = 100;
    config.convergence_threshold = 1e-12;
    config.use_buffering = false;
    config.version = "synthetic-optimizer-config";
    input.optimizer_config = config;
    auto diagnostic = evaluate_qt_request(input, kernels);
    ASSERT_TRUE(diagnostic.is_ok());
    EXPECT_EQ(risk_calls, 1);
    EXPECT_EQ(optimizer_calls, 1);
    EXPECT_EQ(diagnostic.value().optimizer_status, QtEvidenceStatus::Evaluated);
    ASSERT_TRUE(diagnostic.value().optimizer.has_value());
    EXPECT_EQ(diagnostic.value().optimizer_config_source_id,
              "synthetic-optimizer-policy");
}

TEST(QtEvaluationTest, PermutingInputsLeavesSelectedDigestAndCostsStable) {
    auto input = request();
    auto first = evaluate_qt_request(input);
    ASSERT_TRUE(first.is_ok());
    std::reverse(input.context.slots.begin(), input.context.slots.end());
    std::reverse(input.proposal.quantities.begin(), input.proposal.quantities.end());
    std::reverse(input.component_cost_inputs.begin(), input.component_cost_inputs.end());
    std::reverse(input.risk_inputs.closes.begin(), input.risk_inputs.closes.end());
    auto reordered = evaluate_qt_request(input);
    ASSERT_TRUE(reordered.is_ok());
    EXPECT_EQ(reordered.value().evaluated_book_digest, first.value().evaluated_book_digest);
    ASSERT_TRUE(reordered.value().selected_costs);
    EXPECT_EQ(reordered.value().selected_costs->total_cash_cost,
              first.value().selected_costs->total_cash_cost);
}

}  // namespace
}  // namespace trade_ngin
