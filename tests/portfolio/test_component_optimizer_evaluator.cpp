#include <gtest/gtest.h>

#include "trade_ngin/portfolio/component_optimizer_evaluator.hpp"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace trade_ngin;

namespace {

Timestamp day(int n) { return Timestamp{} + std::chrono::hours(24 * n); }

ComponentPositionKey key(const char* strategy, const char* symbol = "XYZ") {
    return {"portfolio", strategy, strategy, "2026-09-22", symbol, "paper"};
}

ComponentBookContext context(Quantity previous = Quantity(0.0)) {
    ComponentBookContext value{"portfolio", "2026-09-22", "paper", "revision", {}};
    value.slots.push_back({key("editable"), {AssetType::FUTURE, "XYZ"}, true,
                           Position("XYZ", previous, Price(100.0), Decimal(0.0),
                                    Decimal(0.0), day(20))});
    return value;
}

ComponentBookProposal proposal(Quantity quantity = Quantity(0.75)) {
    return {"portfolio", "2026-09-22", "paper", "revision", {{key("editable"), quantity}}};
}

ComponentOptimizerInputs inputs() {
    ComponentOptimizerInputs value;
    value.expected_portfolio_id = "portfolio";
    value.expected_date = "2026-09-22";
    value.expected_portfolio_type = "paper";
    value.expected_revision = "revision";
    value.market_snapshot_id = "synthetic-snapshot";
    value.valuation_time = day(20);
    value.capital_currency = "USD";
    value.instruments.push_back({{AssetType::FUTURE, "XYZ"}, day(20), 100.0, 1.0,
                                 "USD", Quantity(0.25), 0.5, "USD", "synthetic-step",
                                 "synthetic-linear-cost"});
    for (int n = 0; n <= 20; ++n) {
        value.expected_observation_times.push_back(day(n));
        value.closes.push_back({{AssetType::FUTURE, "XYZ"}, day(n),
                                n % 2 == 0 ? 100.0 : 110.0});
    }
    return value;
}

DynamicOptConfig config() {
    DynamicOptConfig value;
    value.capital = 1000.0;
    value.tau = 1.0;
    value.cost_penalty_scalar = 1.0;
    value.max_iterations = 100;
    value.convergence_threshold = 1e-12;
    value.use_buffering = false;
    return value;
}

}  // namespace

TEST(ComponentOptimizerEvaluatorTest, CompleteBookUsesRealOptimizerAndFractionalCostUnits) {
    ComponentOptimizerEvaluator evaluator(config());
    auto result = evaluator.evaluate(context(), proposal(), inputs());
    ASSERT_TRUE(result.is_ok()) << (result.is_error() ? result.error()->what() : "");
    const auto& output = result.value();
    ASSERT_EQ(output.prepared.bindings.size(), 1u);
    EXPECT_DOUBLE_EQ(output.prepared.current_weights[0], 0.0);
    EXPECT_DOUBLE_EQ(output.prepared.target_weights[0], 0.075);
    EXPECT_DOUBLE_EQ(output.prepared.weights_per_increment[0], 0.025);
    EXPECT_DOUBLE_EQ(output.prepared.cost_coefficients[0], 0.02);
    const double variance = 252.0 * 20.0 / 19.0 * (21.0 / 220.0) * (21.0 / 220.0);
    EXPECT_NEAR(output.prepared.annualized_covariance[0][0], variance, 1e-12);
    EXPECT_NEAR(output.optimization.positions[0], 0.075, 1e-12);
    EXPECT_NEAR(output.optimization.cost_penalty, 0.0015, 1e-12);
    EXPECT_NEAR(output.optimization.tracking_error, 0.0015, 1e-12);
    EXPECT_EQ(output.trace.buffer_branch, OptimizationBufferBranch::Disabled);
    EXPECT_EQ(output.evaluated_book.components[0].position.quantity, Quantity(0.75));
}

TEST(ComponentOptimizerEvaluatorTest, NegativeTargetAndPriorExitKeepSignedWeights) {
    ComponentOptimizerEvaluator evaluator(config());
    auto negative = evaluator.evaluate(context(), proposal(Quantity(-0.75)), inputs());
    ASSERT_TRUE(negative.is_ok());
    EXPECT_NEAR(negative.value().prepared.target_weights[0], -0.075, 1e-12);
    EXPECT_NEAR(negative.value().optimization.positions[0], -0.075, 1e-12);
    EXPECT_NEAR(negative.value().optimization.cost_penalty, 0.0015, 1e-12);

    auto exit_proposal = proposal();
    exit_proposal.quantities.clear();
    auto exit = evaluator.evaluate(context(Quantity(0.75)), exit_proposal, inputs());
    ASSERT_TRUE(exit.is_ok());
    ASSERT_EQ(exit.value().prepared.bindings.size(), 1u);
    EXPECT_EQ(exit.value().prepared.bindings[0].previous_net_quantity, Quantity(0.75));
    EXPECT_EQ(exit.value().prepared.bindings[0].proposed_net_quantity, Quantity(0.0));
    EXPECT_NEAR(exit.value().optimization.positions[0], 0.0, 1e-12);
    EXPECT_NEAR(exit.value().optimization.cost_penalty, 0.0015, 1e-12);
}

TEST(ComponentOptimizerEvaluatorTest, RealBufferTraceExposesBothBranches) {
    auto data = inputs();
    data.instruments[0].cash_cost_per_increment = 0.0;
    const double variance = 252.0 * 20.0 / 19.0 * (21.0 / 220.0) * (21.0 / 220.0);
    auto cfg = config();
    cfg.use_buffering = true;
    cfg.buffer_size_factor = 0.01 * std::sqrt(variance);
    ComponentOptimizerEvaluator applied(cfg);
    auto result = applied.evaluate(context(), proposal(), data);
    ASSERT_TRUE(result.is_ok()) << (result.is_error() ? result.error()->what() : "");
    EXPECT_EQ(result.value().trace.buffer_branch, OptimizationBufferBranch::Applied);
    ASSERT_TRUE(result.value().trace.solver_positions);
    ASSERT_TRUE(result.value().trace.continuous_buffered_positions);
    ASSERT_TRUE(result.value().trace.rounded_buffered_positions);
    EXPECT_NEAR((*result.value().trace.solver_positions)[0], 0.075, 1e-12);
    EXPECT_NEAR((*result.value().trace.continuous_buffered_positions)[0], 0.065, 1e-12);
    EXPECT_NEAR((*result.value().trace.rounded_buffered_positions)[0], 0.075, 1e-12);
    EXPECT_EQ(result.value().evaluated_book.components[0].position.quantity, Quantity(0.75));

    cfg.buffer_size_factor = 0.1 * std::sqrt(variance);
    ComponentOptimizerEvaluator returned(cfg);
    auto prior = returned.evaluate(context(), proposal(), data);
    ASSERT_TRUE(prior.is_ok());
    EXPECT_EQ(prior.value().trace.buffer_branch, OptimizationBufferBranch::ReturnedPrior);
    ASSERT_TRUE(prior.value().trace.solver_positions);
    EXPECT_FALSE(prior.value().trace.continuous_buffered_positions);
    EXPECT_FALSE(prior.value().trace.rounded_buffered_positions);
    EXPECT_DOUBLE_EQ(prior.value().optimization.positions[0], 0.0);
}

TEST(ComponentOptimizerEvaluatorTest, ImmutableOffsetAndZeroNetRemainComplete) {
    auto book = context(Quantity(10.0));
    book.slots.push_back({key("immutable"), {AssetType::FUTURE, "XYZ"}, false,
                          Position("XYZ", Quantity(-7.0), Price(100.0), Decimal(0.0),
                                   Decimal(0.0), day(20))});
    auto request = proposal(Quantity(5.0));
    ComponentOptimizerEvaluator evaluator(config());
    auto negative = evaluator.evaluate(book, request, inputs());
    ASSERT_TRUE(negative.is_ok());
    ASSERT_EQ(negative.value().evaluated_book.components.size(), 2u);
    EXPECT_EQ(negative.value().prepared.bindings[0].previous_net_quantity, Quantity(3.0));
    EXPECT_EQ(negative.value().prepared.bindings[0].proposed_net_quantity, Quantity(-2.0));
    EXPECT_EQ(negative.value().evaluated_book.components[1].position.quantity, Quantity(-7.0));
    EXPECT_EQ(negative.value().prepared.bindings[0].members.size(), 2u);

    request.quantities[0].quantity = Quantity(7.0);
    auto zero = evaluator.evaluate(book, request, inputs());
    ASSERT_TRUE(zero.is_ok());
    EXPECT_EQ(zero.value().prepared.bindings[0].proposed_net_quantity, Quantity(0.0));
    EXPECT_EQ(zero.value().evaluated_book.components[1].position.quantity, Quantity(-7.0));
}

TEST(ComponentOptimizerEvaluatorTest, PreviousTypedAggregationOverflowRejects) {
    auto book = context(Quantity::from_raw(std::numeric_limits<int64_t>::max()));
    book.slots.push_back({key("second"), {AssetType::FUTURE, "XYZ"}, true,
                          Position("XYZ", Quantity::from_raw(1), Price(100.0),
                                   Decimal(0.0), Decimal(0.0), day(20))});
    auto request = proposal(Quantity(0.0));
    request.quantities.push_back({key("second"), Quantity(0.0)});
    ComponentOptimizerEvaluator evaluator(config());
    auto result = evaluator.evaluate(book, request, inputs());
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_DATA);
    EXPECT_NE(std::string(result.error()->what()).find("previous aggregate"), std::string::npos);
}

TEST(ComponentOptimizerEvaluatorTest, TypedSameSymbolRetainsSeparateAxes) {
    auto book = context();
    book.slots.push_back({key("equity"), {AssetType::EQUITY, "XYZ"}, true,
                          Position("XYZ", Quantity(0.0), Price(100.0), Decimal(0.0),
                                   Decimal(0.0), day(20))});
    auto request = proposal();
    request.quantities.push_back({key("equity"), Quantity(0.75)});
    auto data = inputs();
    auto equity = data.instruments[0];
    equity.instrument.type = AssetType::EQUITY;
    equity.price_multiplier = 1.0;
    equity.cash_cost_per_increment = 0.25;
    data.instruments[0].price_multiplier = 10.0;
    data.instruments.push_back(equity);
    for (const auto& close : inputs().closes) {
        auto row = close;
        row.instrument.type = AssetType::EQUITY;
        row.close = close.close * 2.0;
        data.closes.push_back(row);
    }
    std::reverse(data.instruments.begin(), data.instruments.end());
    std::reverse(data.closes.begin(), data.closes.end());
    ComponentOptimizerEvaluator evaluator(config());
    auto result = evaluator.evaluate(book, request, data);
    ASSERT_TRUE(result.is_ok()) << (result.is_error() ? result.error()->what() : "");
    ASSERT_EQ(result.value().prepared.bindings.size(), 2u);
    EXPECT_EQ(result.value().prepared.bindings[0].instrument.type, AssetType::FUTURE);
    EXPECT_EQ(result.value().prepared.bindings[1].instrument.type, AssetType::EQUITY);
    EXPECT_NEAR(result.value().prepared.weights_per_increment[0], 0.25, 1e-12);
    EXPECT_NEAR(result.value().prepared.weights_per_increment[1], 0.025, 1e-12);
    EXPECT_NEAR(result.value().prepared.cost_coefficients[0], 0.002, 1e-12);
    EXPECT_NEAR(result.value().prepared.cost_coefficients[1], 0.01, 1e-12);
}

TEST(ComponentOptimizerEvaluatorTest, RejectsContextAndPreservesOverlayError) {
    ComponentOptimizerEvaluator evaluator(config());
    auto data = inputs();
    data.expected_revision = "stale";
    auto stale = evaluator.evaluate(context(), proposal(), data);
    ASSERT_TRUE(stale.is_error());
    EXPECT_EQ(stale.error()->code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_EQ(stale.error()->component(), "ComponentOptimizerEvaluator");
    EXPECT_NE(std::string(stale.error()->what()).find("stale"), std::string::npos);

    auto invalid_proposal = proposal();
    invalid_proposal.expected_revision = "stale";
    auto forwarded = evaluator.evaluate(context(), invalid_proposal, inputs());
    ASSERT_TRUE(forwarded.is_error());
    EXPECT_EQ(forwarded.error()->component(), "component_book");
    EXPECT_EQ(forwarded.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST(ComponentOptimizerEvaluatorTest, RejectsTypedCoverageAndTemporalGridDefects) {
    ComponentOptimizerEvaluator evaluator(config());
    const auto expect_invalid = [&](const ComponentOptimizerInputs& data, const char* prefix) {
        auto result = evaluator.evaluate(context(), proposal(), data);
        ASSERT_TRUE(result.is_error());
        EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
        EXPECT_NE(std::string(result.error()->what()).find(prefix), std::string::npos);
    };
    auto data = inputs();
    data.instruments.push_back(data.instruments[0]);
    expect_invalid(data, "optimizer_instrument_coverage");
    data = inputs();
    data.instruments[0].instrument.type = AssetType::EQUITY;
    expect_invalid(data, "optimizer_instrument_coverage");
    data = inputs();
    data.closes[0].instrument.type = AssetType::EQUITY;
    expect_invalid(data, "optimizer_history_grid");
    data = inputs();
    data.closes.push_back(data.closes[0]);
    expect_invalid(data, "optimizer_history_grid");
    data = inputs();
    data.closes.pop_back();
    expect_invalid(data, "optimizer_history_grid");
    data = inputs();
    data.closes[0].timestamp = day(21);
    expect_invalid(data, "optimizer_history_grid");
    data = inputs();
    std::swap(data.expected_observation_times[0], data.expected_observation_times[1]);
    expect_invalid(data, "optimizer_history_grid");
    data = inputs();
    data.expected_observation_times.pop_back();
    expect_invalid(data, "optimizer_history_grid");
    data = inputs();
    data.instruments[0].mark_as_of = day(19);
    expect_invalid(data, "optimizer_input_units");
    data = inputs();
    data.instruments[0].cost_currency = "EUR";
    expect_invalid(data, "optimizer_input_units");
    data = inputs();
    data.market_snapshot_id.clear();
    expect_invalid(data, "optimizer_input_units");
}

TEST(ComponentOptimizerEvaluatorTest, RejectsInvalidConfigurationAndNumericalEnvelope) {
    const auto expect_config = [&](const DynamicOptConfig& cfg) {
        ComponentOptimizerEvaluator evaluator(cfg);
        auto result = evaluator.evaluate(context(), proposal(), inputs());
        ASSERT_TRUE(result.is_error());
        EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
        EXPECT_NE(std::string(result.error()->what()).find("optimizer_config_invalid"),
                  std::string::npos);
    };
    auto cfg = config(); cfg.capital = 0.0; expect_config(cfg);
    cfg = config(); cfg.tau = std::numeric_limits<double>::infinity(); expect_config(cfg);
    cfg = config(); cfg.cost_penalty_scalar = -1.0; expect_config(cfg);
    cfg = config(); cfg.convergence_threshold = std::numeric_limits<double>::quiet_NaN();
    expect_config(cfg);
    cfg = config(); cfg.buffer_size_factor = -0.1; expect_config(cfg);
    cfg = config(); cfg.asymmetric_risk_buffer = std::numeric_limits<double>::infinity();
    expect_config(cfg);
    cfg = config(); cfg.max_iterations = 0; expect_config(cfg);
    cfg = config(); cfg.max_iterations = std::numeric_limits<int>::max(); expect_config(cfg);
    cfg = config(); cfg.tau = 1e308; cfg.buffer_size_factor = 1e308; expect_config(cfg);

    ComponentOptimizerEvaluator evaluator(config());
    auto data = inputs();
    data.instruments[0].mark = std::numeric_limits<double>::quiet_NaN();
    auto malformed = evaluator.evaluate(context(), proposal(), data);
    ASSERT_TRUE(malformed.is_error());
    EXPECT_EQ(malformed.error()->code(), ErrorCode::INVALID_ARGUMENT);
    data = inputs(); data.instruments[0].cash_cost_per_increment =
        std::numeric_limits<double>::infinity();
    malformed = evaluator.evaluate(context(), proposal(), data);
    ASSERT_TRUE(malformed.is_error());
    data = inputs(); data.instruments[0].calculation_increment = Quantity(0.0);
    malformed = evaluator.evaluate(context(), proposal(), data);
    ASSERT_TRUE(malformed.is_error());

    data = inputs(); data.instruments[0].mark = 1e200;
    auto unsafe = evaluator.evaluate(context(), proposal(), data);
    ASSERT_TRUE(unsafe.is_error());
    EXPECT_EQ(unsafe.error()->code(), ErrorCode::INVALID_DATA);
    EXPECT_NE(std::string(unsafe.error()->what()).find("optimizer_numeric_range"),
              std::string::npos);

    cfg = config(); cfg.use_buffering = true;
    cfg.max_iterations = std::numeric_limits<int>::max() - 1;
    ComponentOptimizerEvaluator buffer_evaluator(cfg);
    unsafe = buffer_evaluator.evaluate(context(), proposal(), inputs());
    ASSERT_TRUE(unsafe.is_error());
    EXPECT_NE(std::string(unsafe.error()->what()).find("buffer_step_count"),
              std::string::npos);
}

TEST(ComponentOptimizerEvaluatorTest, ConstantHistoryAndExplicitZeroCostAreValid) {
    auto data = inputs();
    for (auto& close : data.closes) close.close = 100.0;
    data.instruments[0].cash_cost_per_increment = 0.0;
    ComponentOptimizerEvaluator evaluator(config());
    auto result = evaluator.evaluate(context(), proposal(), data);
    ASSERT_TRUE(result.is_ok()) << (result.is_error() ? result.error()->what() : "");
    EXPECT_DOUBLE_EQ(result.value().prepared.annualized_covariance[0][0], 0.0);
    EXPECT_DOUBLE_EQ(result.value().prepared.cost_coefficients[0], 0.0);
    EXPECT_EQ(result.value().evaluated_book.components[0].position.quantity, Quantity(0.75));
}

TEST(ComponentOptimizerEvaluatorTest, OwnsCanonicalEvidenceAcrossPermutations) {
    auto book = context();
    book.slots.push_back({key("another"), {AssetType::FUTURE, "XYZ"}, true,
                          Position("XYZ", Quantity(0.0), Price(100.0), Decimal(0.0),
                                   Decimal(0.0), day(20))});
    auto request = proposal();
    request.quantities.push_back({key("another"), Quantity(0.25)});
    auto data = inputs();
    ComponentOptimizerEvaluator evaluator(config());
    auto first = evaluator.evaluate(book, request, data);
    ASSERT_TRUE(first.is_ok());
    std::reverse(book.slots.begin(), book.slots.end());
    std::reverse(request.quantities.begin(), request.quantities.end());
    std::reverse(data.closes.begin(), data.closes.end());
    auto second = evaluator.evaluate(book, request, data);
    ASSERT_TRUE(second.is_ok());
    ASSERT_EQ(first.value().evaluated_proposal.quantities.size(), 2u);
    EXPECT_EQ(first.value().evaluated_proposal.quantities[0].key,
              second.value().evaluated_proposal.quantities[0].key);
    EXPECT_EQ(first.value().evaluated_inputs.closes[0].timestamp,
              second.value().evaluated_inputs.closes[0].timestamp);
    EXPECT_EQ(first.value().prepared.bindings[0].members,
              second.value().prepared.bindings[0].members);
    EXPECT_DOUBLE_EQ(first.value().optimization.positions[0],
                     second.value().optimization.positions[0]);
    book.slots.clear(); request.quantities.clear(); data.closes.clear();
    EXPECT_EQ(first.value().evaluated_book.components.size(), 2u);
    EXPECT_EQ(first.value().evaluated_inputs.closes.size(), 21u);
}

TEST(ComponentOptimizerEvaluatorTest, CloseScalePreservesCovarianceWhileMarkChangesWeights) {
    ComponentOptimizerEvaluator evaluator(config());
    auto original = evaluator.evaluate(context(), proposal(), inputs());
    ASSERT_TRUE(original.is_ok());
    auto data = inputs();
    for (auto& close : data.closes) close.close *= 2.0;
    data.instruments[0].mark = 200.0;
    auto scaled = evaluator.evaluate(context(), proposal(), data);
    ASSERT_TRUE(scaled.is_ok());
    EXPECT_NEAR(scaled.value().prepared.annualized_covariance[0][0],
                original.value().prepared.annualized_covariance[0][0], 1e-15);
    EXPECT_NEAR(scaled.value().prepared.target_weights[0], 0.15, 1e-12);
    EXPECT_NEAR(scaled.value().prepared.weights_per_increment[0], 0.05, 1e-12);
    EXPECT_NEAR(scaled.value().prepared.cost_coefficients[0], 0.01, 1e-12);
    EXPECT_NEAR(original.value().prepared.target_weights[0], 0.075, 1e-12);
}

TEST(ComponentOptimizerEvaluatorTest, ZeroNetIdentityStillRequiresExplicitHistory) {
    auto book = context(Quantity(7.0));
    book.slots.push_back({key("immutable"), {AssetType::FUTURE, "XYZ"}, false,
                          Position("XYZ", Quantity(-7.0), Price(100.0), Decimal(0.0),
                                   Decimal(0.0), day(20))});
    auto request = proposal(Quantity(7.0));
    auto data = inputs();
    data.closes.pop_back();
    ComponentOptimizerEvaluator evaluator(config());
    auto rejected = evaluator.evaluate(book, request, data);
    ASSERT_TRUE(rejected.is_error());
    EXPECT_EQ(rejected.error()->code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_NE(std::string(rejected.error()->what()).find("optimizer_history_grid"),
              std::string::npos);
    data = inputs();
    auto accepted = evaluator.evaluate(book, request, data);
    ASSERT_TRUE(accepted.is_ok());
    EXPECT_EQ(accepted.value().prepared.bindings[0].proposed_net_quantity, Quantity(0.0));
    EXPECT_EQ(accepted.value().prepared.bindings[0].members.size(), 2u);
}

TEST(ComponentOptimizerEvaluatorTest, RejectsUnscaledCostOverflowBeforeSolverForZeroOrTinyPenalty) {
    auto cfg = config();
    cfg.capital = 1e-100;
    auto data = inputs();
    data.instruments[0].mark = 1e-92;
    data.instruments[0].cash_cost_per_increment = 1e208;
    // current weight is 1e8, step is 2.5e7 and cost coefficient is 4e300.
    // The solver's unscaled prior-cost product exceeds DBL_MAX even if the
    // configured scalar would subsequently erase or shrink that overflow.
    for (double penalty : {0.0, 1e-300}) {
        cfg.cost_penalty_scalar = penalty;
        ComponentOptimizerEvaluator evaluator(cfg);
        auto result = evaluator.evaluate(context(Quantity(1.0)), proposal(), data);
        ASSERT_TRUE(result.is_error());
        EXPECT_EQ(result.error()->component(), "ComponentOptimizerEvaluator");
        EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_DATA);
        EXPECT_NE(std::string(result.error()->what()).find("optimizer_numeric_range"),
                  std::string::npos) << result.error()->what();
        EXPECT_NE(std::string(result.error()->what()).find("unscaled_cost_bound"),
                  std::string::npos) << result.error()->what();
    }

    // Zero cash cost and an ordinary finite cash cost with zero penalty both
    // keep the unscaled solver intermediate finite and remain valid inputs.
    cfg.cost_penalty_scalar = 0.0;
    data.instruments[0].cash_cost_per_increment = 0.0;
    ComponentOptimizerEvaluator zero_cost(cfg);
    EXPECT_TRUE(zero_cost.evaluate(context(Quantity(1.0)), proposal(), data).is_ok());
    data.instruments[0].cash_cost_per_increment = 0.5;
    EXPECT_TRUE(zero_cost.evaluate(context(Quantity(1.0)), proposal(), data).is_ok());
}

TEST(ComponentOptimizerEvaluatorTest, InterleavedSameSpellingTypedBooksStayIndependent) {
    ComponentOptimizerEvaluator evaluator(config());
    auto future_book = context();
    auto future_proposal = proposal();
    auto future_data = inputs();
    auto first = evaluator.evaluate(future_book, future_proposal, future_data);
    ASSERT_TRUE(first.is_ok());

    auto equity_book = context();
    equity_book.slots[0].instrument.type = AssetType::EQUITY;
    auto equity_data = inputs();
    equity_data.instruments[0].instrument.type = AssetType::EQUITY;
    equity_data.instruments[0].mark = 150.0;
    equity_data.instruments[0].cash_cost_per_increment = 0.25;
    for (auto& close : equity_data.closes) {
        close.instrument.type = AssetType::EQUITY;
        if (close.close == 110.0) close.close = 120.0;
    }
    auto middle = evaluator.evaluate(equity_book, proposal(), equity_data);
    ASSERT_TRUE(middle.is_ok());
    auto last = evaluator.evaluate(future_book, future_proposal, future_data);
    ASSERT_TRUE(last.is_ok());

    EXPECT_EQ(first.value().prepared.bindings[0].instrument,
              (InstrumentIdentity{AssetType::FUTURE, "XYZ"}));
    EXPECT_EQ(middle.value().prepared.bindings[0].instrument,
              (InstrumentIdentity{AssetType::EQUITY, "XYZ"}));
    EXPECT_EQ(last.value().prepared.bindings[0].instrument,
              first.value().prepared.bindings[0].instrument);
    EXPECT_DOUBLE_EQ(first.value().prepared.target_weights[0],
                     last.value().prepared.target_weights[0]);
    EXPECT_DOUBLE_EQ(first.value().prepared.cost_coefficients[0],
                     last.value().prepared.cost_coefficients[0]);
    EXPECT_DOUBLE_EQ(first.value().prepared.annualized_covariance[0][0],
                     last.value().prepared.annualized_covariance[0][0]);
    EXPECT_EQ(first.value().optimization.positions, last.value().optimization.positions);
    EXPECT_EQ(first.value().trace.solver_positions, last.value().trace.solver_positions);
    EXPECT_NE(first.value().prepared.target_weights[0],
              middle.value().prepared.target_weights[0]);
    EXPECT_NE(first.value().prepared.annualized_covariance[0][0],
              middle.value().prepared.annualized_covariance[0][0]);
    EXPECT_EQ(first.value().evaluated_book.components[0].position.quantity, Quantity(0.75));
    equity_data.closes.clear();
    EXPECT_EQ(middle.value().evaluated_inputs.closes.size(), 21u);
    EXPECT_EQ(first.value().evaluated_inputs.closes.size(), 21u);
}
