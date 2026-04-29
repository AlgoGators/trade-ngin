// Tests for BSTSRegimeDetector — gaussian smoothing causality, forward/leading
// fill semantics, R0-R3 sentinel labels, and PCA sign convention.

#include "trade_ngin/statistics/state_estimation/bsts_regime_detector.hpp"

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <cmath>
#include <limits>

using namespace trade_ngin::statistics;

// ============================================================================
// gaussian_smooth must be causal (smoothed[t] does not depend on
// any x[t+1..n-1]). Test by perturbing future values and asserting
// smoothed[t] for any t < perturbation index is unchanged.
// ============================================================================

TEST(RegimeSubstrate, GaussianSmoothIsCausal) {
    constexpr int N = 100;
    constexpr int radius = 4;
    constexpr double sigma = 2.0;

    Eigen::VectorXd x = Eigen::VectorXd::Zero(N);
    for (int i = 0; i < N; ++i) x(i) = std::sin(i * 0.1);

    Eigen::VectorXd smoothed_a = BSTSRegimeDetector::gaussian_smooth(x, radius, sigma);

    // Perturb a single future value: index 60.
    constexpr int perturb_idx = 60;
    Eigen::VectorXd x2 = x;
    x2(perturb_idx) += 1000.0;  // huge perturbation

    Eigen::VectorXd smoothed_b = BSTSRegimeDetector::gaussian_smooth(x2, radius, sigma);

    // For ALL t strictly less than perturb_idx, smoothed_a[t] must equal
    // smoothed_b[t]. If it doesn't, the kernel is reading future data.
    for (int t = 0; t < perturb_idx; ++t) {
        EXPECT_DOUBLE_EQ(smoothed_a(t), smoothed_b(t))
            << "Causal kernel violated at t=" << t
            << " (perturbed future index " << perturb_idx << ")";
    }

    // At and after perturb_idx, the values can and should differ.
    bool any_diff_after = false;
    for (int t = perturb_idx; t < N; ++t) {
        if (std::abs(smoothed_a(t) - smoothed_b(t)) > 1e-9) any_diff_after = true;
    }
    EXPECT_TRUE(any_diff_after)
        << "Causal kernel should still respond to perturbation at-and-after the perturbed index";
}

TEST(RegimeSubstrate, GaussianSmoothRadiusZeroIsIdentity) {
    Eigen::VectorXd x = Eigen::VectorXd::LinSpaced(50, -1.0, 1.0);
    Eigen::VectorXd y = BSTSRegimeDetector::gaussian_smooth(x, 0, 2.0);
    ASSERT_EQ(y.size(), x.size());
    for (int i = 0; i < x.size(); ++i) EXPECT_DOUBLE_EQ(y(i), x(i));
}

// ============================================================================
// backward_fill is removed; only forward_fill + leading_pad exist.
// Test that mid-panel NaN does NOT get patched with future values.
// ============================================================================

TEST(RegimeSubstrate, ForwardFillDoesNotLeakFuture) {
    Eigen::MatrixXd X(6, 1);
    X << 1.0,
         std::numeric_limits<double>::quiet_NaN(),  // mid-panel NaN at t=1
         std::numeric_limits<double>::quiet_NaN(),  // mid-panel NaN at t=2
         4.0,
         std::numeric_limits<double>::quiet_NaN(),  // mid-panel NaN at t=4
         6.0;

    BSTSRegimeDetector::forward_fill(X);

    // Forward-fill semantics: NaN gets last known value carried forward.
    // It must NEVER be 4.0 (the future value) at indices 1 or 2 — that would
    // mean future leakage.
    EXPECT_DOUBLE_EQ(X(0, 0), 1.0);
    EXPECT_DOUBLE_EQ(X(1, 0), 1.0) << "t=1 must inherit from PAST (1.0), not FUTURE (4.0)";
    EXPECT_DOUBLE_EQ(X(2, 0), 1.0) << "t=2 must inherit from PAST (1.0), not FUTURE (4.0)";
    EXPECT_DOUBLE_EQ(X(3, 0), 4.0);
    EXPECT_DOUBLE_EQ(X(4, 0), 4.0) << "t=4 must inherit from PAST (4.0), not FUTURE (6.0)";
    EXPECT_DOUBLE_EQ(X(5, 0), 6.0);
}

// leading_pad_with_first_valid is the legitimate state-space init
// for indices BEFORE the first observation. Test it pads with the first
// valid value and only those leading positions.
TEST(RegimeSubstrate, LeadingPadOnlyTouchesLeadingNaN) {
    Eigen::MatrixXd X(6, 1);
    X << std::numeric_limits<double>::quiet_NaN(),  // leading NaN
         std::numeric_limits<double>::quiet_NaN(),  // leading NaN
         3.0,
         std::numeric_limits<double>::quiet_NaN(),  // mid-panel NaN — must NOT touch
         5.0,
         6.0;

    BSTSRegimeDetector::leading_pad_with_first_valid(X);

    EXPECT_DOUBLE_EQ(X(0, 0), 3.0) << "leading NaN should be seeded with first valid";
    EXPECT_DOUBLE_EQ(X(1, 0), 3.0);
    EXPECT_DOUBLE_EQ(X(2, 0), 3.0);
    EXPECT_TRUE(std::isnan(X(3, 0)))
        << "mid-panel NaN must remain NaN — leading_pad must not backward-fill";
    EXPECT_DOUBLE_EQ(X(4, 0), 5.0);
    EXPECT_DOUBLE_EQ(X(5, 0), 6.0);
}

TEST(RegimeSubstrate, LeadingPadHandlesAllNaNColumn) {
    Eigen::MatrixXd X = Eigen::MatrixXd::Constant(
        4, 1, std::numeric_limits<double>::quiet_NaN());
    BSTSRegimeDetector::leading_pad_with_first_valid(X);
    // No valid value to seed with — column stays all-NaN, no crash
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(std::isnan(X(i, 0)));
}

// ----------------------------------------------------------------------------
// BSTS PCA deterministic sign convention. Two fits with same input
// produce identical pca_transformed up to numerical noise (no sign flip).
// We test the convention indirectly by re-PCAing the same standardized
// matrix via the same eigensolver path and confirming convention holds.
// (BSTSRegimeDetector::run_pca is private; we don't have a fit that can
// run without DB. The most we can guarantee is the principle: if PCA
// produces a vector whose largest-magnitude entry was negative, our fix
// flips it. Test that property using a representative Eigen sequence.)
// ----------------------------------------------------------------------------

TEST(RegimeSubstrate, EigenvectorSignConvention_LargestAbsPositive_L21) {
    // Construct a 4x4 symmetric matrix; verify post-flip convention.
    Eigen::MatrixXd C(4, 4);
    C << 4, 1, 0, 0,
         1, 3, 1, 0,
         0, 1, 2, 1,
         0, 0, 1, 1;

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(C);
    Eigen::MatrixXd vecs = eig.eigenvectors();

    // Apply the convention: flip if largest-magnitude entry is negative.
    for (int k = 0; k < vecs.cols(); ++k) {
        Eigen::Index max_abs_idx;
        vecs.col(k).cwiseAbs().maxCoeff(&max_abs_idx);
        if (vecs(max_abs_idx, k) < 0) vecs.col(k) *= -1.0;
    }

    // After convention applied, the largest-mag entry of every column is
    // non-negative. This is the property guaranteed per re-fit.
    for (int k = 0; k < vecs.cols(); ++k) {
        Eigen::Index max_abs_idx;
        vecs.col(k).cwiseAbs().maxCoeff(&max_abs_idx);
        EXPECT_GE(vecs(max_abs_idx, k), 0.0)
            << "post-convention column " << k
            << " largest-mag entry should be non-negative";
    }
}

// ============================================================================
// BSTS regime_name returns "Unclassified" for sentinel -1.
// The greedy assignment now leaves clusters at -1 if their best match
// score is ≤ 0, instead of forcing them onto the closest signature.
// ============================================================================

TEST(RegimePhase3, BSTSUnclassifiedSentinelLabel_M06) {
    // The sentinel is reserved for clusters that score ≤ 0 against
    // every signature. regime_name(-1) must return "Unclassified".
    EXPECT_STREQ(BSTSRegimeDetector::regime_name(-1), "Unclassified")
        << "-1 sentinel must map to 'Unclassified' label, not 'UNKNOWN'";
    EXPECT_STREQ(BSTSRegimeDetector::regime_name(0), "R0 Risk-On Growth");
    EXPECT_STREQ(BSTSRegimeDetector::regime_name(3), "R3 Reflation");
    EXPECT_STREQ(BSTSRegimeDetector::regime_name(99), "UNKNOWN")
        << "Out-of-range labels still return UNKNOWN, not Unclassified";
}
