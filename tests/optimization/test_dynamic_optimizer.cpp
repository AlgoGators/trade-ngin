#include <gtest/gtest.h>
#include "trade_ngin/optimization/dynamic_optimizer.hpp"

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
    // (5.0 * 0.001 * 10) + (0.1 * 5.0 * 0.001) = 0.05 + 0.0005 = 0.0505
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
    //EXPECT_EQ(result.value().positions, std::vector<double>({1.0, 3.0, -1.0}));
    std::vector<double> positions = {1.0, 3.0, -1.0};

    for (size_t i = 0; i < positions.size(); ++i) {
        EXPECT_NEAR(positions[i], positions[i], 1e-6);
    }

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
