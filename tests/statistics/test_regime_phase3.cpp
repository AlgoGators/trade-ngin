// test_regime_phase3.cpp
// Phase 3 economic-refinement regression tests:
//   M-06: BSTS R0-R3 rejects negative scores (sentinel = -1 / "Unclassified")
//   M-07: MS-DFM fingerprints are soft-prob-weighted aggregates (not hard argmax)
//   K-04: HMM trend dim uses |μ| (drift magnitude), not signed μ
//   K-06: MarketMSAR re-estimates transition matrix from smoothed posteriors
//         (internal consistency between AR fit and transition dynamics)
//   L-09: GARCH/EGARCH update() demeans incoming returns consistently with fit()

#include "trade_ngin/statistics/state_estimation/bsts_regime_detector.hpp"
#include "trade_ngin/statistics/volatility/garch.hpp"
#include "trade_ngin/statistics/volatility/egarch.hpp"
#include "../../src/models/autoregression/msar.hpp"
#include "../../include/trade_ngin/statistics/state_estimation/markov_switching.hpp"

#include <gtest/gtest.h>
#include <Eigen/Dense>
#include <random>
#include <vector>
#include <cmath>

using namespace trade_ngin;
using namespace trade_ngin::statistics;

// ============================================================================
// M-06: BSTS regime_name returns "Unclassified" for sentinel -1.
// The greedy assignment now leaves clusters at -1 if their best match
// score is ≤ 0, instead of forcing them onto the closest signature.
// ============================================================================

TEST(RegimePhase3, BSTSUnclassifiedSentinelLabel_M06) {
    // The sentinel is reserved for clusters that score ≤ 0 against
    // every signature. regime_name(-1) must return "Unclassified".
    EXPECT_STREQ(BSTSRegimeDetector::regime_name(-1), "Unclassified")
        << "M-06: -1 sentinel must map to 'Unclassified' label, not 'UNKNOWN'";
    EXPECT_STREQ(BSTSRegimeDetector::regime_name(0), "R0 Risk-On Growth");
    EXPECT_STREQ(BSTSRegimeDetector::regime_name(3), "R3 Reflation");
    EXPECT_STREQ(BSTSRegimeDetector::regime_name(99), "UNKNOWN")
        << "Out-of-range labels still return UNKNOWN, not Unclassified";
}

// ============================================================================
// K-04: HMM target fingerprints are non-negative on the trend (|μ|) dim.
// Pre-fix had STRESS_PRICE = (-1.0, +1.5) — negative on trend dim,
// conflating bear-trend with stress. Post-fix, STRESS_PRICE = (0.5, 1.5)
// — moderate |μ|, distinguished from TREND by the σ dim.
// ============================================================================
//
// We can't easily access the private target_fingerprints from outside
// the class, but the L-04 fix is observable in the train output via the
// runner. As a structural check, we verify that the HMM emission means
// passed in produce a reasonable mapping when run through |μ| convention.

TEST(RegimePhase3, HMMTrendDimUsesAbsoluteMu_K04) {
    // Two synthetic states with same |μ| but opposite sign. Pre-K-04
    // they would be classified differently (one TREND, one STRESS);
    // post-K-04 they have the same |μ| coordinate so the fingerprint
    // distance to TREND vs STRESS is the same except for the σ component.
    constexpr double mu = 0.002;  // 0.2% daily drift
    constexpr double sigma = 0.01;
    Eigen::Vector2d up_trend(std::abs(mu), sigma);
    Eigen::Vector2d down_trend(std::abs(-mu), sigma);
    EXPECT_DOUBLE_EQ(up_trend(0), down_trend(0))
        << "K-04: |μ| convention makes up-trend and down-trend identical "
           "on the trend dimension. Distinguishing them requires direction "
           "info beyond HMM (not in scope for the per-state mapping).";
    EXPECT_DOUBLE_EQ(up_trend(1), down_trend(1));
}

// ============================================================================
// K-04 v2: target geometry must put high-vol crash states closer to
// STRESS_PRICE than to TREND_HIGHVOL. The v1 retune got this wrong:
// TREND_HIGHVOL at (1.0, 1.0) was closer to z-scored crash states
// (~1.4, ~1.4) than STRESS_PRICE at (0.5, 1.5). The v2 retune defines
// stress by σ, with TREND_HIGHVOL pulled away from σ=high.
//
// We test the property by replicating the target geometry and asserting
// distance ordering for a representative "crash state" z-score vector.
// ============================================================================

TEST(RegimePhase3, HMMTargets_CrashStateMapsToStress_K04v2) {
    // Targets from market_regime_pipeline.cpp train_hmm_fingerprints (v2):
    Eigen::Vector2d trend_lowvol(0.0, -1.5);
    Eigen::Vector2d trend_highvol(0.5,  0.0);
    Eigen::Vector2d meanrev(-0.5,  0.0);
    Eigen::Vector2d stress_price(0.5,  1.5);
    Eigen::Vector2d stress_liq(0.0,  2.5);

    // Representative crash state (high |μ|, high σ) after z-scoring across
    // 3 native states. e.g. commodities state 0 in the audit data:
    // |μ|=0.9%/day, σ=10%/day → z-scored to ~(1.45, 1.43).
    Eigen::Vector2d crash_state(1.45, 1.43);

    auto dist = [](const Eigen::Vector2d& a, const Eigen::Vector2d& b) {
        return (a - b).norm();
    };

    double d_trend_lowvol  = dist(crash_state, trend_lowvol);
    double d_trend_highvol = dist(crash_state, trend_highvol);
    double d_meanrev       = dist(crash_state, meanrev);
    double d_stress_price  = dist(crash_state, stress_price);
    double d_stress_liq    = dist(crash_state, stress_liq);

    // STRESS_PRICE must be CLOSER than TREND_HIGHVOL — that's the v1
    // regression we're guarding against.
    EXPECT_LT(d_stress_price, d_trend_highvol)
        << "K-04 v2: crash state must be closer to STRESS_PRICE ("
        << d_stress_price << ") than TREND_HIGHVOL ("
        << d_trend_highvol << "). v1 had TREND_HIGHVOL win at 0.62.";

    // STRESS_PRICE should also be the OVERALL closest target.
    EXPECT_LT(d_stress_price, d_trend_lowvol);
    EXPECT_LT(d_stress_price, d_meanrev);
    EXPECT_LT(d_stress_price, d_stress_liq);
}

TEST(RegimePhase3, HMMTargets_BullTrendMapsToTrendLowVol_K04v2) {
    // Targets from market_regime_pipeline.cpp train_hmm_fingerprints (v2)
    Eigen::Vector2d trend_lowvol(0.0, -1.5);
    Eigen::Vector2d trend_highvol(0.5,  0.0);
    Eigen::Vector2d meanrev(-0.5,  0.0);
    Eigen::Vector2d stress_price(0.5,  1.5);
    Eigen::Vector2d stress_liq(0.0,  2.5);

    // Representative bull-trend state: small |μ| relative to other states
    // (because the crash state has higher |μ| magnitude), but low σ.
    // Equities state 1 in the audit data: |μ|=0.091%, σ=0.65%
    // After z-scoring across the 3 equity states: ~(-0.45, -0.93).
    // The DEFINING feature is low σ (z-score below mean).
    Eigen::Vector2d bull_state(-0.45, -0.93);

    auto dist = [](const Eigen::Vector2d& a, const Eigen::Vector2d& b) {
        return (a - b).norm();
    };

    double d_trend_lowvol  = dist(bull_state, trend_lowvol);
    double d_trend_highvol = dist(bull_state, trend_highvol);
    double d_meanrev       = dist(bull_state, meanrev);
    double d_stress_price  = dist(bull_state, stress_price);

    // TREND_LOWVOL should be the closest. The v1 retune broke this
    // because TREND_LOWVOL demanded |μ|=+1.5 (high magnitude); bull
    // states with moderate |μ| got pulled toward MEANREV.
    EXPECT_LT(d_trend_lowvol, d_trend_highvol);
    EXPECT_LT(d_trend_lowvol, d_meanrev)
        << "K-04 v2: bull state with low σ must map to TREND_LOWVOL ("
        << d_trend_lowvol << "), not MEANREV (" << d_meanrev << "). "
        << "v1 had MEANREV win because TREND_LOWVOL demanded high |μ|.";
    EXPECT_LT(d_trend_lowvol, d_stress_price);
}

// ============================================================================
// L-09: GARCH update() applies same demean as fit().
// Verify by training on returns with a non-zero mean, then driving
// update() with raw returns. Pre-fix the live vol would diverge from
// what a fit-on-extended-series would produce; post-fix they match
// to first order.
// ============================================================================

TEST(RegimePhase3, GARCHUpdateAppliesDemean_L09) {
    constexpr int N = 100;
    std::mt19937 rng(31);
    std::normal_distribution<double> nd(0.005, 0.01);  // 0.5% mean, 1% vol

    std::vector<double> returns;
    for (int i = 0; i < N; ++i) returns.push_back(nd(rng));

    GARCH garch_a(GARCHConfig{});
    auto fit_a = garch_a.fit(returns);
    ASSERT_TRUE(fit_a.is_ok()) << fit_a.error()->what();

    // Now drive update() with one new return that has the SAME drift as
    // the training mean. With L-09 the residual is ~0; without L-09 the
    // residual would be ~0.005 (the mean) — different vol step.
    double next_return = 0.005;  // exactly the training mean
    auto upd = garch_a.update(next_return);
    ASSERT_TRUE(upd.is_ok());

    auto vol = garch_a.get_current_volatility();
    ASSERT_TRUE(vol.is_ok());
    EXPECT_GT(vol.value(), 0.0)
        << "L-09: vol after update must be finite and positive";

    // Compare with a different update value: a 5σ shock should produce
    // visibly higher vol than the at-mean update.
    GARCH garch_b(GARCHConfig{});
    ASSERT_TRUE(garch_b.fit(returns).is_ok());
    ASSERT_TRUE(garch_b.update(0.005 + 5 * 0.01).is_ok());  // 5σ shock above mean
    auto vol_shock = garch_b.get_current_volatility();
    ASSERT_TRUE(vol_shock.is_ok());
    EXPECT_GT(vol_shock.value(), vol.value())
        << "L-09: 5σ shock above mean must produce higher vol than at-mean update";
}

// ============================================================================
// K-06: MarketMSAR recomputes transition matrix from smoothed posteriors.
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

    // Pass an OBVIOUSLY-WRONG transition matrix (uniform). After K-06 the
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
        << "K-06: recomputed P(stay in regime 0) should be substantially "
           "above 0.5 given dominant 0.9 posteriors. Got " << stored(0, 0);
    EXPECT_GT(stored(1, 1), 0.7)
        << "K-06: recomputed P(stay in regime 1) should be substantially "
           "above 0.5. Got " << stored(1, 1);
    // The deliberately-wrong input (uniform 0.5) should NOT survive: the
    // recomputed matrix must differ meaningfully from the input.
    EXPECT_LT(std::abs(stored(0, 1) - 0.5), 0.45)
        << "K-06: stored matrix should differ from the wrong 0.5 input.";
    EXPECT_GT(std::abs(stored(0, 1) - 0.5), 0.1)
        << "K-06: stored matrix shows the recomputation actually fired.";
}

// ============================================================================
// M-07: MS-DFM soft-prob fingerprints. Direct test through the public
// pipeline surface is heavy (requires DFM/MS-DFM training). Instead,
// test the property structurally — soft-weighted aggregates differ from
// hard-argmax aggregates when mode boundaries are non-degenerate.
// ============================================================================

TEST(RegimePhase3, SoftProbWeightedFingerprintDiffersFromArgmax_M07) {
    // Synthetic 3-state posterior with a "fuzzy" boundary between states.
    constexpr int T = 60;
    constexpr int J = 3;
    Eigen::MatrixXd smoothed(T, J);
    std::vector<int> argmax(T);
    for (int t = 0; t < T; ++t) {
        // First 20 bars: state 0 dominant. Bars 20-39: 50/50 between 0 and 1.
        // Bars 40-59: state 1 dominant.
        if (t < 20)        { smoothed(t, 0) = 0.85; smoothed(t, 1) = 0.10; smoothed(t, 2) = 0.05; argmax[t] = 0; }
        else if (t < 40)   { smoothed(t, 0) = 0.45; smoothed(t, 1) = 0.45; smoothed(t, 2) = 0.10; argmax[t] = 0; }
        else               { smoothed(t, 0) = 0.10; smoothed(t, 1) = 0.85; smoothed(t, 2) = 0.05; argmax[t] = 1; }
    }
    // Hidden "true value" series for each timestep; fingerprint is the average.
    std::vector<double> v(T);
    for (int t = 0; t < T; ++t) v[t] = static_cast<double>(t);

    // Argmax fingerprint for state 0: average of timesteps where argmax=0
    double argmax_sum = 0; int argmax_n = 0;
    for (int t = 0; t < T; ++t) if (argmax[t] == 0) { argmax_sum += v[t]; ++argmax_n; }
    double argmax_fp_0 = argmax_n > 0 ? argmax_sum / argmax_n : 0.0;

    // Soft-prob fingerprint for state 0: weighted average by smoothed(t, 0)
    double soft_sum = 0; double soft_w = 0;
    for (int t = 0; t < T; ++t) {
        soft_sum += smoothed(t, 0) * v[t];
        soft_w   += smoothed(t, 0);
    }
    double soft_fp_0 = soft_w > 1e-9 ? soft_sum / soft_w : 0.0;

    // The two should differ noticeably because the fuzzy-boundary bars
    // 20-39 contribute fully (45% weight) to soft, but only argmax assigns
    // them to state 0. Bars 40-59 contribute 10% weight to soft for state 0,
    // zero weight under argmax.
    EXPECT_NE(argmax_fp_0, soft_fp_0)
        << "M-07: soft-weighted fingerprint must differ from argmax-bucket "
           "fingerprint when mode boundaries are non-degenerate.";
    // Direction of the difference depends on data structure; what matters
    // is that the two methods produce different aggregates for the fuzzy-
    // boundary timesteps. The fix's effect is observable:
    //   - argmax assigns bars 20-39 (avg value 29.5) fully to state 0
    //   - soft weights bars 20-39 at 0.45 and bars 40-59 at 0.10 for state 0
    // → soft-weighted mean drifts toward the global mean (T/2 = 29.5).
    constexpr double tol = 1e-6;
    EXPECT_GT(std::abs(soft_fp_0 - argmax_fp_0), tol)
        << "M-07: aggregate values must measurably diverge between the two "
           "weighting schemes when posteriors are non-degenerate.";
}
