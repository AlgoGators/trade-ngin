// Tests for MarketMSAR — transition matrix recomputation from smoothed
// posteriors during fit().

#include "../../src/models/autoregression/msar.hpp"

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <random>
#include <vector>

using namespace trade_ngin;
using namespace trade_ngin::statistics;

// ============================================================================
// MarketMSAR recomputes transition matrix from smoothed posteriors.
// We pass a deliberately-wrong transition matrix and verify the MSAR's
// stored matrix differs (proving recomputation actually fired).
// ============================================================================

TEST(RegimePhase3, MarketMSARRecomputesTransitionFromPosteriors_K06) {
    constexpr int T = 200;
    std::mt19937 rng(101);
    std::normal_distribution<double> nd(0.0, 0.01);
    Eigen::VectorXd returns(T);
    for (int t = 0; t < T; ++t) returns(t) = nd(rng);

    // Smoothed posteriors: regime 0 dominates first half, regime 1 second.
    Eigen::MatrixXd state_probs = Eigen::MatrixXd::Zero(T, 2);
    for (int t = 0; t < T; ++t) {
        if (t < T / 2) { state_probs(t, 0) = 0.9; state_probs(t, 1) = 0.1; }
        else            { state_probs(t, 0) = 0.1; state_probs(t, 1) = 0.9; }
    }

    // Pass an OBVIOUSLY-WRONG transition matrix (uniform). The
    // stored matrix should reflect the smoothed posteriors instead.
    Eigen::MatrixXd wrong_transition(2, 2);
    wrong_transition << 0.5, 0.5,
                        0.5, 0.5;

    MarketMSAR msar(/*lag=*/1);
    auto fit_result = msar.fit(returns, state_probs, wrong_transition);
    ASSERT_TRUE(fit_result.is_ok()) << fit_result.error()->what();

    const Eigen::MatrixXd& stored = msar.get_transition_matrix();
    // Posteriors are 0.9/0.1 split — the smoothed-posterior approximation
    // P(i→j) ≈ Σ γ(t,i)γ(t+1,j) / Σ γ(t,i) with one switch at midpoint
    // gives diagonal ≈ 0.81, off-diagonal ≈ 0.19 (NOT the input 0.5).
    EXPECT_GT(stored(0, 0), 0.7)
        << "recomputed P(stay in regime 0) should be substantially "
           "above 0.5 given dominant 0.9 posteriors. Got " << stored(0, 0);
    EXPECT_GT(stored(1, 1), 0.7)
        << "recomputed P(stay in regime 1) should be substantially "
           "above 0.5. Got " << stored(1, 1);
    // The deliberately-wrong input (uniform 0.5) should NOT survive: the
    // recomputed matrix must differ meaningfully from the input.
    EXPECT_LT(std::abs(stored(0, 1) - 0.5), 0.45)
        << "stored matrix should differ from the wrong 0.5 input.";
    EXPECT_GT(std::abs(stored(0, 1) - 0.5), 0.1)
        << "stored matrix shows the recomputation actually fired.";
}
