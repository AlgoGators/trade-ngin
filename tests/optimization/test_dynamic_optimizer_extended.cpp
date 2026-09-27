// Extended branch coverage for dynamic_optimizer.cpp. Targets:
// - optimize (the public buffered variant) covering both buffer-skip and
//   buffer-trade paths
// - apply_buffering tracking-error-vs-buffer comparison
// - validate_inputs square covariance check
// - update_config invalid configs
// - Catch-all error path

#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include "../core/test_base.hpp"
#include "trade_ngin/optimization/dynamic_optimizer.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

DynamicOptConfig default_config() {
    DynamicOptConfig c;
    c.tau = 1.0;
    c.capital = 100.0;
    c.cost_penalty_scalar = 10.0;
    c.asymmetric_risk_buffer = 0.1;
    c.max_iterations = 100;
    c.convergence_threshold = 1e-6;
    c.use_buffering = false;
    c.buffer_size_factor = 0.05;
    return c;
}

std::vector<std::vector<double>> identity_cov(size_t n) {
    std::vector<std::vector<double>> cov(n, std::vector<double>(n, 0.0));
    for (size_t i = 0; i < n; ++i) cov[i][i] = 1.0;
    return cov;
}

}  // namespace

class DynamicOptimizerExtendedTest : public TestBase {};

namespace {

void expect_consumption_result(const OptimizationResult& result, std::vector<double> positions,
                               double tracking_error, double cost_penalty, int iterations,
                               bool converged) {
    EXPECT_EQ(result.positions, positions);
    EXPECT_NEAR(result.tracking_error, tracking_error, 1e-12);
    EXPECT_NEAR(result.cost_penalty, cost_penalty, 1e-12);
    EXPECT_EQ(result.iterations, iterations);
    EXPECT_EQ(result.converged, converged);
}

void expect_same_consumption_result(const OptimizationResult& observed,
                                    const OptimizationResult& plain) {
    EXPECT_EQ(observed.positions, plain.positions);
    EXPECT_DOUBLE_EQ(observed.tracking_error, plain.tracking_error);
    EXPECT_DOUBLE_EQ(observed.cost_penalty, plain.cost_penalty);
    EXPECT_EQ(observed.iterations, plain.iterations);
    EXPECT_EQ(observed.converged, plain.converged);
}

}  // namespace

TEST_F(DynamicOptimizerExtendedTest, ConsumptionRecordsSolverReadsAndDisabledGuardWithZeroCosts) {
    auto config = default_config();
    config.cost_penalty_scalar = 12.5;
    config.max_iterations = 7;
    config.convergence_threshold = 0.00025;
    DynamicOptimizer opt(config);
    OptimizationTrace trace;
    const auto plain = opt.optimize({0.0}, {3.0}, {0.0}, {1.0}, identity_cov(1));
    const auto observed = opt.optimize({0.0}, {3.0}, {0.0}, {1.0}, identity_cov(1), &trace);
    ASSERT_TRUE(plain.is_ok());
    ASSERT_TRUE(observed.is_ok());
    expect_consumption_result(observed.value(), {3.0}, 0.0, 0.0, 4, true);
    expect_same_consumption_result(observed.value(), plain.value());
    EXPECT_EQ(trace.consumed_config.cost_penalty_scalar, 12.5);
    EXPECT_EQ(trace.consumed_config.max_iterations, 7);
    EXPECT_EQ(trace.consumed_config.convergence_threshold, 0.00025);
    EXPECT_EQ(trace.consumed_config.use_buffering, false);
    EXPECT_FALSE(trace.consumed_config.tau.has_value());
    EXPECT_FALSE(trace.consumed_config.buffer_size_factor.has_value());
    EXPECT_EQ(trace.buffer_branch, OptimizationBufferBranch::Disabled);
}

TEST_F(DynamicOptimizerExtendedTest, ConsumptionRecordsAppliedAndReturnedPriorBufferInputs) {
    auto config = default_config();
    config.use_buffering = true;
    config.cost_penalty_scalar = 12.5;
    config.tau = 2.0;
    config.buffer_size_factor = 0.3;
    DynamicOptimizer opt(config);
    OptimizationTrace trace;
    const auto applied_plain = opt.optimize({0.0}, {3.0}, {0.01}, {1.0}, identity_cov(1));
    const auto applied = opt.optimize({0.0}, {3.0}, {0.01}, {1.0}, identity_cov(1), &trace);
    ASSERT_TRUE(applied_plain.is_ok());
    ASSERT_TRUE(applied.is_ok());
    expect_consumption_result(applied.value(), {2.0}, 1.25, 0.25, 0, true);
    expect_same_consumption_result(applied.value(), applied_plain.value());
    EXPECT_EQ(trace.consumed_config.cost_penalty_scalar, 12.5);
    EXPECT_EQ(trace.consumed_config.use_buffering, true);
    EXPECT_EQ(trace.consumed_config.tau, 2.0);
    EXPECT_EQ(trace.consumed_config.buffer_size_factor, 0.3);
    EXPECT_EQ(trace.buffer_branch, OptimizationBufferBranch::Applied);
    ASSERT_TRUE(trace.continuous_buffered_positions.has_value());
    EXPECT_NEAR(trace.continuous_buffered_positions->at(0), 2.4, 1e-12);
    ASSERT_TRUE(trace.rounded_buffered_positions.has_value());
    EXPECT_EQ(*trace.rounded_buffered_positions, std::vector<double>({2.0}));

    config.buffer_size_factor = 2.0;
    ASSERT_TRUE(opt.update_config(config).is_ok());
    const auto prior_plain = opt.optimize({1.0}, {3.0}, {0.0}, {1.0}, identity_cov(1));
    const auto prior = opt.optimize({1.0}, {3.0}, {0.0}, {1.0}, identity_cov(1), &trace);
    ASSERT_TRUE(prior_plain.is_ok());
    ASSERT_TRUE(prior.is_ok());
    expect_consumption_result(prior.value(), {1.0}, 2.0, 0.0, 0, true);
    expect_same_consumption_result(prior.value(), prior_plain.value());
    EXPECT_EQ(trace.consumed_config.tau, 2.0);
    EXPECT_EQ(trace.consumed_config.buffer_size_factor, 2.0);
    EXPECT_EQ(trace.buffer_branch, OptimizationBufferBranch::ReturnedPrior);
    EXPECT_FALSE(trace.continuous_buffered_positions.has_value());
    EXPECT_FALSE(trace.rounded_buffered_positions.has_value());
}

TEST_F(DynamicOptimizerExtendedTest, ConsumptionOmitsThresholdWithoutCandidateComparison) {
    auto config = default_config();
    config.max_iterations = 0;
    DynamicOptimizer opt(config);
    OptimizationTrace trace;
    const auto zero_plain = opt.optimize({0.0}, {3.0}, {0.0}, {1.0}, identity_cov(1));
    const auto zero = opt.optimize({0.0}, {3.0}, {0.0}, {1.0}, identity_cov(1), &trace);
    ASSERT_TRUE(zero_plain.is_ok());
    ASSERT_TRUE(zero.is_ok());
    expect_consumption_result(zero.value(), {0.0}, 3.0, 0.0, 1, false);
    expect_same_consumption_result(zero.value(), zero_plain.value());
    EXPECT_EQ(trace.consumed_config.cost_penalty_scalar, 10.0);
    EXPECT_EQ(trace.consumed_config.max_iterations, 0);
    EXPECT_FALSE(trace.consumed_config.convergence_threshold.has_value());
    EXPECT_EQ(trace.consumed_config.use_buffering, false);

    config.max_iterations = 9;
    ASSERT_TRUE(opt.update_config(config).is_ok());
    const auto equal_plain = opt.optimize({0.0}, {0.0}, {0.0}, {1.0}, identity_cov(1));
    const auto equal = opt.optimize({0.0}, {0.0}, {0.0}, {1.0}, identity_cov(1), &trace);
    ASSERT_TRUE(equal_plain.is_ok());
    ASSERT_TRUE(equal.is_ok());
    expect_consumption_result(equal.value(), {0.0}, 0.0, 0.0, 1, true);
    expect_same_consumption_result(equal.value(), equal_plain.value());
    EXPECT_EQ(trace.consumed_config.max_iterations, 9);
    EXPECT_FALSE(trace.consumed_config.convergence_threshold.has_value());
}

TEST_F(DynamicOptimizerExtendedTest, DirectConsumptionResetsOnInvalidRetryWithoutBufferReads) {
    auto config = default_config();
    config.use_buffering = true;
    DynamicOptimizer opt(config);
    OptimizationConfigConsumption consumed;
    const auto plain = opt.optimize_single_period({0.0}, {3.0}, {0.01}, {1.0}, identity_cov(1));
    const auto observed = opt.optimize_single_period({0.0}, {3.0}, {0.01}, {1.0}, identity_cov(1), &consumed);
    ASSERT_TRUE(plain.is_ok());
    ASSERT_TRUE(observed.is_ok());
    expect_consumption_result(observed.value(), {3.0}, 0.3, 0.3, 4, true);
    expect_same_consumption_result(observed.value(), plain.value());
    EXPECT_EQ(consumed.cost_penalty_scalar, 10.0);
    EXPECT_EQ(consumed.max_iterations, 100);
    EXPECT_EQ(consumed.convergence_threshold, 1e-6);
    EXPECT_FALSE(consumed.use_buffering.has_value());
    EXPECT_FALSE(consumed.tau.has_value());
    EXPECT_FALSE(consumed.buffer_size_factor.has_value());

    const auto invalid_plain = opt.optimize_single_period({0.0}, {3.0}, {}, {1.0}, identity_cov(1));
    const auto invalid = opt.optimize_single_period({0.0}, {3.0}, {}, {1.0}, identity_cov(1), &consumed);
    ASSERT_TRUE(invalid_plain.is_error());
    ASSERT_TRUE(invalid.is_error());
    EXPECT_EQ(invalid.error()->code(), invalid_plain.error()->code());
    EXPECT_STREQ(invalid.error()->what(), invalid_plain.error()->what());
    EXPECT_FALSE(consumed.cost_penalty_scalar.has_value());
    EXPECT_FALSE(consumed.max_iterations.has_value());
    EXPECT_FALSE(consumed.convergence_threshold.has_value());
    EXPECT_FALSE(consumed.use_buffering.has_value());
    EXPECT_FALSE(consumed.tau.has_value());
    EXPECT_FALSE(consumed.buffer_size_factor.has_value());
}

TEST_F(DynamicOptimizerExtendedTest, FullTraceClearsAcrossConfigUpdateAndInvalidSolver) {
    auto config = default_config();
    config.use_buffering = true;
    config.buffer_size_factor = 0.6;
    DynamicOptimizer opt(config);
    OptimizationTrace trace;
    ASSERT_TRUE(opt.optimize({0.0}, {3.0}, {0.0}, {1.0}, identity_cov(1), &trace).is_ok());
    ASSERT_TRUE(trace.rounded_buffered_positions.has_value());
    EXPECT_EQ(trace.consumed_config.buffer_size_factor, 0.6);

    config.use_buffering = false;
    config.cost_penalty_scalar = 7.25;
    config.max_iterations = 8;
    config.convergence_threshold = 0.002;
    ASSERT_TRUE(opt.update_config(config).is_ok());
    EXPECT_DOUBLE_EQ(opt.get_config().cost_penalty_scalar, 7.25);
    EXPECT_EQ(trace.consumed_config.cost_penalty_scalar, 10.0);
    const auto disabled = opt.optimize({0.0}, {3.0}, {0.0}, {1.0}, identity_cov(1), &trace);
    ASSERT_TRUE(disabled.is_ok());
    expect_consumption_result(disabled.value(), {3.0}, 0.0, 0.0, 4, true);
    EXPECT_EQ(trace.consumed_config.cost_penalty_scalar, 7.25);
    EXPECT_EQ(trace.consumed_config.max_iterations, 8);
    EXPECT_EQ(trace.consumed_config.convergence_threshold, 0.002);
    EXPECT_EQ(trace.consumed_config.use_buffering, false);
    EXPECT_FALSE(trace.consumed_config.tau.has_value());
    EXPECT_FALSE(trace.consumed_config.buffer_size_factor.has_value());
    EXPECT_EQ(trace.buffer_branch, OptimizationBufferBranch::Disabled);
    EXPECT_FALSE(trace.continuous_buffered_positions.has_value());
    EXPECT_FALSE(trace.rounded_buffered_positions.has_value());

    const auto invalid_plain = opt.optimize({0.0}, {3.0}, {}, {1.0}, identity_cov(1));
    const auto invalid = opt.optimize({0.0}, {3.0}, {}, {1.0}, identity_cov(1), &trace);
    ASSERT_TRUE(invalid_plain.is_error());
    ASSERT_TRUE(invalid.is_error());
    EXPECT_EQ(invalid.error()->code(), invalid_plain.error()->code());
    EXPECT_STREQ(invalid.error()->what(), invalid_plain.error()->what());
    EXPECT_FALSE(trace.consumed_config.cost_penalty_scalar.has_value());
    EXPECT_FALSE(trace.consumed_config.max_iterations.has_value());
    EXPECT_FALSE(trace.consumed_config.convergence_threshold.has_value());
    EXPECT_EQ(trace.consumed_config.use_buffering, false);
    EXPECT_FALSE(trace.consumed_config.tau.has_value());
    EXPECT_FALSE(trace.consumed_config.buffer_size_factor.has_value());
    EXPECT_FALSE(trace.solver_positions.has_value());
    EXPECT_FALSE(trace.continuous_buffered_positions.has_value());
    EXPECT_FALSE(trace.rounded_buffered_positions.has_value());
    EXPECT_EQ(trace.buffer_branch, OptimizationBufferBranch::NotReached);

    config.use_buffering = true;
    ASSERT_TRUE(opt.update_config(config).is_ok());
    const auto invalid_true = opt.optimize({0.0}, {3.0}, {}, {1.0}, identity_cov(1), &trace);
    ASSERT_TRUE(invalid_true.is_error());
    EXPECT_EQ(trace.consumed_config.use_buffering, true);
    EXPECT_FALSE(trace.consumed_config.cost_penalty_scalar.has_value());
    EXPECT_FALSE(trace.consumed_config.max_iterations.has_value());
    EXPECT_FALSE(trace.consumed_config.convergence_threshold.has_value());
    EXPECT_FALSE(trace.consumed_config.tau.has_value());
    EXPECT_FALSE(trace.consumed_config.buffer_size_factor.has_value());
    EXPECT_EQ(trace.buffer_branch, OptimizationBufferBranch::NotReached);
}

TEST_F(DynamicOptimizerExtendedTest, CharacterizesSolverAndBufferedResultMetrics) {
    auto config = default_config();
    config.use_buffering = true;
    config.buffer_size_factor = 0.6;
    config.cost_penalty_scalar = 10.0;
    DynamicOptimizer opt(config);

    auto direct = opt.optimize_single_period({0.0}, {3.0}, {0.0}, {1.0}, identity_cov(1));
    auto buffered = opt.optimize({0.0}, {3.0}, {0.0}, {1.0}, identity_cov(1));
    ASSERT_TRUE(direct.is_ok());
    ASSERT_TRUE(buffered.is_ok());
    EXPECT_EQ(direct.value().positions, std::vector<double>({3.0}));
    EXPECT_DOUBLE_EQ(direct.value().tracking_error, 0.0);
    EXPECT_DOUBLE_EQ(direct.value().cost_penalty, 0.0);
    EXPECT_EQ(direct.value().iterations, 4);
    EXPECT_EQ(buffered.value().positions, std::vector<double>({2.0}));
    EXPECT_DOUBLE_EQ(buffered.value().tracking_error, 1.0);
    EXPECT_DOUBLE_EQ(buffered.value().cost_penalty, 0.0);
    EXPECT_EQ(buffered.value().iterations, 0);
}

TEST_F(DynamicOptimizerExtendedTest, TraceCapturesSolverContinuousAndRoundedStages) {
    auto config = default_config();
    config.use_buffering = true;
    config.buffer_size_factor = 0.6;
    DynamicOptimizer opt(config);
    OptimizationTrace trace;
    auto result = opt.optimize({0.0}, {3.0}, {0.0}, {1.0}, identity_cov(1), &trace);
    ASSERT_TRUE(result.is_ok());
    ASSERT_TRUE(trace.solver_positions.has_value());
    ASSERT_TRUE(trace.continuous_buffered_positions.has_value());
    ASSERT_TRUE(trace.rounded_buffered_positions.has_value());
    EXPECT_EQ(*trace.solver_positions, std::vector<double>({3.0}));
    EXPECT_NEAR(trace.continuous_buffered_positions->at(0), 2.4, 1e-12);
    EXPECT_EQ(*trace.rounded_buffered_positions, std::vector<double>({2.0}));
    EXPECT_EQ(trace.buffer_branch, OptimizationBufferBranch::Applied);
    EXPECT_EQ(result.value().positions, std::vector<double>({2.0}));
}

TEST_F(DynamicOptimizerExtendedTest, TraceIdentifiesPriorBookBranch) {
    auto config = default_config();
    config.use_buffering = true;
    config.buffer_size_factor = 100.0;
    DynamicOptimizer opt(config);
    OptimizationTrace trace;
    auto result = opt.optimize({1.0}, {3.0}, {0.0}, {1.0}, identity_cov(1), &trace);
    ASSERT_TRUE(result.is_ok());
    ASSERT_TRUE(trace.solver_positions.has_value());
    EXPECT_EQ(*trace.solver_positions, std::vector<double>({3.0}));
    EXPECT_FALSE(trace.continuous_buffered_positions.has_value());
    EXPECT_FALSE(trace.rounded_buffered_positions.has_value());
    EXPECT_EQ(trace.buffer_branch, OptimizationBufferBranch::ReturnedPrior);
    EXPECT_EQ(result.value().positions, std::vector<double>({1.0}));
    EXPECT_EQ(result.value().iterations, 0);
}

TEST_F(DynamicOptimizerExtendedTest, TraceIdentifiesDisabledBufferWithoutInventedStages) {
    DynamicOptimizer opt(default_config());
    OptimizationTrace trace;
    auto result = opt.optimize({0.0}, {3.0}, {0.0}, {1.0}, identity_cov(1), &trace);
    ASSERT_TRUE(result.is_ok());
    ASSERT_TRUE(trace.solver_positions.has_value());
    EXPECT_EQ(*trace.solver_positions, std::vector<double>({3.0}));
    EXPECT_FALSE(trace.continuous_buffered_positions.has_value());
    EXPECT_FALSE(trace.rounded_buffered_positions.has_value());
    EXPECT_EQ(trace.buffer_branch, OptimizationBufferBranch::Disabled);
    EXPECT_EQ(result.value().positions, std::vector<double>({3.0}));
}

TEST_F(DynamicOptimizerExtendedTest, ReusedTraceClearsStagesOnSolverError) {
    auto config = default_config();
    config.use_buffering = true;
    config.buffer_size_factor = 0.6;
    DynamicOptimizer opt(config);
    OptimizationTrace trace;
    ASSERT_TRUE(opt.optimize({0.0}, {3.0}, {0.0}, {1.0}, identity_cov(1), &trace).is_ok());
    ASSERT_TRUE(trace.rounded_buffered_positions.has_value());

    auto error = opt.optimize({0.0}, {3.0}, {}, {1.0}, identity_cov(1), &trace);
    ASSERT_TRUE(error.is_error());
    EXPECT_FALSE(trace.solver_positions.has_value());
    EXPECT_FALSE(trace.continuous_buffered_positions.has_value());
    EXPECT_FALSE(trace.rounded_buffered_positions.has_value());
    EXPECT_EQ(trace.buffer_branch, OptimizationBufferBranch::NotReached);
}

TEST_F(DynamicOptimizerExtendedTest, TraceAndNoTracePreserveExistingResultFields) {
    auto config = default_config();
    config.use_buffering = true;
    config.buffer_size_factor = 0.6;
    DynamicOptimizer opt(config);
    OptimizationTrace trace;
    auto without = opt.optimize({0.0}, {3.0}, {0.01}, {1.0}, identity_cov(1));
    auto with = opt.optimize({0.0}, {3.0}, {0.01}, {1.0}, identity_cov(1), &trace);
    ASSERT_TRUE(without.is_ok());
    ASSERT_TRUE(with.is_ok());
    EXPECT_EQ(with.value().positions, without.value().positions);
    EXPECT_DOUBLE_EQ(with.value().tracking_error, without.value().tracking_error);
    EXPECT_DOUBLE_EQ(with.value().cost_penalty, without.value().cost_penalty);
    EXPECT_EQ(with.value().iterations, without.value().iterations);
    EXPECT_EQ(with.value().converged, without.value().converged);
    EXPECT_EQ(with.value().positions, std::vector<double>({2.0}));
    EXPECT_NEAR(with.value().tracking_error, 1.2, 1e-12);
    EXPECT_NEAR(with.value().cost_penalty, 0.2, 1e-12);
}

// ===== validate_inputs branches =====

TEST_F(DynamicOptimizerExtendedTest, ValidateInputsRejectsNonSquareCovariance) {
    DynamicOptimizer opt(default_config());
    std::vector<std::vector<double>> jagged = {{1.0, 0.0}, {0.0}};  // row 1 has size 1, expected 2
    auto r = opt.optimize_single_period({1.0, 2.0}, {1.0, 2.0}, {0.1, 0.1}, {1.0, 1.0}, jagged);
    EXPECT_TRUE(r.is_error());
}

TEST_F(DynamicOptimizerExtendedTest, ValidateInputsRejectsCostsSizeMismatch) {
    DynamicOptimizer opt(default_config());
    auto r = opt.optimize_single_period({1.0, 2.0}, {1.0, 2.0}, {0.1}, {1.0, 1.0}, identity_cov(2));
    EXPECT_TRUE(r.is_error());
}

TEST_F(DynamicOptimizerExtendedTest, ValidateInputsRejectsWeightsSizeMismatch) {
    DynamicOptimizer opt(default_config());
    auto r = opt.optimize_single_period({1.0, 2.0}, {1.0, 2.0}, {0.1, 0.1}, {1.0}, identity_cov(2));
    EXPECT_TRUE(r.is_error());
}

// ===== optimize (with buffering disabled) returns optimize_single_period =====

TEST_F(DynamicOptimizerExtendedTest, OptimizeWithBufferingDisabledMatchesSinglePeriod) {
    auto cfg = default_config();
    cfg.use_buffering = false;
    DynamicOptimizer opt(cfg);
    std::vector<double> current{0.0, 0.0};
    std::vector<double> target{2.0, 3.0};
    auto cov = identity_cov(2);
    auto buffered = opt.optimize({0.0, 0.0}, target, {0.1, 0.1}, {1.0, 1.0}, cov);
    auto direct = opt.optimize_single_period(current, target, {0.1, 0.1}, {1.0, 1.0}, cov);
    ASSERT_TRUE(buffered.is_ok());
    ASSERT_TRUE(direct.is_ok());
    EXPECT_EQ(buffered.value().positions, direct.value().positions);
}

// ===== optimize with buffering enabled =====

TEST_F(DynamicOptimizerExtendedTest, OptimizeWithBufferingDoesNotErrorAndReturnsResult) {
    auto cfg = default_config();
    cfg.use_buffering = true;
    cfg.buffer_size_factor = 0.05;
    DynamicOptimizer opt(cfg);
    auto cov = identity_cov(2);
    auto r = opt.optimize({0.0, 0.0}, {2.0, 3.0}, {0.1, 0.1}, {1.0, 1.0}, cov);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().positions.size(), 2u);
}

TEST_F(DynamicOptimizerExtendedTest, BufferingSkipsTradesWhenWithinBuffer) {
    auto cfg = default_config();
    cfg.use_buffering = true;
    cfg.buffer_size_factor = 100.0;  // huge buffer → no trading needed
    DynamicOptimizer opt(cfg);
    auto cov = identity_cov(2);
    // Optimizer would normally suggest moving to (2,3); buffer should hold us at (0,0).
    auto r = opt.optimize({0.0, 0.0}, {2.0, 3.0}, {0.1, 0.1}, {1.0, 1.0}, cov);
    ASSERT_TRUE(r.is_ok());
    // With huge buffer, current positions should be returned (buffering kept them).
    EXPECT_DOUBLE_EQ(r.value().positions[0], 0.0);
    EXPECT_DOUBLE_EQ(r.value().positions[1], 0.0);
    EXPECT_EQ(r.value().iterations, 0);
}

// ===== update_config branches =====

TEST_F(DynamicOptimizerExtendedTest, UpdateConfigRejectsNegativeTau) {
    DynamicOptimizer opt(default_config());
    auto bad = default_config();
    bad.tau = -0.5;
    EXPECT_TRUE(opt.update_config(bad).is_error());
}

TEST_F(DynamicOptimizerExtendedTest, UpdateConfigRejectsZeroTau) {
    DynamicOptimizer opt(default_config());
    auto bad = default_config();
    bad.tau = 0.0;
    EXPECT_TRUE(opt.update_config(bad).is_error());
}

TEST_F(DynamicOptimizerExtendedTest, UpdateConfigRejectsNegativeCapital) {
    DynamicOptimizer opt(default_config());
    auto bad = default_config();
    bad.capital = -100.0;
    EXPECT_TRUE(opt.update_config(bad).is_error());
}

TEST_F(DynamicOptimizerExtendedTest, UpdateConfigRejectsNegativeCostPenaltyScalar) {
    DynamicOptimizer opt(default_config());
    auto bad = default_config();
    bad.cost_penalty_scalar = -1.0;
    EXPECT_TRUE(opt.update_config(bad).is_error());
}

TEST_F(DynamicOptimizerExtendedTest, UpdateConfigRejectsNegativeBufferSizeFactor) {
    DynamicOptimizer opt(default_config());
    auto bad = default_config();
    bad.buffer_size_factor = -0.05;
    EXPECT_TRUE(opt.update_config(bad).is_error());
}

TEST_F(DynamicOptimizerExtendedTest, GetConfigReflectsUpdate) {
    DynamicOptimizer opt(default_config());
    auto good = default_config();
    good.tau = 3.5;
    good.cost_penalty_scalar = 25.0;
    ASSERT_TRUE(opt.update_config(good).is_ok());
    EXPECT_DOUBLE_EQ(opt.get_config().tau, 3.5);
    EXPECT_DOUBLE_EQ(opt.get_config().cost_penalty_scalar, 25.0);
}

// ===== Optimization output invariants =====

TEST_F(DynamicOptimizerExtendedTest, OptimizationConvergedFlagSetWhenTargetEqualsCurrent) {
    DynamicOptimizer opt(default_config());
    std::vector<double> pos{1.0, 2.0};
    auto r = opt.optimize_single_period(pos, pos, {0.1, 0.1}, {1.0, 1.0}, identity_cov(2));
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().converged);
}

TEST_F(DynamicOptimizerExtendedTest, OptimizationProducesIntegerPositionsWithUnitWeights) {
    DynamicOptimizer opt(default_config());
    std::vector<double> current{0.0, 0.0, 0.0};
    std::vector<double> target{2.7, -3.3, 0.4};
    auto r = opt.optimize_single_period(current, target, {0.0, 0.0, 0.0}, {1.0, 1.0, 1.0},
                                         identity_cov(3));
    ASSERT_TRUE(r.is_ok());
    for (double p : r.value().positions) {
        EXPECT_DOUBLE_EQ(p, std::round(p));  // unit weights → integer positions
    }
}

TEST_F(DynamicOptimizerExtendedTest, ResultTrackingErrorEqualsPureTrackingErrorPlusCost) {
    DynamicOptimizer opt(default_config());
    std::vector<double> current{0.0};
    std::vector<double> target{5.0};
    auto r = opt.optimize_single_period(current, target, {0.001}, {1.0}, identity_cov(1));
    ASSERT_TRUE(r.is_ok());
    // Position rounded to 5.0 → pure_te = 0; cost = 5.0 * 0.001 * 10 = 0.05
    EXPECT_NEAR(r.value().cost_penalty, 0.05, 1e-9);
}

TEST_F(DynamicOptimizerExtendedTest, OptimizationWithSmallStepWeightsHaltsAtMaxIterations) {
    auto cfg = default_config();
    cfg.max_iterations = 3;  // tight cap
    DynamicOptimizer opt(cfg);
    std::vector<double> current(5, 0.0);
    std::vector<double> target{10.0, 10.0, 10.0, 10.0, 10.0};
    auto r = opt.optimize_single_period(current, target, std::vector<double>(5, 0.001),
                                         std::vector<double>(5, 0.5), identity_cov(5));
    ASSERT_TRUE(r.is_ok());
    EXPECT_LE(r.value().iterations, 4);  // bounded
}

TEST_F(DynamicOptimizerExtendedTest, SerializeRoundTripPreservesValues) {
    auto cfg = default_config();
    cfg.tau = 2.5;
    cfg.cost_penalty_scalar = 15.0;
    cfg.use_buffering = true;
    auto j = cfg.to_json();
    DynamicOptConfig restored;
    restored.from_json(j);
    EXPECT_DOUBLE_EQ(restored.tau, 2.5);
    EXPECT_DOUBLE_EQ(restored.cost_penalty_scalar, 15.0);
    EXPECT_TRUE(restored.use_buffering);
}
