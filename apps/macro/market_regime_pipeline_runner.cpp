// market_regime_pipeline_runner.cpp
// Market Regime Pipeline — runs per-sleeve regime detection from the database.
// Models: HMM (A1), MSAR (A2), GARCH (A3), GMM (A4)
// Following Regime Engine Algo (1).pdf
//
// Usage:
//   ./market_regime_pipeline_runner [connection_string] [start_date] [end_date]

#include "trade_ngin/statistics/state_estimation/market_regime_pipeline.hpp"
#include "trade_ngin/statistics/state_estimation/market_data_loader.hpp"
#include "trade_ngin/statistics/state_estimation/hmm.hpp"
#include "trade_ngin/statistics/state_estimation/markov_switching.hpp"
#include "trade_ngin/statistics/clustering/gmm.hpp"
#include "trade_ngin/statistics/volatility/garch.hpp"
#include "trade_ngin/statistics/volatility/egarch.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/core/types.hpp"

// MarketMSAR for proper AR(1) estimation (fix #2)
#include "../../src/models/autoregression/msar.hpp"
// Note: msar.hpp includes markov_switching.hpp which is already included above

#include <Eigen/Dense>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <cstdlib>
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace trade_ngin;
using namespace trade_ngin::statistics;

// ============================================================================
// Print helpers
// ============================================================================

static void print_belief(const MarketBelief& b) {
    std::cout << "\n  Sleeve: " << sleeve_name(b.sleeve_id)
              << "  |  Regime: " << market_regime_name(b.most_likely)
              << "  |  Confidence: " << std::fixed << std::setprecision(3) << b.confidence
              << "  |  Age: " << b.regime_age_bars << " bars\n";

    std::cout << "  ";
    for (int i = 0; i < kNumMarketRegimes; ++i) {
        auto r = static_cast<MarketRegimeL1>(i);
        auto it = b.market_probs.find(r);
        double p = (it != b.market_probs.end()) ? it->second : 0.0;
        std::cout << market_regime_name(r) << "=" << std::fixed << std::setprecision(3) << p << "  ";
    }
    std::cout << "\n";

    for (const auto& [model, probs] : b.model_contributions) {
        std::cout << "    " << std::left << std::setw(8) << model << ": ";
        for (const auto& [r, p] : probs)
            std::cout << std::fixed << std::setprecision(2) << p << " ";
        std::cout << "\n";
    }
}

// ============================================================================
// Compute shared features (A0)
// ============================================================================

static Eigen::MatrixXd compute_feature_matrix(
    const std::vector<double>& returns,
    const std::vector<double>& volumes,
    int window = 20)
{
    const int T = (int)returns.size();
    Eigen::MatrixXd features = Eigen::MatrixXd::Zero(T, 4);

    for (int t = 0; t < T; ++t) {
        if (t >= window) {
            // col 0: trend_strength (rolling autocorrelation)
            double sxy = 0, sx = 0, sy = 0, sx2 = 0, sy2 = 0;
            for (int i = 1; i < window; ++i) {
                double x = returns[t-i], y = returns[t-i+1];
                sxy += x*y; sx += x; sy += y; sx2 += x*x; sy2 += y*y;
            }
            int n = window - 1;
            double d = std::sqrt((sx2-sx*sx/n)*(sy2-sy*sy/n));
            features(t, 0) = (d > 1e-10) ? (sxy-sx*sy/n)/d : 0.0;

            // col 1: vol_level (annualised realized vol)
            double s = 0, sq = 0;
            for (int i = t-window; i < t; ++i) { s += returns[i]; sq += returns[i]*returns[i]; }
            double m = s/window, v = sq/window - m*m;
            features(t, 1) = std::sqrt(std::max(0.0, v)) * std::sqrt(252.0);

            // col 2: liquidity_proxy (volume ratio)
            // K-07/L-26: emit NaN when volumes are unavailable instead of
            // silently defaulting to 1.0 (which would imply "normal liquidity"
            // and silently mute STRESS_LIQUIDITY detection during data
            // outages or for sleeves without volume data). Downstream
            // map_garch checks isfinite() and skips the adjustment if NaN.
            if (!volumes.empty() && t < (int)volumes.size()) {
                double avg = 0;
                for (int i = t-window; i < t; ++i)
                    avg += (i < (int)volumes.size()) ? volumes[i] : 0;
                avg /= window;
                features(t, 2) = (avg > 1e-10)
                    ? volumes[t]/avg
                    : std::numeric_limits<double>::quiet_NaN();
            } else features(t, 2) = std::numeric_limits<double>::quiet_NaN();
        } else features(t, 2) = std::numeric_limits<double>::quiet_NaN();
        // col 3: correlation_stress — placeholder (set in cross-asset pass)
    }
    return features;
}

// ============================================================================
// Build GARCH conditional vol series: σ²_t = ω + α·ε²_{t-1} + β·σ²_{t-1}
// ============================================================================

static std::vector<double> build_garch_vol(
    const std::vector<double>& returns, GARCH& garch)
{
    const int T = (int)returns.size();
    double omega = garch.get_omega();
    double alpha = garch.get_alpha();
    double beta  = garch.get_beta();

    // Causal initialization: long-run unconditional variance ω/(1-α-β)
    // — depends only on fitted model parameters, not on any specific bar's
    // future returns. Fallback to r_0² when the recursion is explosive
    // (α+β >= 1) or the unconditional variance is not positive.
    double ab = alpha + beta;
    double sigma2;
    if (ab < 1.0 - 1e-6 && omega > 0.0) {
        sigma2 = omega / (1.0 - ab);
    } else {
        sigma2 = (T > 0) ? std::max(returns[0] * returns[0], 1e-10) : 1e-6;
    }

    std::vector<double> vol(T);
    for (int t = 0; t < T; ++t) {
        vol[t] = std::sqrt(std::max(sigma2, 1e-10));
        sigma2 = omega + alpha * returns[t]*returns[t] + beta * sigma2;
    }
    return vol;
}

// ============================================================================
// Build GMM 5D features: [r_t, σ̂_t, dd_speed, volume_ratio, corr_spike]
// K-03 fix: column 3 was labelled "vol_shock" but the code computed
// (vol[t]-avg)/avg which is a normalized volume RATIO, not a vol shock
// (which should be derivative of σ̂). The target fingerprints in
// market_regime_pipeline.cpp:491-495 were tuned for vol_shock semantics
// (positive = high vol surge in stress, low elsewhere) but the actual
// feature was volume_ratio (negative = volume below average = stress).
// We fix the semantics to match what the code actually computes:
// volume_ratio. Targets are retuned in market_regime_pipeline.cpp.
// ============================================================================

static Eigen::MatrixXd build_gmm_features(
    const std::vector<double>& returns,
    const std::vector<double>& volumes,
    const std::vector<double>& garch_vol)
{
    const int T = (int)returns.size();
    Eigen::MatrixXd gf = Eigen::MatrixXd::Zero(T, 5);
    double peak = 0, prev_dd = 0, cum = 0;

    for (int t = 0; t < T; ++t) {
        gf(t, 0) = returns[t];
        gf(t, 1) = garch_vol[t];
        cum += returns[t];
        peak = std::max(peak, cum);
        double dd = peak - cum;
        gf(t, 2) = dd - prev_dd;
        prev_dd = dd;
        // K-03: col 3 is volume_ratio = (volume_t - avg_20) / avg_20
        if (t >= 20 && !volumes.empty() && t < (int)volumes.size()) {
            double avg = 0;
            for (int i = t-20; i < t; ++i) avg += (i < (int)volumes.size()) ? volumes[i] : 0;
            avg /= 20.0;
            gf(t, 3) = (avg > 1e-10) ? (volumes[t]-avg)/avg : 0.0;
        }
        // col 4 = corr_spike — injected from pre-computed series below
    }
    return gf;
}

// (old build_hmm_filtered_probs removed — replaced by build_filtered_probs above)

// ============================================================================
// Compute MarketFeatures for timestep t (fix #4 — all fields populated)
// ============================================================================

static MarketFeatures compute_market_features(
    const std::vector<double>& returns,
    const std::vector<double>& garch_vol,
    const Eigen::MatrixXd& features,
    int t, int T)
{
    MarketFeatures mf;
    mf.realized_vol = features(t, 1);
    mf.liquidity_proxy = features(t, 2);
    mf.correlation_spike = features(t, 3);

    // Drawdown
    double cum = 0, peak = 0;
    for (int i = 0; i <= t; ++i) { cum += returns[i]; peak = std::max(peak, cum); }
    mf.drawdown = peak - cum;

    // Drawdown speed (change in drawdown from previous bar)
    if (t > 0) {
        double cum_prev = cum - returns[t];
        double peak_prev = 0;
        for (int i = 0; i < t; ++i) { double c = 0; for (int j = 0; j <= i; ++j) c += returns[j]; peak_prev = std::max(peak_prev, c); }
        double dd_prev = peak_prev - cum_prev;
        mf.drawdown_speed = mf.drawdown - dd_prev;
    }

    // Vol-of-vol: rolling std of GARCH σ_t over 60 bars
    if (t >= 60) {
        double s = 0, sq = 0;
        for (int i = t-60; i < t; ++i) { s += garch_vol[i]; sq += garch_vol[i]*garch_vol[i]; }
        double m = s / 60.0;
        mf.vol_of_vol = std::sqrt(std::max(0.0, sq/60.0 - m*m));
    }

    return mf;
}

// ============================================================================
// Compute rolling cross-asset correlation between symbols (fix #3)
// Returns a T-length vector of average pairwise correlation z-scores
// ============================================================================

static std::vector<double> compute_corr_spike(
    const Eigen::MatrixXd& composite_returns, int window = 60)
{
    const int T = (int)composite_returns.rows();
    const int N = (int)composite_returns.cols();
    std::vector<double> corr_spike(T, 0.0);

    if (N < 2) return corr_spike;

    // Compute rolling average pairwise correlation
    std::vector<double> rolling_corr(T, 0.0);
    for (int t = window; t < T; ++t) {
        double avg_corr = 0;
        int n_pairs = 0;
        for (int a = 0; a < N; ++a) {
            for (int b = a + 1; b < N; ++b) {
                double sa = 0, sb = 0, sa2 = 0, sb2 = 0, sab = 0;
                for (int i = t - window; i < t; ++i) {
                    double ra = composite_returns(i, a);
                    double rb = composite_returns(i, b);
                    sa += ra; sb += rb; sa2 += ra*ra; sb2 += rb*rb; sab += ra*rb;
                }
                double n = window;
                double denom = std::sqrt((sa2-sa*sa/n)*(sb2-sb*sb/n));
                double corr = (denom > 1e-10) ? (sab-sa*sb/n)/denom : 0.0;
                avg_corr += corr;
                ++n_pairs;
            }
        }
        rolling_corr[t] = (n_pairs > 0) ? avg_corr / n_pairs : 0.0;
    }

    // Compute z-score of rolling correlation vs 1-year history
    for (int t = 252; t < T; ++t) {
        double s = 0, sq = 0;
        for (int i = t - 252; i < t; ++i) { s += rolling_corr[i]; sq += rolling_corr[i]*rolling_corr[i]; }
        double m = s / 252.0;
        double sd = std::sqrt(std::max(1e-10, sq/252.0 - m*m));
        corr_spike[t] = (rolling_corr[t] - m) / sd;
    }

    return corr_spike;
}

// ============================================================================
// Build filtered probs via forward algorithm
//
// Plain HMM (A1): emission = N(r_t | μ_j, σ_j²)
// MSAR     (A2): emission = N(r_t | μ_j + Σ_L φ_{j,L} · r_{t-L-1}, σ_j²)
//
// Pass ar_coeffs = nullptr for plain HMM. Pass a K × lag matrix for MSAR.
// ============================================================================

static Eigen::MatrixXd build_filtered_probs(
    const std::vector<double>& returns,
    const Eigen::VectorXd& state_means,
    const Eigen::VectorXd& state_vars,
    const Eigen::MatrixXd& transition_matrix,
    int K,
    const Eigen::MatrixXd* ar_coeffs = nullptr)
{
    const int T = (int)returns.size();
    Eigen::MatrixXd filtered(T, K);
    Eigen::VectorXd alpha = Eigen::VectorXd::Constant(K, 1.0 / K);
    const int L = (ar_coeffs != nullptr) ? (int)ar_coeffs->cols() : 0;

    for (int t = 0; t < T; ++t) {
        Eigen::VectorXd pred = transition_matrix.transpose() * alpha;
        for (int j = 0; j < K; ++j) {
            // PDF A2: emission mean includes AR(lag) term for MSAR
            double mu_t = state_means(j);
            if (ar_coeffs) {
                for (int lag = 0; lag < L && t - lag - 1 >= 0; ++lag)
                    mu_t += (*ar_coeffs)(j, lag) * returns[t - lag - 1];
            }
            double sigma = std::sqrt(std::max(state_vars(j), 1e-10));
            double z = (returns[t] - mu_t) / sigma;
            double emission = std::exp(-0.5 * z * z) / (sigma * std::sqrt(2.0 * M_PI));
            pred(j) *= std::max(emission, 1e-300);
        }
        double s = pred.sum();
        if (s > 1e-300) { alpha = pred; alpha /= s; }
        else alpha = Eigen::VectorXd::Constant(K, 1.0 / K);
        filtered.row(t) = alpha.transpose();
    }
    return filtered;
}

// ============================================================================
// Process one sleeve
// ============================================================================

static void process_sleeve(
    SleeveId sleeve_id,
    const std::vector<double>& returns,
    const std::vector<double>& volumes,
    const std::vector<double>& corr_spike,  // fix #3: pre-computed correlation spike
    const std::vector<std::string>& dates,  // length = returns.size()+1; date[t+1] is when return t was realized
    MarketRegimePipeline& pipeline,
    std::ofstream* timeline_csv)
{
    const int T = (int)returns.size();
    if (T < 50) { std::cerr << "  Skipping: T=" << T << " too short\n"; return; }
    std::cout << "  T=" << T << "\n";

    Eigen::MatrixXd features = compute_feature_matrix(returns, volumes);

    // Fix #3: Inject cross-asset correlation spike into features column 3
    for (int t = 0; t < T && t < (int)corr_spike.size(); ++t)
        features(t, 3) = corr_spike[t];

    std::vector<double> rvec(returns.begin(), returns.end());

    // ── A1: Fit 3-state MarkovSwitching as HMM (fix #1) ────────────────
    // Using MarkovSwitching instead of standalone HMM because its Hamilton
    // filter is numerically stable on financial returns (no NaN emissions).
    // MarkovSwitching IS an HMM for scalar observations — same math.
    int K_hmm = pipeline.config().hmm_n_states;
    MarkovSwitchingConfig hmm_ms_config;
    hmm_ms_config.n_states = K_hmm;
    hmm_ms_config.max_iterations = pipeline.config().hmm_max_iterations;
    auto hmm_ms = std::make_shared<MarkovSwitching>(hmm_ms_config);
    auto hmm_fit = hmm_ms->fit(rvec);
    if (hmm_fit.is_error()) { std::cerr << "  HMM(MS3) fit failed\n"; return; }
    const auto& hmm_result = hmm_fit.value();

    // HMM emission signatures: (μ_i, σ_i) from learned parameters
    std::vector<Eigen::VectorXd> hmm_means(K_hmm);
    std::vector<Eigen::MatrixXd> hmm_covs(K_hmm);
    for (int j = 0; j < K_hmm; ++j) {
        hmm_means[j] = Eigen::VectorXd::Constant(1, hmm_result.state_means(j));
        hmm_covs[j] = Eigen::MatrixXd::Identity(1,1) * std::max(hmm_result.state_variances(j), 1e-10);
    }

    // Decoded states from smoothed probs
    std::vector<int> hmm_decoded(T);
    for (int t = 0; t < T; ++t) {
        int best = 0;
        hmm_result.smoothed_probabilities.row(t).maxCoeff(&best);
        hmm_decoded[t] = best;
    }

    for (int j = 0; j < K_hmm; ++j)
        std::cerr << "  HMM " << j << ": μ=" << hmm_result.state_means(j)
                  << " σ=" << std::sqrt(hmm_result.state_variances(j)) << "\n";

    // Fix #2: True filtered probs via forward pass with learned params
    Eigen::MatrixXd hmm_filtered = build_filtered_probs(
        returns, hmm_result.state_means, hmm_result.state_variances,
        hmm_result.transition_matrix, K_hmm);
    std::cerr << "  HMM forward pass complete (true filtered probs)\n";

    // ── A2: Fit 2-state MarkovSwitching + MarketMSAR for AR ────────────
    MarkovSwitchingConfig ms_config;
    ms_config.n_states = pipeline.config().msar_n_states;
    auto ms = std::make_shared<MarkovSwitching>(ms_config);
    auto ms_fit = ms->fit(rvec);
    if (ms_fit.is_error()) { std::cerr << "  MS fit failed\n"; return; }
    const auto& ms_result = ms_fit.value();

    // Use MarketMSAR for joint AR estimation (fix #2)
    int J_ms = (int)ms_result.state_means.size();
    Eigen::VectorXd msar_means(J_ms), msar_vars(J_ms);
    Eigen::MatrixXd msar_ar = Eigen::MatrixXd::Zero(J_ms, 1);

    Eigen::VectorXd ret_eigen(T);
    for (int t = 0; t < T; ++t) ret_eigen(t) = returns[t];

    MarketMSAR msar_model(pipeline.config().msar_ar_lag);
    auto msar_fit = msar_model.fit(ret_eigen,
        ms_result.smoothed_probabilities, ms_result.transition_matrix);

    if (msar_fit.is_ok()) {
        // Use MarketMSAR's jointly estimated parameters
        msar_means = msar_model.get_intercepts();
        msar_vars  = msar_model.get_residual_variances();
        msar_ar    = msar_model.get_ar_coeffs();  // K × lag
        std::cerr << "  MSAR: using MarketMSAR joint AR estimation\n";
    } else {
        // Fallback to MarkovSwitching params + post-hoc AR
        std::cerr << "  MSAR: MarketMSAR failed, using post-hoc AR\n";
        msar_means = ms_result.state_means;
        msar_vars  = ms_result.state_variances;
        for (int j = 0; j < J_ms; ++j) {
            double xy = 0, x2 = 0;
            for (int t = 1; t < T; ++t) {
                double w = ms_result.smoothed_probabilities(t, j);
                xy += w * returns[t] * returns[t-1];
                x2 += w * returns[t-1] * returns[t-1];
            }
            msar_ar(j, 0) = (x2 > 1e-12) ? xy/x2 : 0.0;
        }
    }

    for (int j = 0; j < J_ms; ++j)
        std::cerr << "  MSAR " << j << ": μ=" << msar_means(j) << " σ=" << std::sqrt(msar_vars(j))
                  << " φ=" << msar_ar(j, 0) << "\n";

    // True MSAR filtered probs: PDF A2 emission N(μ_j + φ_j · r_{t-1}, σ_j²)
    // Uses MarketMSAR intercepts/variances/AR-coeffs (not plain-MS state means).
    // K-06: use the transition matrix from MarketMSAR (consistent with the
    // smoothed posteriors used for AR fitting), NOT the pre-AR ms_result
    // matrix. Pre-fix: intercepts/vars/AR came from MarketMSAR but the
    // transition came from the raw-return MarkovSwitching, creating an
    // internal inconsistency in the forward pass dynamics.
    const Eigen::MatrixXd& msar_transition = msar_fit.is_ok()
        ? msar_model.get_transition_matrix()
        : ms_result.transition_matrix;  // fallback: post-hoc AR path uses MS matrix
    Eigen::MatrixXd msar_filtered = build_filtered_probs(
        returns, msar_means, msar_vars,
        msar_transition, J_ms,
        &msar_ar);
    std::cerr << "  MSAR forward pass complete (AR(1) emission, PDF-exact)\n";

    // ── A3: Fit GARCH + EGARCH — build actual σ_t series ─────────────
    auto garch = std::make_shared<GARCH>(GARCHConfig{});
    auto gfit = garch->fit(rvec);
    if (gfit.is_error()) { std::cerr << "  GARCH fit failed\n"; return; }
    std::vector<double> garch_vol = build_garch_vol(returns, *garch);
    std::cerr << "  GARCH σ_t: [" << *std::min_element(garch_vol.begin(), garch_vol.end())
              << ", " << *std::max_element(garch_vol.begin(), garch_vol.end()) << "]\n";

    // Fit EGARCH for asymmetry/leverage detection (PDF A3: "asymmetry flags")
    auto egarch = std::make_shared<EGARCH>(EGARCHConfig{});
    bool egarch_ok = false;
    double egarch_gamma = 0.0;
    auto egfit = egarch->fit(rvec);
    if (egfit.is_ok()) {
        egarch_ok = true;
        egarch_gamma = egarch->get_gamma();
        std::cerr << "  EGARCH γ=" << egarch_gamma
                  << (egarch_gamma < -0.01 ? " (leverage effect detected)" : " (no leverage)") << "\n";
    } else {
        std::cerr << "  EGARCH fit failed — asymmetry flags disabled\n";
    }

    // ── A4: Fit GMM on 5D feature space ────────────────────────────────
    Eigen::MatrixXd gmm_features = build_gmm_features(returns, volumes, garch_vol);
    // Fix: inject corr_spike into GMM feature column 4 (was always 0)
    for (int t = 0; t < T && t < (int)corr_spike.size(); ++t)
        gmm_features(t, 4) = corr_spike[t];

    GMM::Config gc; gc.max_iterations = pipeline.config().gmm_max_iterations;
    gc.restarts = pipeline.config().gmm_restarts;
    GMM gmm_model(gc);
    auto gmm_result = std::make_shared<GMMResult>(
        gmm_model.fit(gmm_features, pipeline.config().gmm_n_clusters));

    // ── Train pipeline ─────────────────────────────────────────────────
    // K-05: liquidity_proxy column (features col 2 — volume ratio, NaN
    // where unavailable per L-26) + HMM smoothed posteriors → optional
    // 3rd dim of HMM fingerprint.
    // K-05+: rolling 60-day cumulative log return per bar → optional 4th
    // dim. Captures slow-bear stress (moderate σ + persistent neg drift)
    // that the σ-based attractor alone misses.
    std::vector<double> liq_series(features.rows());
    for (int t = 0; t < features.rows(); ++t) liq_series[t] = features(t, 2);

    constexpr int kRet60Window = 60;
    std::vector<double> ret60_series(returns.size(), 0.0);
    for (int t = kRet60Window; t < (int)returns.size(); ++t) {
        double cum = 0.0;
        for (int i = t - kRet60Window; i < t; ++i) cum += returns[i];
        ret60_series[t] = cum;  // sum of log-returns over trailing 60 bars
    }

    auto tr = pipeline.train(sleeve_id, returns,
        hmm_means, hmm_covs,
        msar_means, msar_vars, msar_ar,
        garch_vol, *gmm_result, gmm_features,
        liq_series, hmm_result.smoothed_probabilities,
        ret60_series);
    if (tr.is_error()) { std::cerr << "  Train failed: " << tr.error()->what() << "\n"; return; }

    // ── Update loop ────────────────────────────────────────────────────
    // If timeline_csv is provided, walk EVERY bar from t=60 (warmup window)
    // to T-1 and emit a CSV line per bar for cross-version validation.
    // Otherwise, walk only the last 5 bars (legacy behavior).
    const int loop_start = (timeline_csv != nullptr) ? 60 : std::max(60, T - 5);

    std::cout << "\n  REGIME HISTORY (last 5 bars)\n"
              << "  ----------------------------------------\n";

    int start_t = loop_start;
    for (int t = start_t; t < T - 1; ++t) {
        // A1: True filtered HMM probs (fix #1)
        Eigen::VectorXd hmm_probs(K_hmm);
        for (int j = 0; j < K_hmm; ++j) hmm_probs(j) = hmm_filtered(t, j);

        // A2: MSAR true filtered probs (fix #2)
        Eigen::VectorXd msar_probs(J_ms);
        for (int j = 0; j < J_ms; ++j) msar_probs(j) = msar_filtered(t, j);

        // A3: GARCH features using actual σ_t
        // Causal vol percentile: rank current vol within trailing window [t-W+1, t].
        // No look-ahead — only data up to and including time t is used.
        GARCHFeatures gf;
        gf.conditional_vol = garch_vol[t];
        int W = pipeline.config().garch_vol_history;  // default 252
        int win_start = std::max(0, t - W + 1);
        int win_size  = t - win_start + 1;
        int cb = 0;
        for (int i = win_start; i <= t; ++i)
            if (garch_vol[i] <= garch_vol[t]) ++cb;
        gf.vol_percentile = (win_size > 0) ? (double)cb / win_size : 0.5;
        gf.vol_spike = (gf.vol_percentile > 0.90);
        if (t >= 60) {
            double vs = 0, vsq = 0;
            for (int i = t-60; i < t; ++i) { vs += garch_vol[i]; vsq += garch_vol[i]*garch_vol[i]; }
            double vm = vs/60.0;
            gf.vol_of_vol_high = (std::sqrt(std::max(0.0, vsq/60.0-vm*vm)) > 0.5*gf.conditional_vol);
        }
        // EGARCH asymmetry flag: leverage effect when γ < -0.01
        gf.asymmetry_flag = egarch_ok && (egarch_gamma < -0.01);

        // A0: All MarketFeatures populated (fix #4)
        MarketFeatures mf = compute_market_features(returns, garch_vol, features, t, T);

        // A4: GMM probs
        Eigen::VectorXd gi(5);
        for (int d = 0; d < 5; ++d) gi(d) = gmm_features(t, d);
        Eigen::VectorXd gmm_probs = GMM::predict_proba(gi, *gmm_result);

        auto br = pipeline.update(sleeve_id, hmm_probs, msar_probs, gf, mf, gmm_probs);
        if (br.is_error()) { std::cerr << "  Update failed t=" << t << "\n"; break; }

        // Emit CSV row if timeline mode is on
        if (timeline_csv != nullptr) {
            const auto& belief = br.value();
            const std::string& date = (t + 1 < (int)dates.size()) ? dates[t + 1] : "?";
            (*timeline_csv) << date << "," << sleeve_name(sleeve_id) << ","
                            << market_regime_name(belief.most_likely) << ","
                            << std::fixed << std::setprecision(4)
                            << belief.confidence;
            // Per-regime probs for richer analysis
            for (int r = 0; r < kNumMarketRegimes; ++r) {
                auto reg = static_cast<MarketRegimeL1>(r);
                double p = belief.market_probs.count(reg) ? belief.market_probs.at(reg) : 0.0;
                (*timeline_csv) << "," << p;
            }
            (*timeline_csv) << "\n";
        }

        // Console: only print the last 5 bars
        if (t >= T - 5) {
            std::cout << "  t=" << t << " ";
            print_belief(br.value());
        }
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    std::string conn_str = (argc > 1) ? argv[1] : "";
    std::string start_date = (argc > 2) ? argv[2] : "2020-01-01";
    std::string end_date   = (argc > 3) ? argv[3] : "2025-12-31";

    if (conn_str.empty()) {
        const char* env = std::getenv("DATABASE_URL");
        if (env) conn_str = env;
        else { std::cerr << "Usage: market_regime_pipeline_runner <conn> [start] [end]\n"; return 1; }
    }

    PostgresDatabase db(conn_str);
    if (db.connect().is_error()) { std::cerr << "DB connect failed\n"; return 1; }

    MarketDataLoader loader(db);
    MarketRegimePipeline pipeline;

    struct SleeveSpec { SleeveId id; std::string name; std::vector<std::string> symbols; AssetClass ac; };
    std::vector<SleeveSpec> sleeves = {
        {SleeveId::EQUITIES,    "equities",    {"MES.v.0","MNQ.v.0","MYM.v.0"},          AssetClass::FUTURES},
        {SleeveId::RATES,       "rates",       {"ZN.v.0","ZF.v.0"},                       AssetClass::FUTURES},
        {SleeveId::FX,          "fx",          {"6E.v.0","6J.v.0","6B.v.0","6A.v.0"},     AssetClass::FUTURES},
        {SleeveId::COMMODITIES, "commodities", {"CL.v.0","NG.v.0","GC.v.0","HG.v.0"},    AssetClass::FUTURES},
    };

    // Load all sleeve data + compute corr_spike per sleeve (fix #3)
    struct SleeveData {
        std::vector<double> returns;
        std::vector<double> volumes;
        std::vector<double> corr_spike;  // fix #3
        std::string primary;
        Eigen::MatrixXd composite_returns;  // K-08: kept for pooled cross-asset corr
        std::vector<std::string> dates;     // YYYY-MM-DD per bar (length = returns.size() + 1)
    };
    std::vector<SleeveData> sleeve_data(sleeves.size());

    for (size_t i = 0; i < sleeves.size(); ++i) {
        auto sr = loader.load_sleeve(sleeves[i].name, sleeves[i].symbols, sleeves[i].ac, start_date, end_date);
        if (sr.is_error() || sr.value().symbol_panels.empty()) {
            std::cerr << "Skipping " << sleeves[i].name << "\n";
            continue;
        }
        const auto& sd = sr.value();
        const auto& p = sd.symbol_panels[0];
        sleeve_data[i].returns = p.returns;
        sleeve_data[i].volumes = p.volumes;
        sleeve_data[i].primary = p.symbol;
        sleeve_data[i].dates   = p.dates;  // for timeline CSV diagnostic

        // K-08: pool composite_returns across all sleeves for global cross-
        // asset correlation. Per-sleeve correlation (e.g., MES/MNQ/MYM for
        // equities) sits at ~0.95 nearly always — useless as a stress signal.
        // The economically meaningful signal is when CROSS-asset correlations
        // (equity vs bond, stock vs commodity) flip during stress events.
        // We collect all composite_returns here and compute one global
        // corr_spike series in a second pass below (after this loop), then
        // assign the same series to every sleeve.
        sleeve_data[i].composite_returns = sd.composite_returns;
    }

    // K-08: build the pooled cross-asset returns matrix. Each sleeve
    // contributes its first symbol (or composite mean) as one column.
    // The rolling correlation z-score on this pooled matrix becomes the
    // shared corr_spike signal across sleeves.
    int max_T = 0;
    for (const auto& sd : sleeve_data)
        max_T = std::max(max_T, (int)sd.composite_returns.rows());
    int n_sleeve_cols = 0;
    for (const auto& sd : sleeve_data)
        if (sd.composite_returns.rows() > 0) ++n_sleeve_cols;

    std::vector<double> global_corr_spike(max_T,
        std::numeric_limits<double>::quiet_NaN());
    if (n_sleeve_cols >= 2 && max_T > 60) {
        Eigen::MatrixXd pooled = Eigen::MatrixXd::Constant(
            max_T, n_sleeve_cols, std::numeric_limits<double>::quiet_NaN());
        int col = 0;
        for (const auto& sd : sleeve_data) {
            if (sd.composite_returns.rows() == 0) continue;
            // Take the first column of each sleeve's composite_returns
            // as that sleeve's representative return series.
            for (int t = 0; t < sd.composite_returns.rows() && t < max_T; ++t)
                pooled(t, col) = sd.composite_returns(t, 0);
            ++col;
        }
        global_corr_spike = compute_corr_spike(pooled);
        std::cerr << "  K-08: pooled cross-asset corr_spike computed from "
                  << n_sleeve_cols << " sleeves\n";
    } else {
        std::cerr << "  K-08: insufficient sleeves (" << n_sleeve_cols
                  << ") for pooled correlation; corr_spike stays NaN\n";
    }

    // Assign the pooled global corr_spike to every sleeve, truncated/padded
    // to match each sleeve's individual T.
    for (size_t i = 0; i < sleeves.size(); ++i) {
        if (sleeve_data[i].returns.empty()) continue;
        const int T_sleeve = (int)sleeve_data[i].returns.size();
        sleeve_data[i].corr_spike.assign(T_sleeve,
            std::numeric_limits<double>::quiet_NaN());
        for (int t = 0; t < T_sleeve && t < (int)global_corr_spike.size(); ++t)
            sleeve_data[i].corr_spike[t] = global_corr_spike[t];
    }

    // Process each sleeve
    // Optional regime-timeline CSV output for cross-version validation.
    // Set TIMELINE_CSV=path to enable; otherwise the existing last-5-bars
    // console output is unaffected.
    std::ofstream timeline_csv;
    std::ofstream* timeline_ptr = nullptr;
    if (const char* env = std::getenv("TIMELINE_CSV")) {
        timeline_csv.open(env);
        if (timeline_csv.is_open()) {
            timeline_csv << "date,sleeve,regime,confidence,"
                            "p_TREND_LOWVOL,p_TREND_HIGHVOL,p_MEANREV_CHOPPY,"
                            "p_STRESS_PRICE,p_STRESS_LIQUIDITY\n";
            timeline_ptr = &timeline_csv;
            std::cerr << "[TIMELINE] writing per-bar regime CSV → " << env << "\n";
        } else {
            std::cerr << "[TIMELINE] WARN: cannot open " << env
                      << " for writing — skipping CSV output\n";
        }
    }

    for (size_t i = 0; i < sleeves.size(); ++i) {
        if (sleeve_data[i].returns.empty()) continue;
        std::cout << "\n========================================================\n"
                  << " SLEEVE: " << sleeves[i].name << "\n"
                  << "========================================================\n"
                  << "  Primary: " << sleeve_data[i].primary;
        process_sleeve(sleeves[i].id, sleeve_data[i].returns, sleeve_data[i].volumes,
                       sleeve_data[i].corr_spike, sleeve_data[i].dates,
                       pipeline, timeline_ptr);
    }

    if (timeline_csv.is_open()) timeline_csv.close();

    std::cout << "\n========================================================\n"
              << " MARKET REGIME PIPELINE COMPLETE\n"
              << "========================================================\n";

    return 0;
}
