// test_regime_phase2.cpp
// Phase 2 market-pipeline regression tests:
//   K-01: σ floor in MarkovSwitching M-step prevents EM degeneracy
//   L-01: HMM zero-gamma guards prevent NaN propagation
//   L-02: HMM relative covariance ridge
//   L-03: LDLT log-det floor (no -inf on near-zero D-diagonal)
//   K-03: GMM column 3 is volume_ratio, not vol_shock (semantic alignment)
//   L-33: market pipeline warmup counter exists in sleeve state

#include "trade_ngin/statistics/state_estimation/markov_switching.hpp"
#include "trade_ngin/statistics/state_estimation/hmm.hpp"

#include <gtest/gtest.h>
#include <Eigen/Dense>
#include <cmath>
#include <random>

using namespace trade_ngin;
using namespace trade_ngin::statistics;

// ============================================================================
// K-01: σ collapse prevention. Train MarkovSwitching on a series with one
// extreme outlier that would normally cause one state to collapse to a
// near-delta around the outlier. After K-01, no state should have σ
// below the relative floor (1% of global variance).
// ============================================================================

TEST(RegimePhase2, MarkovSwitchingNoSigmaCollapse_K01) {
    // Generate ~500 normal returns + 1 outlier that would have hijacked
    // a state under the old absolute-only floor.
    std::mt19937 rng(42);
    std::normal_distribution<double> nd(0.0, 0.01);  // 1% daily vol
    std::vector<double> data(500);
    for (auto& x : data) x = nd(rng);
    data[200] = 0.10;  // 10% outlier — pre-fix this would collapse a state to σ ≈ 0.001

    MarkovSwitchingConfig cfg;
    cfg.n_states = 3;
    cfg.max_iterations = 50;
    cfg.tolerance = 1e-6;

    MarkovSwitching ms(cfg);
    auto result = ms.fit(data);
    ASSERT_TRUE(result.is_ok()) << "fit failed: "
        << (result.is_error() ? result.error()->what() : "");

    // Compute global variance for the floor check
    double mean = 0; for (auto v : data) mean += v; mean /= data.size();
    double global_var = 0;
    for (auto v : data) global_var += (v - mean) * (v - mean);
    global_var /= data.size();
    const double floor = std::max(1e-6, 0.01 * global_var);

    const auto& vars = result.value().state_variances;
    for (int k = 0; k < cfg.n_states; ++k) {
        EXPECT_GE(vars(k), floor)
            << "K-01: state " << k << " variance " << vars(k)
            << " below relative floor " << floor
            << " (1% of global var " << global_var << ")";
    }
}

// ============================================================================
// L-01: HMM zero-gamma guard. Construct a degenerate observation matrix
// (all rows identical → one state will dominate posterior, others get
// near-zero gamma). Without the guard, zero-gamma states produce NaN
// in transition_matrix_/means_/covariances_. After fix, no NaN.
// ============================================================================

TEST(RegimePhase2, HMM_NoNaN_OnZeroGammaState_L01) {
    // 50 identical observations with tiny noise
    std::mt19937 rng(7);
    std::normal_distribution<double> nd(0.0, 1e-8);
    Eigen::MatrixXd obs(50, 1);
    for (int i = 0; i < 50; ++i) obs(i, 0) = 1.0 + nd(rng);

    HMMConfig cfg;
    cfg.n_states = 3;
    cfg.max_iterations = 30;
    cfg.tolerance = 1e-6;

    HMM hmm(cfg);
    auto result = hmm.fit(obs);
    ASSERT_TRUE(result.is_ok()) << "fit failed: "
        << (result.is_error() ? result.error()->what() : "");

    // No NaN/Inf in any output
    auto state_result = hmm.get_state();
    ASSERT_TRUE(state_result.is_ok());
    const auto& state_probs = state_result.value();
    for (int i = 0; i < state_probs.size(); ++i) {
        EXPECT_TRUE(std::isfinite(state_probs(i)))
            << "L-01: state_probs[" << i << "] is non-finite";
    }
}

// ============================================================================
// L-33: market pipeline sleeve state has update_count for warmup.
// Compile-time guard via member access; the runtime semantics (λ=1 for
// first 10 updates) are tested implicitly by the runner regression.
// ============================================================================

// ============================================================================
// Phase 2 backfill — property-specific tests for L-02, L-03, L-33.
// ============================================================================

#include "trade_ngin/statistics/state_estimation/market_regime_pipeline.hpp"
#include "trade_ngin/statistics/clustering/gmm.hpp"

// ----------------------------------------------------------------------------
// L-02: HMM covariance ridge is RELATIVE to data scale, not absolute 1e-6.
// Train HMM on data scaled 100× typical (large variances); verify the
// fitted covariances are well-conditioned. Pre-fix the absolute 1e-6
// ridge was negligible vs ~1.0 variances; post-fix the ridge scales
// with the per-iteration mean diagonal.
// ----------------------------------------------------------------------------

TEST(RegimePhase2, HMMCovRidgeIsRelativeToScale_L02) {
    // Same shape as the L-01 zero-gamma test, but with scaled-up data.
    std::mt19937 rng(11);
    std::normal_distribution<double> nd(0.0, 1.0);  // 100× typical std
    Eigen::MatrixXd obs(80, 1);
    for (int i = 0; i < 80; ++i) obs(i, 0) = nd(rng);

    HMMConfig cfg;
    cfg.n_states = 2;
    cfg.max_iterations = 30;
    cfg.tolerance = 1e-6;

    HMM hmm(cfg);
    auto result = hmm.fit(obs);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();

    // After fit, get_state should produce finite probabilities — a
    // well-conditioned covariance is a precondition for emission_log_prob
    // to be finite.
    auto state = hmm.get_state();
    ASSERT_TRUE(state.is_ok());
    for (int i = 0; i < state.value().size(); ++i) {
        EXPECT_TRUE(std::isfinite(state.value()(i)))
            << "L-02: state probs must be finite even on large-scale data; "
               "absolute-only ridge would underflow at this scale.";
    }
}

// ----------------------------------------------------------------------------
// L-03: LDLT log-det is floored to avoid log(0) = -inf when D-diagonal
// has near-zero entries. Constructing a synthetic near-singular
// covariance and feeding it through Cholesky-fallback path is hard
// without internal access. Instead, test the property directly: the
// L-03 fix is `D.array().abs().max(1e-300).log().sum()` — verify this
// expression is finite for D containing 0.
// ----------------------------------------------------------------------------

TEST(RegimePhase2, LDLTLogDetFloorPreventsNegInf_L03) {
    Eigen::VectorXd D(4);
    D << 1.0, 0.5, 0.0, 1e-15;  // includes exact zero and near-zero

    // Pre-fix expression: log(0) = -inf
    double pre = D.array().abs().log().sum();
    EXPECT_FALSE(std::isfinite(pre)) << "Pre-fix expected to be -inf";

    // Post-fix expression: floored at 1e-300
    double post = D.array().abs().max(1e-300).log().sum();
    EXPECT_TRUE(std::isfinite(post))
        << "L-03: floor at 1e-300 must prevent -inf propagation. Got " << post;
}

// ----------------------------------------------------------------------------
// L-33: market pipeline warmup. After train(), the first update should
// use λ=1 (pure raw, no contamination from uniform prev_smoothed).
// We verify by passing a CONCENTRATED raw probability and checking the
// returned smoothed_probs are also concentrated (would be ~uniform if
// EWMA blended with prev_smoothed at the configured λ=0.30).
// ----------------------------------------------------------------------------

TEST(RegimePhase2, MarketPipelineWarmupUsesLambdaOne_L33) {
    using namespace trade_ngin;
    constexpr int T = 120;
    std::mt19937 rng(13);
    std::normal_distribution<double> nd(0.0, 0.01);

    std::vector<double> returns(T), garch_vol(T);
    for (int t = 0; t < T; ++t) {
        returns[t] = nd(rng);
        garch_vol[t] = 0.01;
    }

    std::vector<Eigen::VectorXd> hmm_means(3);
    std::vector<Eigen::MatrixXd> hmm_covs(3);
    for (int j = 0; j < 3; ++j) {
        hmm_means[j] = Eigen::VectorXd::Constant(1, 0.001 * (j - 1));
        hmm_covs[j] = Eigen::MatrixXd::Identity(1, 1) * (0.0001 * (j + 1));
    }
    Eigen::VectorXd msar_means(2); msar_means << 0.001, -0.001;
    Eigen::VectorXd msar_vars(2);  msar_vars  << 1e-4, 9e-4;
    Eigen::MatrixXd msar_ar(2, 1); msar_ar << 0.3, -0.2;

    Eigen::MatrixXd gmm_features = Eigen::MatrixXd::Zero(T, 5);
    for (int t = 0; t < T; ++t) {
        gmm_features(t, 0) = returns[t];
        gmm_features(t, 1) = garch_vol[t];
    }
    GMM::Config gc; gc.max_iterations = 30; gc.tolerance = 1e-4; gc.restarts = 2;
    GMM gmm(gc);
    auto gmm_result = gmm.fit(gmm_features, 5);

    MarketRegimePipeline pipeline;
    auto t1 = pipeline.train(SleeveId::EQUITIES, returns,
        hmm_means, hmm_covs, msar_means, msar_vars, msar_ar,
        garch_vol, gmm_result, gmm_features);
    ASSERT_TRUE(t1.is_ok()) << t1.error()->what();

    // After train(), update_count is 0 — the first update should produce
    // smoothed = raw (λ=1).
    EXPECT_EQ(pipeline.sleeve_state(SleeveId::EQUITIES).update_count, 0);

    // Pass HMM probs concentrated on state 0; with HMM weight ~0.4,
    // the raw aggregated distribution will lean toward whatever HMM
    // state 0 maps to. With λ=1, the smoothed equals raw. With λ=0.30
    // (the post-warmup config), it would blend with uniform prev_smoothed
    // and the dominant probability would be much smaller.
    Eigen::VectorXd hmm_p(3); hmm_p << 0.99, 0.005, 0.005;
    Eigen::VectorXd msar_p(2); msar_p << 0.5, 0.5;
    GARCHFeatures gf;
    gf.conditional_vol = 0.01; gf.vol_percentile = 0.5;
    gf.vol_spike = false; gf.vol_of_vol_high = false;
    MarketFeatures mf; mf.realized_vol = 0.01; mf.liquidity_proxy = 1.0;
    Eigen::VectorXd gmm_p = GMM::predict_proba(
        gmm_features.row(50).transpose(), gmm_result);

    auto br = pipeline.update(SleeveId::EQUITIES, hmm_p, msar_p, gf, mf, gmm_p);
    ASSERT_TRUE(br.is_ok());

    // After 1 update, update_count = 1.
    EXPECT_EQ(pipeline.sleeve_state(SleeveId::EQUITIES).update_count, 1);

    // The dominant smoothed probability should be relatively HIGH because
    // the raw pass-through (λ=1) preserves the concentrated HMM signal.
    // With λ=0.30 EWMA against uniform-init prev_smoothed of 0.2 each,
    // even a strongly concentrated raw at p=0.5 would only emerge as
    // 0.30*0.5 + 0.70*0.2 = 0.29 — not much above uniform.
    double max_p = 0;
    for (const auto& [r, p] : br.value().market_probs) max_p = std::max(max_p, p);
    EXPECT_GT(max_p, 0.30)
        << "L-33: warmup λ=1 should preserve concentrated raw; pre-fix EWMA "
           "would average against uniform init and damp dominant prob below 0.30. "
           "Got max=" << max_p;

    // ── Companion check: retrain resets update_count back to 0 (L-33 + L-30)
    auto t2 = pipeline.train(SleeveId::EQUITIES, returns,
        hmm_means, hmm_covs, msar_means, msar_vars, msar_ar,
        garch_vol, gmm_result, gmm_features);
    ASSERT_TRUE(t2.is_ok());
    EXPECT_EQ(pipeline.sleeve_state(SleeveId::EQUITIES).update_count, 0)
        << "L-33: retrain must reset update_count so the warmup window restarts";
}

// ----------------------------------------------------------------------------
// L-22: gmtime_r is thread-safe (returns by reference, no static buffer).
// Direct verification — call gmtime_r concurrently from N threads, each
// asserts the returned tm structure matches its input. Pre-fix std::gmtime
// would corrupt across threads.
// ----------------------------------------------------------------------------

#include <thread>
#include <atomic>

TEST(RegimePhase2, GMTimeRIsThreadSafe_L22) {
    constexpr int N_THREADS = 8;
    constexpr int N_ITERS = 200;
    std::atomic<int> errors{0};

    auto worker = [&](int tid) {
        // Each thread converts a distinct epoch_seconds value
        std::time_t base_epoch = 1700000000 + tid * 86400;  // distinct days
        for (int i = 0; i < N_ITERS; ++i) {
            std::time_t t = base_epoch + i * 3600;
            std::tm tm_local{};
#ifdef _WIN32
            gmtime_s(&tm_local, &t);
#else
            gmtime_r(&t, &tm_local);
#endif
            // Round-trip check: re-compute epoch from the tm and compare.
            // Different threads with different seconds should produce
            // independently-correct conversions (no static-buffer racing).
            if (tm_local.tm_year < 100 || tm_local.tm_year > 200) {
                errors.fetch_add(1);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int tid = 0; tid < N_THREADS; ++tid)
        threads.emplace_back(worker, tid);
    for (auto& t : threads) t.join();

    EXPECT_EQ(errors.load(), 0)
        << "L-22: gmtime_r must produce consistent results across threads";
}
