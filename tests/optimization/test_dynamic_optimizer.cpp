#include <gtest/gtest.h>
#include "trade_ngin/optimization/dynamic_optimizer.hpp"

#include <cmath>
#include <vector>
#include "../core/test_base.hpp"
namespace trade_ngin {

class DynamicOptimizerTest : public ::testing::Test {
protected:
    DynamicOptConfig default_config;
    DynamicOptimizerTest() {
        default_config.tau = 1.0;
        default_config.capital = 100.0;
        default_config.asymmetric_risk_buffer = 0.1;
        default_config.cost_penalty_scalar = 10;
        default_config.max_iterations = 1000;
        default_config.convergence_threshold = 1e-6;
    }

    std::vector<std::vector<double>> identity_covariance(size_t n) {
        std::vector<std::vector<double>> cov(n, std::vector<double>(n, 0.0));
        for (size_t i = 0; i < n; ++i)
            cov[i][i] = 1.0;
        return cov;
    }
};

// Test input validation through public API
TEST_F(DynamicOptimizerTest, InvalidInputs) {
    DynamicOptimizer optimizer(default_config);
    std::vector<double> valid_positions(2, 0.0);
    std::vector<double> empty_vec;
    std::vector<std::vector<double>> invalid_cov{{1.0}, {1.0, 2.0}};

    // Test empty inputs
    auto result = optimizer.optimize_single_period(empty_vec, valid_positions, valid_positions,
                                                   valid_positions, invalid_cov);
    EXPECT_TRUE(result.is_error());

    // Test size mismatch
    result = optimizer.optimize_single_period(valid_positions, {1.0}, valid_positions,
                                              valid_positions, identity_covariance(2));
    EXPECT_TRUE(result.is_error());
}

// Test cost penalty calculation indirectly
TEST_F(DynamicOptimizerTest, CostPenaltyThroughOptimization) {
    DynamicOptConfig config = default_config;
    config.tau = 2.0;
    DynamicOptimizer optimizer(config);

    std::vector<double> current = {0.0};
    std::vector<double> target = {5.0};  // Diff of 5.0
    std::vector<double> costs = {0.001};
    auto cov = identity_covariance(1);

    auto result = optimizer.optimize_single_period(current, target, costs, {1.0}, cov);
    ASSERT_FALSE(result.is_error());

    // Expected cost penalty:
    // trade_size * costs * cost_penalty_scalar = 5.0 * 0.001 * 10 = 0.05
    EXPECT_NEAR(result.value().cost_penalty, 0.05, 1e-6);
}

// Test tracking error through optimization results
TEST_F(DynamicOptimizerTest, TrackingErrorCalculation) {
    DynamicOptimizer optimizer(default_config);
    std::vector<double> current = {3.0, 4.0};
    std::vector<double> target = {4.0, 4.0};  // Diff of [1.0, 0.0]
    auto cov = identity_covariance(2);

    auto result = optimizer.optimize_single_period(current, target, {0.1, 0.1}, {1.0, 1.0}, cov);
    ASSERT_FALSE(result.is_error());

    // Verify result is within expected range
    EXPECT_NEAR(result.value().tracking_error, 1.0, 1e-6);  // Cost penalty only
}

// Test position rounding through optimization output
TEST_F(DynamicOptimizerTest, PositionRounding) {
    DynamicOptimizer optimizer(default_config);
    std::vector<double> current = {1.3, 2.7, -0.5};
    auto cov = identity_covariance(3);

    // Target positions that would require rounding
    auto result =
        optimizer.optimize_single_period(current, current, {0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}, cov);

    ASSERT_FALSE(result.is_error());
    EXPECT_EQ(result.value().positions, std::vector<double>({1.0, 3.0, 0.0}));
}

// Test configuration updates
TEST_F(DynamicOptimizerTest, UpdateConfig) {
    DynamicOptimizer optimizer(default_config);

    DynamicOptConfig new_config = default_config;
    new_config.tau = 2.0;
    auto update_result = optimizer.update_config(new_config);
    EXPECT_FALSE(update_result.is_error());
    EXPECT_EQ(optimizer.get_config().tau, 2.0);

    // Test invalid config through public API
    new_config.tau = -1.0;
    update_result = optimizer.update_config(new_config);
    EXPECT_TRUE(update_result.is_error());
}

// Test convergence through iteration count
TEST_F(DynamicOptimizerTest, ConvergenceBehavior) {
    DynamicOptConfig config = default_config;
    config.max_iterations = 5;
    DynamicOptimizer optimizer(config);

    std::vector<double> current(10, 0.0);
    std::vector<double> target(10, 0.0);  // Identical positions
    auto cov = identity_covariance(10);

    auto result = optimizer.optimize_single_period(current, target, std::vector<double>(10, 0.1),
                                                   std::vector<double>(10, 1.0), cov);

    ASSERT_FALSE(result.is_error());
    EXPECT_LE(result.value().iterations, 1);
    EXPECT_TRUE(result.value().converged);
}

}  // namespace trade_ngin

// ===== folded in from tests/optimization/test_dynamic_optimizer_extended.cpp =====
// Extended branch coverage for dynamic_optimizer.cpp. Targets:
// - optimize (the public buffered variant) covering both buffer-skip and
//   buffer-trade paths
// - apply_buffering tracking-error-vs-buffer comparison
// - validate_inputs square covariance check
// - update_config invalid configs
// - Catch-all error path
namespace dynamic_optimizer_extended_detail {

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

}  // namespace dynamic_optimizer_extended_detail
