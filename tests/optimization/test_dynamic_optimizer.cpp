#include <gtest/gtest.h>
#include "trade_ngin/optimization/dynamic_optimizer.hpp"

#include <Eigen/Dense>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>
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

// T-7b-2 9d (OPT-N1): this test used to pin the truncation (a cap of 3 stopped a 100-contract
// book after 3 contracts). The configured max_iterations is now a floor: the greedy's pass cap is
// at least 2 x (the target book in contracts) + 1, so the book is reached: 100 adopted steps of
// half a unit (20 contracts on each of 5 symbols) plus the empty pass that finds nothing.
TEST_F(DynamicOptimizerExtendedTest, ConfiguredCapBelowTheBookIsAFloorNotATruncation) {
    auto cfg = default_config();
    cfg.max_iterations = 3;  // configured far below the book
    DynamicOptimizer opt(cfg);
    std::vector<double> current(5, 0.0);
    std::vector<double> target{10.0, 10.0, 10.0, 10.0, 10.0};
    auto r = opt.optimize_single_period(current, target, std::vector<double>(5, 0.001),
                                        std::vector<double>(5, 0.5), identity_cov(5));
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().positions, target);
    EXPECT_EQ(r.value().iterations, 101);
    EXPECT_TRUE(r.value().converged);
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

// ===== T-7b-2 9d (OPT-N1): the greedy's pass cap scales with the book =====
// The greedy starts at zero and adopts one contract of one symbol per pass, so a target of L
// contracts (shorts included: the long-only write-back discards short asks only after the
// optimizer) needs at least L passes. A constant cap of 100 truncated every book above 100
// contracts (T-VOL: 333 of 368 lap-1 greedy calls at 5x capital). These tests pin that the book is
// no longer truncated AND that nothing else about the greedy moved: the answer equals a verbatim
// copy of the parent's greedy (e9994839) run with its cap lifted, and, on a book the parent never
// capped, equals the parent's greedy at the configured cap in positions and pass count.
namespace opt_n1_detail {

using namespace trade_ngin;

struct RefResult {
    std::vector<double> positions;
    double tracking_error;
    double cost_penalty;
    int iterations;
    bool converged;
};

// Verbatim copy of DynamicOptimizer::optimize_single_period's greedy at e9994839 with the cap as a
// parameter (the parent read config_.max_iterations).
RefResult parent_greedy(const DynamicOptConfig& cfg, int cap, const std::vector<double>& actual,
                        const std::vector<double>& target_positions,
                        const std::vector<double>& costs,
                        const std::vector<double>& weights_per_contract,
                        const std::vector<std::vector<double>>& covariance) {
    const size_t n = actual.size();
    Eigen::MatrixXd cov(n, n);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j)
            cov(i, j) = covariance[i][j];
    Eigen::VectorXd target(n), actual_eigen(n), costs_eigen(n);
    for (size_t i = 0; i < n; ++i) {
        target(i) = target_positions[i];
        actual_eigen(i) = actual[i];
        costs_eigen(i) = costs[i];
    }
    Eigen::VectorXd current_best = Eigen::VectorXd::Zero(n);
    Eigen::VectorXd e = target - current_best;
    double tracking_error_sq = e.transpose() * cov * e;
    double pure_tracking_error = std::sqrt(std::max(0.0, tracking_error_sq));
    Eigen::VectorXd trade_diff = current_best - actual_eigen;
    double cost_penalty =
        (trade_diff.cwiseAbs().array() * costs_eigen.array()).sum() * cfg.cost_penalty_scalar;
    double best_tracking_error = pure_tracking_error + cost_penalty;
    bool improved = true;
    int iteration = 0;
    Eigen::VectorXd cov_diag = cov.diagonal();
    while (improved && iteration++ < cap) {
        improved = false;
        e = target - current_best;
        Eigen::VectorXd cov_e = cov * e;
        double base_te_sq = e.dot(cov_e);
        trade_diff = current_best - actual_eigen;
        double base_cost =
            (trade_diff.cwiseAbs().array() * costs_eigen.array()).sum() * cfg.cost_penalty_scalar;
        double proposed_err = best_tracking_error;
        int best_i = -1;
        double best_step = 0.0;
        for (size_t i = 0; i < n; ++i) {
            double raw_diff = target(i) - current_best(i);
            if (std::abs(raw_diff) < 1e-12)
                continue;
            double step = weights_per_contract[i] * (raw_diff > 0 ? +1.0 : -1.0);
            double new_te_sq = base_te_sq - 2.0 * step * cov_e(i) + step * step * cov_diag(i);
            double new_pure_te = std::sqrt(std::max(0.0, new_te_sq));
            double old_trade_i = std::abs(current_best(i) - actual_eigen(i));
            double new_trade_i = std::abs(current_best(i) + step - actual_eigen(i));
            double new_cost =
                base_cost + (new_trade_i - old_trade_i) * costs_eigen(i) * cfg.cost_penalty_scalar;
            double total_err = new_pure_te + new_cost;
            if (total_err + cfg.convergence_threshold < proposed_err) {
                best_i = static_cast<int>(i);
                best_step = step;
                proposed_err = total_err;
            }
        }
        if (best_i >= 0 && proposed_err + cfg.convergence_threshold < best_tracking_error) {
            current_best(best_i) += best_step;
            best_tracking_error = proposed_err;
            improved = true;
        }
    }
    Eigen::VectorXd final_e = target - current_best;
    double final_te_sq = final_e.transpose() * cov * final_e;
    double final_pure_te = std::sqrt(std::max(0.0, final_te_sq));
    Eigen::VectorXd final_trade_diff = current_best - actual_eigen;
    double final_cost =
        (final_trade_diff.cwiseAbs().array() * costs_eigen.array()).sum() * cfg.cost_penalty_scalar;
    std::vector<double> pos(n);
    for (size_t i = 0; i < n; ++i)
        pos[i] = current_best(i);
    return RefResult{pos, final_pure_te + final_cost, final_cost, iteration, !improved};
}

// The production shape: max_iterations 100 (config_template/defaults.json), cost_penalty_scalar 50,
// convergence_threshold 1e-6, buffering off (the greedy alone).
DynamicOptConfig production_config() {
    DynamicOptConfig c;
    c.tau = 1.0;
    c.capital = 500000.0;
    c.cost_penalty_scalar = 50.0;
    c.asymmetric_risk_buffer = 0.1;
    c.max_iterations = 100;
    c.convergence_threshold = 1e-6;
    c.use_buffering = false;
    c.buffer_size_factor = 0.05;
    return c;
}

struct Book {
    std::vector<double> current, target, costs, w;
    std::vector<std::vector<double>> cov;
};

// A 36-symbol book in the engine's units: weight per contract = notional / capital, covariance of
// daily-annualised returns with a three-factor correlation (positive definite), cost per weight
// unit of a few basis points. Targets in contracts are scaled so that sum |target| = l1_contracts,
// with short_fraction of it on short asks; the held book is the target rounded, perturbed.
Book make_book(unsigned seed, double l1_contracts, double short_fraction, bool whole_contracts) {
    const size_t n = 36;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    Book b;
    b.w.resize(n);
    std::vector<double> sigma(n);
    for (size_t i = 0; i < n; ++i) {
        b.w[i] = 0.005 + 0.145 * u01(rng);
        sigma[i] = 0.05 + 0.35 * u01(rng);
    }
    std::vector<std::array<double, 3>> load(n);
    std::vector<double> idio(n);
    for (size_t i = 0; i < n; ++i) {
        for (auto& x : load[i])
            x = 2.0 * u01(rng) - 1.0;
        idio[i] = 0.2 + 0.8 * u01(rng);
    }
    b.cov.assign(n, std::vector<double>(n, 0.0));
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j) {
            double s = 0.0;
            for (int k = 0; k < 3; ++k)
                s += load[i][k] * load[j][k];
            if (i == j)
                s += idio[i];
            b.cov[i][j] = s;
        }
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j)
            if (i != j)
                b.cov[i][j] =
                    b.cov[i][j] / std::sqrt(b.cov[i][i] * b.cov[j][j]) * sigma[i] * sigma[j];
    for (size_t i = 0; i < n; ++i)
        b.cov[i][i] = sigma[i] * sigma[i];
    std::vector<double> raw(n);
    double sum_abs = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double mag = u01(rng);
        raw[i] = (u01(rng) < 0.25 ? -1.0 : 1.0) * mag;
    }
    double long_sum = 0.0, short_sum = 0.0;
    for (double r : raw)
        (r > 0 ? long_sum : short_sum) += std::abs(r);
    std::vector<double> contracts(n);
    for (size_t i = 0; i < n; ++i) {
        double c = raw[i] > 0 ? raw[i] / long_sum * l1_contracts * (1.0 - short_fraction)
                              : raw[i] / short_sum * l1_contracts * short_fraction;
        contracts[i] = whole_contracts ? std::round(c) : c;
        sum_abs += std::abs(contracts[i]);
    }
    (void)sum_abs;
    b.target.resize(n);
    b.current.resize(n);
    b.costs.resize(n);
    for (size_t i = 0; i < n; ++i) {
        b.target[i] = contracts[i] * b.w[i];
        double held = std::round(contracts[i]) + std::round(2.0 * u01(rng) - 1.0);
        b.current[i] = held * b.w[i];
        b.costs[i] = 0.0001 + 0.0004 * u01(rng);
    }
    return b;
}

double book_contracts(const std::vector<double>& pos, const std::vector<double>& w) {
    double s = 0.0;
    for (size_t i = 0; i < pos.size(); ++i)
        s += std::abs(pos[i] / w[i]);
    return s;
}

// T-VOL's 2025-10-07 target at 5x capital: 284 contracts, 244 long + 40 short. With unit weights,
// identity covariance and no cost the greedy's answer is the target itself after 284 adopted steps
// and one empty pass. The parent stops at 100 contracts (iterations 101, not converged).
TEST(OptN1GreedyCap, TargetOf284ContractsIsReachedNotTruncatedAt100) {
    DynamicOptConfig cfg = production_config();
    DynamicOptimizer opt(cfg);
    const size_t n = 36;
    std::vector<double> target(n, 0.0);
    // 244 long over 26 symbols (10 x 10 + 16 x 9), 40 short over 10 symbols (4 each)
    for (size_t i = 0; i < 10; ++i)
        target[i] = 10.0;
    for (size_t i = 10; i < 26; ++i)
        target[i] = 9.0;
    for (size_t i = 26; i < 36; ++i)
        target[i] = -4.0;
    double l1 = 0.0;
    for (double t : target)
        l1 += std::abs(t);
    ASSERT_EQ(l1, 284.0);
    std::vector<std::vector<double>> cov(n, std::vector<double>(n, 0.0));
    for (size_t i = 0; i < n; ++i)
        cov[i][i] = 1.0;
    auto r =
        opt.optimize_single_period(std::vector<double>(n, 0.0), target, std::vector<double>(n, 0.0),
                                   std::vector<double>(n, 1.0), cov);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().positions, target);
    EXPECT_EQ(r.value().iterations, 285);  // 284 adopted steps + the empty pass
    EXPECT_TRUE(r.value().converged);
    EXPECT_NEAR(r.value().tracking_error, 0.0, 1e-12);
}

// A correlated, costed 36-symbol book of about 284 contracts (40 short), production config: the
// answer is exactly the parent's greedy with its cap lifted (same picks, same order, same passes),
// and the parent at cap 100 is truncated on the same inputs (so this book exercises the defect).
TEST(OptN1GreedyCap, LargeCorrelatedBookEqualsTheUncappedParentGreedy) {
    DynamicOptConfig cfg = production_config();
    DynamicOptimizer opt(cfg);
    for (unsigned seed : {11u, 29u, 71u}) {
        Book b = make_book(seed, 284.0, 40.0 / 284.0, /*whole_contracts=*/false);
        RefResult capped = parent_greedy(cfg, 100, b.current, b.target, b.costs, b.w, b.cov);
        RefResult uncapped = parent_greedy(cfg, std::numeric_limits<int>::max() - 1, b.current,
                                           b.target, b.costs, b.w, b.cov);
        ASSERT_EQ(capped.iterations, 101) << "seed " << seed;  // the parent stops at the cap
        ASSERT_FALSE(capped.converged) << "seed " << seed;
        ASSERT_TRUE(uncapped.converged) << "seed " << seed;
        ASSERT_GT(uncapped.iterations, 101) << "seed " << seed;

        auto r = opt.optimize_single_period(b.current, b.target, b.costs, b.w, b.cov);
        ASSERT_TRUE(r.is_ok());
        EXPECT_EQ(r.value().positions, uncapped.positions) << "seed " << seed;
        EXPECT_EQ(r.value().iterations, uncapped.iterations) << "seed " << seed;
        EXPECT_TRUE(r.value().converged) << "seed " << seed;
        EXPECT_DOUBLE_EQ(r.value().tracking_error, uncapped.tracking_error) << "seed " << seed;
        EXPECT_DOUBLE_EQ(r.value().cost_penalty, uncapped.cost_penalty) << "seed " << seed;
        EXPECT_GT(book_contracts(r.value().positions, b.w), 100.5) << "seed " << seed;
    }
}

// Today's size (T-VOL 1x: at most 52 target contracts, at most 51 passes): on books the parent
// never capped, the answer AND the pass count are the parent's at the configured cap of 100.
// Passes on the parent and on the fix (a pin, not a red test).
TEST(OptN1GreedyCap, SmallBookGivesTheParentsAnswerAndPassCount) {
    DynamicOptConfig cfg = production_config();
    DynamicOptimizer opt(cfg);
    int books = 0;
    for (unsigned seed = 1; seed <= 40; ++seed) {
        double l1 = 8.0 + 44.0 * (seed % 11) / 10.0;  // 8 .. 52 contracts
        Book b = make_book(seed, l1, (seed % 4) * 0.05, /*whole_contracts=*/seed % 2 == 0);
        RefResult ref =
            parent_greedy(cfg, cfg.max_iterations, b.current, b.target, b.costs, b.w, b.cov);
        ASSERT_TRUE(ref.converged) << "seed " << seed;  // the parent was not capped here
        ASSERT_LE(ref.iterations, 100) << "seed " << seed;
        auto r = opt.optimize_single_period(b.current, b.target, b.costs, b.w, b.cov);
        ASSERT_TRUE(r.is_ok());
        EXPECT_EQ(r.value().positions, ref.positions) << "seed " << seed;
        EXPECT_EQ(r.value().iterations, ref.iterations) << "seed " << seed;
        EXPECT_EQ(r.value().converged, ref.converged) << "seed " << seed;
        EXPECT_DOUBLE_EQ(r.value().tracking_error, ref.tracking_error) << "seed " << seed;
        EXPECT_DOUBLE_EQ(r.value().cost_penalty, ref.cost_penalty) << "seed " << seed;
        ++books;
    }
    EXPECT_EQ(books, 40);
}

}  // namespace opt_n1_detail
