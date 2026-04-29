// Tests for GMM clustering — K=1 entropy guard and determinism under fixed seed.

#include "trade_ngin/statistics/clustering/gmm.hpp"

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <cmath>
#include <random>

using namespace trade_ngin;
using namespace trade_ngin::statistics;

// ============================================================================
// GMM K=1 entropy is 0 (no log(K)=0 divide-by-zero NaN).
// ============================================================================

TEST(RegimePhase4, GMMSingleClusterEntropyIsZero_L10) {
    // Trivial 2D dataset, fit K=1 — there's no uncertainty, so
    // normalised Shannon entropy must be exactly 0 (not NaN).
    std::mt19937 rng(7);
    std::normal_distribution<double> g(0.0, 1.0);
    const int n = 50, d = 2;
    Eigen::MatrixXd X(n, d);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < d; ++j)
            X(i, j) = g(rng);

    GMM::Config cfg;
    cfg.restarts = 1;
    cfg.max_iterations = 20;
    GMM gmm(cfg);
    auto result = gmm.fit(X, 1, /*seed=*/7);

    ASSERT_EQ(result.entropy.size(), n);
    for (int i = 0; i < n; ++i) {
        EXPECT_FALSE(std::isnan(result.entropy(i)))
            << "K=1 entropy must not be NaN (was log(1)=0 in denominator).";
        EXPECT_DOUBLE_EQ(result.entropy(i), 0.0)
            << "K=1 entropy is by definition 0.";
    }
}

// ============================================================================
// Adversarial: GMM determinism under fixed (X, K, seed). The rng-per-restart
// change was deliberately dropped because changing the init pattern
// across restarts perturbs the converged solution. But the existing
// determinism property — same input + same seed → same fit — must
// still hold, otherwise downstream regime calls would silently shift
// across runs.
// ============================================================================

TEST(RegimePhase4, GMMDeterminismUnderFixedSeed_L11Adversarial) {
    std::mt19937 data_rng(99);
    std::normal_distribution<double> g(0.0, 1.0);
    const int n = 80, d = 3;
    Eigen::MatrixXd X(n, d);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < d; ++j) X(i, j) = g(data_rng);

    GMM::Config cfg;
    cfg.restarts = 3;
    cfg.max_iterations = 30;
    GMM gmm(cfg);

    auto a = gmm.fit(X, 3, /*seed=*/777);
    auto b = gmm.fit(X, 3, /*seed=*/777);

    ASSERT_EQ(a.weights.size(), b.weights.size());
    for (int j = 0; j < a.weights.size(); ++j) {
        EXPECT_DOUBLE_EQ(a.weights(j), b.weights(j))
            << "GMM determinism: weight " << j << " diverged across runs.";
    }
    ASSERT_EQ(a.labels.size(), b.labels.size());
    for (int i = 0; i < a.labels.size(); ++i) {
        EXPECT_EQ(a.labels(i), b.labels(i))
            << "GMM determinism: label " << i << " diverged across runs.";
    }
}
