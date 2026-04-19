#include "trade_ngin/statistics/state_estimation/market_regime_pipeline.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>

namespace trade_ngin {
namespace statistics {

static constexpr double kPi  = 3.14159265358979323846;
static constexpr double kEps = 1e-8;

static const char* kRegimeNames[] = {
    "TREND_LOWVOL", "TREND_HIGHVOL", "MEANREV_CHOPPY",
    "STRESS_PRICE", "STRESS_LIQUIDITY"
};

// ============================================================================
// Name helpers
// ============================================================================

const char* market_regime_name(MarketRegimeL1 r) {
    int idx = static_cast<int>(r);
    if (idx >= 0 && idx < kNumMarketRegimes) return kRegimeNames[idx];
    return "UNKNOWN";
}

const char* sleeve_name(SleeveId s) {
    switch (s) {
        case SleeveId::EQUITIES:    return "equities";
        case SleeveId::RATES:       return "rates";
        case SleeveId::FX:          return "fx";
        case SleeveId::COMMODITIES: return "commodities";
    }
    return "unknown";
}

// No static target fingerprints — they are model-specific (see train methods).

// ============================================================================
// Default sleeve configs (from synthesized spec thresholds)
// ============================================================================

SleeveConfig default_equities_config() {
    SleeveConfig c;
    c.sleeve_id = SleeveId::EQUITIES;
    c.thresholds = {0.12, 0.35, 3.0, 0.40};
    c.w_hmm = 0.40; c.w_msar = 0.30; c.w_garch = 0.20; c.w_gmm = 0.10;
    return c;
}

SleeveConfig default_rates_config() {
    SleeveConfig c;
    c.sleeve_id = SleeveId::RATES;
    c.thresholds = {0.08, 0.20, 3.0, 0.50};
    // Rates: slightly higher GARCH sensitivity per PDF §9
    c.w_hmm = 0.38; c.w_msar = 0.30; c.w_garch = 0.22; c.w_gmm = 0.10;
    return c;
}

SleeveConfig default_fx_config() {
    SleeveConfig c;
    c.sleeve_id = SleeveId::FX;
    c.thresholds = {0.06, 0.15, 4.0, 0.40};
    c.w_hmm = 0.40; c.w_msar = 0.30; c.w_garch = 0.20; c.w_gmm = 0.10;
    return c;
}

SleeveConfig default_commodities_config() {
    SleeveConfig c;
    c.sleeve_id = SleeveId::COMMODITIES;
    c.thresholds = {0.18, 0.40, 3.0, 0.30};
    c.w_hmm = 0.40; c.w_msar = 0.30; c.w_garch = 0.20; c.w_gmm = 0.10;
    return c;
}

// ============================================================================
// Config constructor + serialisation
// ============================================================================

MarketRegimePipelineConfig::MarketRegimePipelineConfig() {
    sleeve_configs[0] = default_equities_config();
    sleeve_configs[1] = default_rates_config();
    sleeve_configs[2] = default_fx_config();
    sleeve_configs[3] = default_commodities_config();
}

nlohmann::json MarketRegimePipelineConfig::to_json() const {
    nlohmann::json j;
    j["hmm_n_states"]       = hmm_n_states;
    j["hmm_max_iterations"] = hmm_max_iterations;
    j["msar_n_states"]      = msar_n_states;
    j["msar_ar_lag"]        = msar_ar_lag;
    j["gmm_n_clusters"]     = gmm_n_clusters;
    j["fingerprint_tau"]    = fingerprint_tau;
    j["lambda"]             = lambda;
    return j;
}

void MarketRegimePipelineConfig::from_json(const nlohmann::json& j) {
    auto get = [&](const char* key, auto& val) {
        if (j.contains(key)) j.at(key).get_to(val);
    };
    get("hmm_n_states",       hmm_n_states);
    get("hmm_max_iterations", hmm_max_iterations);
    get("msar_n_states",      msar_n_states);
    get("msar_ar_lag",        msar_ar_lag);
    get("gmm_n_clusters",     gmm_n_clusters);
    get("fingerprint_tau",    fingerprint_tau);
    get("lambda",             lambda);
}

// ============================================================================
// Constructor
// ============================================================================

MarketRegimePipeline::MarketRegimePipeline(MarketRegimePipelineConfig config)
    : config_(std::move(config))
{
    for (int s = 0; s < kNumSleeves; ++s) {
        auto& state = sleeve_states_[s];
        state.prev_smoothed.setConstant(1.0 / kNumMarketRegimes);
        state.trained = false;
    }
}

// ============================================================================
// Utility: percentile
// ============================================================================

double MarketRegimePipeline::percentile(const std::vector<double>& data, double p) {
    std::vector<double> sorted = data;
    std::sort(sorted.begin(), sorted.end());
    double idx = p * (sorted.size() - 1);
    int lo = static_cast<int>(std::floor(idx));
    int hi = static_cast<int>(std::ceil(idx));
    if (lo == hi) return sorted[lo];
    double frac = idx - lo;
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

// ============================================================================
// Compute mean feature vector for a set of timestep indices (for GMM)
// ============================================================================

Eigen::VectorXd MarketRegimePipeline::compute_feature_fingerprint(
    const Eigen::MatrixXd& features,
    const std::vector<int>& indices)
{
    const int D = (int)features.cols();
    Eigen::VectorXd fp = Eigen::VectorXd::Zero(D);
    if (indices.empty()) return fp;

    for (int t : indices)
        fp += features.row(t).transpose();
    fp /= static_cast<double>(indices.size());
    return fp;
}

// ============================================================================
// Standardise native fingerprints (z-score) + build softmax mapping matrix
// ============================================================================

void MarketRegimePipeline::standardise_and_build_mapping(
    MarketFingerprintMapping& mapping, double tau) const
{
    const int J = (int)mapping.native_fingerprints.size();
    if (J == 0) return;
    const int D = (int)mapping.native_fingerprints[0].size();

    // Compute mean/std across native fingerprints
    mapping.fp_mean = Eigen::VectorXd::Zero(D);
    for (int j = 0; j < J; ++j)
        mapping.fp_mean += mapping.native_fingerprints[j];
    mapping.fp_mean /= J;

    mapping.fp_std = Eigen::VectorXd::Zero(D);
    for (int j = 0; j < J; ++j) {
        for (int d = 0; d < D; ++d) {
            double diff = mapping.native_fingerprints[j](d) - mapping.fp_mean(d);
            mapping.fp_std(d) += diff * diff;
        }
    }
    // Floor std at 1e-4 to prevent degenerate standardisation when
    // all native states have near-identical signatures
    for (int d = 0; d < D; ++d) {
        double raw_std = std::sqrt(mapping.fp_std(d) / J + kEps);
        mapping.fp_std(d) = std::max(raw_std, 1e-4);
    }

    // Standardise native fingerprints
    for (int j = 0; j < J; ++j) {
        for (int d = 0; d < D; ++d) {
            mapping.native_fingerprints[j](d) =
                (mapping.native_fingerprints[j](d) - mapping.fp_mean(d)) / mapping.fp_std(d);
        }
    }

    // Build softmax distance mapping matrix (J × 5)
    mapping.tau = tau;
    mapping.mapping_matrix = Eigen::MatrixXd::Zero(J, kNumMarketRegimes);

    for (int j = 0; j < J; ++j) {
        Eigen::VectorXd log_weights(kNumMarketRegimes);
        for (int k = 0; k < kNumMarketRegimes; ++k) {
            // Compute L2 distance element-by-element to avoid dimension assertions
            double dist_sq = 0;
            for (int d = 0; d < D; ++d) {
                double diff = mapping.native_fingerprints[j](d) - mapping.target_fingerprints[k](d);
                dist_sq += diff * diff;
            }
            log_weights(k) = -std::sqrt(dist_sq) / tau;
        }
        double max_lw = log_weights.maxCoeff();
        Eigen::VectorXd w = (log_weights.array() - max_lw).exp();
        double w_sum = w.sum();
        if (!std::isfinite(w_sum) || w_sum < kEps)
            w.setConstant(1.0 / kNumMarketRegimes);
        else
            w /= w_sum;
        mapping.mapping_matrix.row(j) = w.transpose();
    }
}

// ============================================================================
// A1: HMM Fingerprint Training — uses model state signatures (μ_i, σ_i)
// Per PDF: mapping consumes "state signatures (mean/vol/autocorr/etc.)"
// ============================================================================

void MarketRegimePipeline::train_hmm_fingerprints(
    SleeveId sleeve,
    const std::vector<Eigen::VectorXd>& hmm_means,
    const std::vector<Eigen::MatrixXd>& hmm_covs)
{
    int s = static_cast<int>(sleeve);
    auto& mapping = sleeve_states_[s].hmm_mapping;
    const int J = (int)hmm_means.size();

    // Native fingerprints: 2D [μ_i, σ_i] from HMM emission parameters
    // μ_i = emission mean (scalar for univariate, first element for multivariate)
    // σ_i = emission std dev (sqrt of variance/first diagonal element)
    mapping.native_fingerprints.resize(J);
    for (int j = 0; j < J; ++j) {
        double mu = hmm_means[j](0);  // first (or only) dimension
        double sigma = std::sqrt(hmm_covs[j](0, 0));
        mapping.native_fingerprints[j] = Eigen::VectorXd(2);
        mapping.native_fingerprints[j] << mu, sigma;
        std::cerr << "[A1-" << sleeve_name(sleeve) << "] HMM state " << j
                  << " μ=" << std::fixed << std::setprecision(5) << mu
                  << " σ=" << sigma << "\n";
    }

    // Target fingerprints: 2D [μ, σ] for each ontology state (z-scored)
    // After standardisation, these represent the expected signature shape
    mapping.target_fingerprints.resize(kNumMarketRegimes);
    Eigen::VectorXd t(2);
    t <<  1.5, -1.5; mapping.target_fingerprints[0] = t;  // TREND_LOWVOL:  positive drift, low vol
    t <<  0.5,  1.0; mapping.target_fingerprints[1] = t;  // TREND_HIGHVOL: some drift, high vol
    t <<  0.0,  0.0; mapping.target_fingerprints[2] = t;  // MEANREV_CHOPPY: no drift, mid vol
    t << -1.0,  1.5; mapping.target_fingerprints[3] = t;  // STRESS_PRICE:  negative drift, high vol
    t << -0.5,  2.0; mapping.target_fingerprints[4] = t;  // STRESS_LIQUIDITY: negative, extreme vol

    // Standardise + build mapping
    standardise_and_build_mapping(mapping, config_.fingerprint_tau);

    std::cerr << "[A1-" << sleeve_name(sleeve) << "] HMM mapping matrix ("
              << J << "x" << kNumMarketRegimes << "):\n";
    for (int j = 0; j < J; ++j) {
        std::cerr << "  state " << j << ": [";
        for (int k = 0; k < kNumMarketRegimes; ++k) {
            if (k > 0) std::cerr << ", ";
            std::cerr << std::fixed << std::setprecision(3) << mapping.mapping_matrix(j, k);
        }
        std::cerr << "]\n";
    }
}

// ============================================================================
// A1: HMM Mapping (runtime)
// ============================================================================

Eigen::Matrix<double, kNumMarketRegimes, 1>
MarketRegimePipeline::map_hmm(SleeveId sleeve, const Eigen::VectorXd& p_native) const {
    int s = static_cast<int>(sleeve);
    // mapping_matrix is J×5 dynamic, p_native is J dynamic → product is 5 dynamic
    Eigen::VectorXd dyn = sleeve_states_[s].hmm_mapping.mapping_matrix.transpose() * p_native;
    Eigen::Matrix<double, kNumMarketRegimes, 1> result;
    for (int i = 0; i < kNumMarketRegimes; ++i)
        result(i) = (i < dyn.size()) ? std::max(0.0, dyn(i)) : 0.0;

    double sum = result.sum();
    if (sum > kEps) result /= sum;
    else result.setConstant(1.0 / kNumMarketRegimes);
    return result;
}

// ============================================================================
// A2: MSAR Fingerprint Training — uses model state signatures (μ_i, σ_i, φ_i)
// Per PDF: "signatures (μ_i, σ_i, φ_i)" and "φ > 0 → trend, φ < 0 → mean-rev"
// ============================================================================

void MarketRegimePipeline::train_msar_fingerprints(
    SleeveId sleeve,
    const Eigen::VectorXd& state_means,
    const Eigen::VectorXd& state_vars,
    const Eigen::MatrixXd& ar_coeffs)
{
    int s = static_cast<int>(sleeve);
    auto& mapping = sleeve_states_[s].msar_mapping;
    const int J = (int)state_means.size();

    // Native fingerprints: 3D [μ_i, σ_i, φ_i] from MSAR learned parameters
    mapping.native_fingerprints.resize(J);
    for (int j = 0; j < J; ++j) {
        double mu = state_means(j);
        double sigma = std::sqrt(std::max(kEps, state_vars(j)));
        double phi = (ar_coeffs.cols() > 0) ? ar_coeffs(j, 0) : 0.0;  // AR(1) coefficient
        mapping.native_fingerprints[j] = Eigen::VectorXd(3);
        mapping.native_fingerprints[j] << mu, sigma, phi;
        std::cerr << "[A2-" << sleeve_name(sleeve) << "] MSAR state " << j
                  << " μ=" << std::fixed << std::setprecision(5) << mu
                  << " σ=" << sigma << " φ=" << phi << "\n";
    }

    // Target fingerprints: 3D [μ, σ, φ] for each ontology state (z-scored)
    // φ > 0 = trending (persistence), φ < 0 = mean-reverting
    mapping.target_fingerprints.resize(kNumMarketRegimes);
    Eigen::VectorXd t(3);
    t <<  1.5, -1.5,  1.5; mapping.target_fingerprints[0] = t;  // TREND_LOWVOL:  pos drift, low vol, trending
    t <<  0.5,  1.0,  1.0; mapping.target_fingerprints[1] = t;  // TREND_HIGHVOL: some drift, high vol, trending
    t <<  0.0,  0.0, -1.5; mapping.target_fingerprints[2] = t;  // MEANREV_CHOPPY: no drift, mid vol, mean-reverting
    t << -1.0,  1.5, -0.5; mapping.target_fingerprints[3] = t;  // STRESS_PRICE:  neg drift, high vol, some mean-rev
    t << -0.5,  2.0, -1.0; mapping.target_fingerprints[4] = t;  // STRESS_LIQUIDITY: neg, extreme vol, mean-rev

    // Standardise + build mapping
    standardise_and_build_mapping(mapping, config_.fingerprint_tau);

    std::cerr << "[A2-" << sleeve_name(sleeve) << "] MSAR mapping matrix ("
              << J << "x" << kNumMarketRegimes << ")\n";
}

// ============================================================================
// A2: MSAR Mapping (runtime)
// ============================================================================

Eigen::Matrix<double, kNumMarketRegimes, 1>
MarketRegimePipeline::map_msar(SleeveId sleeve, const Eigen::VectorXd& p_native) const {
    int s = static_cast<int>(sleeve);
    Eigen::VectorXd dyn = sleeve_states_[s].msar_mapping.mapping_matrix.transpose() * p_native;
    Eigen::Matrix<double, kNumMarketRegimes, 1> result;
    for (int i = 0; i < kNumMarketRegimes; ++i)
        result(i) = (i < dyn.size()) ? std::max(0.0, dyn(i)) : 0.0;

    double sum = result.sum();
    if (sum > kEps) result /= sum;
    else result.setConstant(1.0 / kNumMarketRegimes);
    return result;
}

// ============================================================================
// A3: GARCH Mapping Training (rule-based, like macro quadrant model)
// ============================================================================

void MarketRegimePipeline::train_garch_mapping(
    SleeveId sleeve,
    const Eigen::MatrixXd& /*features*/,
    const std::vector<double>& /*garch_vol_series*/)
{
    int s = static_cast<int>(sleeve);
    auto& params = sleeve_states_[s].garch_mapping;

    // Rule-based 4-bin vol × 5-regime mapping (bins determined at runtime
    // from the causal, trailing-window vol percentile).
    auto& t = params.mapping_table;
    t = Eigen::MatrixXd::Zero(4, kNumMarketRegimes);

    //                        TL_LV  TL_HV  MR_CH  ST_PR  ST_LQ
    t.row(0) << /* low vol */  0.55,  0.05,  0.30,  0.05,  0.05;
    t.row(1) << /* mid vol */  0.15,  0.20,  0.45,  0.10,  0.10;
    t.row(2) << /* high vol */ 0.05,  0.40,  0.15,  0.30,  0.10;
    t.row(3) << /* extreme */  0.02,  0.10,  0.03,  0.45,  0.40;

    params.trained = true;

    std::cerr << "[A3-" << sleeve_name(sleeve) << "] GARCH mapping trained\n";
}

// ============================================================================
// A3: GARCH Mapping (runtime)
// ============================================================================

Eigen::Matrix<double, kNumMarketRegimes, 1>
MarketRegimePipeline::map_garch(
    SleeveId sleeve,
    const GARCHFeatures& garch,
    const MarketFeatures& market) const
{
    int s = static_cast<int>(sleeve);
    const auto& params = sleeve_states_[s].garch_mapping;
    const auto& sc = config_.sleeve_configs[s];

    // Determine vol bin (0=low, 1=mid, 2=high, 3=extreme)
    int vol_bin;
    if (garch.vol_percentile < sc.garch_vol_low_pctile)       vol_bin = 0;
    else if (garch.vol_percentile < sc.garch_vol_high_pctile) vol_bin = 1;
    else if (garch.vol_percentile < sc.garch_vol_extreme_pctile) vol_bin = 2;
    else                                                       vol_bin = 3;

    // Start from base mapping for this vol bin
    Eigen::Matrix<double, kNumMarketRegimes, 1> result = params.mapping_table.row(vol_bin).transpose();

    // Adjust for vol spike: boost STRESS, reduce TREND
    if (garch.vol_spike) {
        result(static_cast<int>(MarketRegimeL1::STRESS_PRICE)) += 0.10;
        result(static_cast<int>(MarketRegimeL1::TREND_LOWVOL)) -= 0.05;
        result(static_cast<int>(MarketRegimeL1::MEANREV_CHOPPY)) -= 0.05;
    }

    // Adjust for vol-of-vol: boost STRESS_LIQUIDITY
    if (garch.vol_of_vol_high) {
        result(static_cast<int>(MarketRegimeL1::STRESS_LIQUIDITY)) += 0.10;
        result(static_cast<int>(MarketRegimeL1::TREND_LOWVOL)) -= 0.05;
        result(static_cast<int>(MarketRegimeL1::TREND_HIGHVOL)) -= 0.05;
    }

    // Adjust for EGARCH asymmetry: leverage effect amplifies stress during drawdowns
    if (garch.asymmetry_flag && garch.vol_percentile > 0.5) {
        result(static_cast<int>(MarketRegimeL1::STRESS_PRICE)) += 0.08;
        result(static_cast<int>(MarketRegimeL1::TREND_LOWVOL)) -= 0.05;
        result(static_cast<int>(MarketRegimeL1::MEANREV_CHOPPY)) -= 0.03;
    }

    // Adjust for liquidity collapse: shift from STRESS_PRICE to STRESS_LIQUIDITY
    if (market.liquidity_proxy < 0.3) {
        double shift = 0.10 * (1.0 - market.liquidity_proxy / 0.3);
        result(static_cast<int>(MarketRegimeL1::STRESS_LIQUIDITY)) += shift;
        result(static_cast<int>(MarketRegimeL1::STRESS_PRICE)) -= shift * 0.5;
        result(static_cast<int>(MarketRegimeL1::MEANREV_CHOPPY)) -= shift * 0.5;
    }

    // Clamp and normalise
    for (int r = 0; r < kNumMarketRegimes; ++r)
        result(r) = std::max(0.0, result(r));
    double sum = result.sum();
    if (sum > kEps) result /= sum;
    else result.setConstant(1.0 / kNumMarketRegimes);

    return result;
}

// ============================================================================
// A4: GMM Fingerprint Training
// Per PDF: GMM consumes x_t = [r_t, σ̂_t, drawdown_speed, vol_shock, corr_spike]
// Native fingerprints = mean feature vector per cluster (empirical averages)
// ============================================================================

void MarketRegimePipeline::train_gmm_fingerprints(
    SleeveId sleeve,
    const GMMResult& gmm_result,
    const Eigen::MatrixXd& gmm_features)
{
    int s = static_cast<int>(sleeve);
    auto& mapping = sleeve_states_[s].gmm_mapping;
    const int T = (int)gmm_result.labels.size();
    const int K = gmm_result.k;
    const int D = (int)gmm_features.cols();  // 5D per PDF

    // Store trained GMM model for runtime predict_proba
    sleeve_states_[s].gmm_model = gmm_result;

    // Compute native fingerprints: mean of GMM feature vectors per cluster
    mapping.native_fingerprints.resize(K);
    for (int j = 0; j < K; ++j) {
        std::vector<int> indices;
        for (int t = 0; t < T; ++t)
            if (gmm_result.labels(t) == j)
                indices.push_back(t);
        mapping.native_fingerprints[j] = compute_feature_fingerprint(gmm_features, indices);
        std::cerr << "[A4-" << sleeve_name(sleeve) << "] GMM cluster " << j
                  << " n=" << indices.size() << "\n";
    }

    // Target fingerprints: 5D [r, σ̂, dd_speed, vol_shock, corr_spike] per ontology state
    // z-scored: positive = high/elevated, negative = low/calm
    mapping.target_fingerprints.resize(kNumMarketRegimes);
    Eigen::VectorXd t(D);
    //                     r     σ̂     dd_spd  vol_shk  corr_spk
    t <<  0.5, -1.5,  0.0, -1.0, -0.5; mapping.target_fingerprints[0] = t;  // TREND_LOWVOL
    t <<  0.3,  1.0,  0.0,  0.0,  0.5; mapping.target_fingerprints[1] = t;  // TREND_HIGHVOL
    t <<  0.0,  0.0,  0.0, -0.5,  0.0; mapping.target_fingerprints[2] = t;  // MEANREV_CHOPPY
    t << -1.0,  1.5,  1.5,  1.0,  1.5; mapping.target_fingerprints[3] = t;  // STRESS_PRICE
    t << -0.5,  2.0,  1.0,  2.0,  1.5; mapping.target_fingerprints[4] = t;  // STRESS_LIQUIDITY

    // Standardise + build mapping
    standardise_and_build_mapping(mapping, config_.fingerprint_tau);

    std::cerr << "[A4-" << sleeve_name(sleeve) << "] GMM mapping matrix ("
              << K << "x" << kNumMarketRegimes << ")\n";
}

// ============================================================================
// A4: GMM Mapping (runtime)
// ============================================================================

Eigen::Matrix<double, kNumMarketRegimes, 1>
MarketRegimePipeline::map_gmm(SleeveId sleeve, const Eigen::VectorXd& p_cluster) const {
    int s = static_cast<int>(sleeve);
    Eigen::VectorXd dyn = sleeve_states_[s].gmm_mapping.mapping_matrix.transpose() * p_cluster;
    Eigen::Matrix<double, kNumMarketRegimes, 1> result;
    for (int i = 0; i < kNumMarketRegimes; ++i)
        result(i) = (i < dyn.size()) ? std::max(0.0, dyn(i)) : 0.0;

    double sum = result.sum();
    if (sum > kEps) result /= sum;
    else result.setConstant(1.0 / kNumMarketRegimes);
    return result;
}

// ============================================================================
// Aggregation
// ============================================================================

Eigen::Matrix<double, kNumMarketRegimes, 1>
MarketRegimePipeline::aggregate(
    SleeveId sleeve,
    const Eigen::Matrix<double, kNumMarketRegimes, 1>& p_hmm,
    const Eigen::Matrix<double, kNumMarketRegimes, 1>& p_msar,
    const Eigen::Matrix<double, kNumMarketRegimes, 1>& p_garch,
    const Eigen::Matrix<double, kNumMarketRegimes, 1>& p_gmm) const
{
    int s = static_cast<int>(sleeve);
    const auto& sc = config_.sleeve_configs[s];

    Eigen::Matrix<double, kNumMarketRegimes, 1> raw =
        sc.w_hmm   * p_hmm +
        sc.w_msar  * p_msar +
        sc.w_garch * p_garch +
        sc.w_gmm   * p_gmm;

    for (int r = 0; r < kNumMarketRegimes; ++r)
        raw(r) = std::max(0.0, raw(r));
    double sum = raw.sum();
    if (sum > kEps) raw /= sum;
    else raw.setConstant(1.0 / kNumMarketRegimes);

    return raw;
}

// ============================================================================
// A7 EWMA smoothing — single constant λ per PDF:
//   p_final = λ · p_raw + (1 - λ) · p_final_{t-1}
// ============================================================================

Eigen::Matrix<double, kNumMarketRegimes, 1>
MarketRegimePipeline::smooth(
    SleeveId sleeve,
    const Eigen::Matrix<double, kNumMarketRegimes, 1>& p_raw)
{
    int s = static_cast<int>(sleeve);
    auto& state = sleeve_states_[s];

    double lam = config_.lambda;
    Eigen::Matrix<double, kNumMarketRegimes, 1> result;
    for (int r = 0; r < kNumMarketRegimes; ++r)
        result(r) = lam * p_raw(r) + (1.0 - lam) * state.prev_smoothed(r);

    double rsum = result.sum();
    if (rsum > kEps) result /= rsum;
    state.prev_smoothed = result;

    return result;
}

// ============================================================================
// Confidence (top1 - top2 margin) + Stability (entropy-based concentration,
// matches PDF A8's entropy formula H = -Σ π log π)
// ============================================================================

double MarketRegimePipeline::compute_confidence(
    const Eigen::Matrix<double, kNumMarketRegimes, 1>& probs) const
{
    double top1 = -1, top2 = -1;
    for (int r = 0; r < kNumMarketRegimes; ++r) {
        if (probs(r) > top1) { top2 = top1; top1 = probs(r); }
        else if (probs(r) > top2) { top2 = probs(r); }
    }
    return top1 - std::max(0.0, top2);
}

double MarketRegimePipeline::compute_stability(
    const Eigen::Matrix<double, kNumMarketRegimes, 1>& probs) const
{
    // stability = 1 - H(p) / log(K). 1 = fully concentrated, 0 = uniform.
    double H = 0.0;
    for (int r = 0; r < kNumMarketRegimes; ++r) {
        double p = probs(r);
        if (p > kEps) H -= p * std::log(p);
    }
    double H_max = std::log(static_cast<double>(kNumMarketRegimes));
    if (H_max < kEps) return 1.0;
    return std::max(0.0, std::min(1.0, 1.0 - H / H_max));
}

// ============================================================================
// Train (per sleeve)
// ============================================================================

Result<void> MarketRegimePipeline::train(
    SleeveId sleeve,
    const std::vector<double>& returns,
    const std::vector<Eigen::VectorXd>& hmm_means,
    const std::vector<Eigen::MatrixXd>& hmm_covs,
    const Eigen::VectorXd& msar_state_means,
    const Eigen::VectorXd& msar_state_vars,
    const Eigen::MatrixXd& msar_ar_coeffs,
    const std::vector<double>& garch_vol_series,
    const GMMResult& gmm_result,
    const Eigen::MatrixXd& gmm_feature_matrix)
{
    int s = static_cast<int>(sleeve);
    const int T = (int)returns.size();

    if (T < 20) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
            "Need at least 20 timesteps for training", "MarketRegimePipeline");
    }

    std::cerr << "[MarketRegimePipeline] Training sleeve '"
              << sleeve_name(sleeve) << "' T=" << T << "\n";

    // A1: HMM fingerprint mapping — uses emission parameters (μ_i, σ_i)
    train_hmm_fingerprints(sleeve, hmm_means, hmm_covs);

    // A2: MSAR fingerprint mapping — uses regime parameters (μ_i, σ_i, φ_i)
    train_msar_fingerprints(sleeve, msar_state_means, msar_state_vars, msar_ar_coeffs);

    // A3: GARCH feature mapping — rule-based on vol percentiles/flags
    // Build a dummy feature matrix for GARCH mapping (just needs vol column)
    Eigen::MatrixXd garch_feat = Eigen::MatrixXd::Zero(T, 4);
    for (int t = 0; t < T; ++t)
        garch_feat(t, 1) = (t < (int)garch_vol_series.size()) ? garch_vol_series[t] : 0.0;
    train_garch_mapping(sleeve, garch_feat, garch_vol_series);

    // A4: GMM fingerprint mapping — on [r_t, σ̂_t, dd_speed, vol_shock, corr_spike]
    train_gmm_fingerprints(sleeve, gmm_result, gmm_feature_matrix);

    // Reset runtime state (only EWMA recurrence state per PDF A7)
    sleeve_states_[s].prev_smoothed.setConstant(1.0 / kNumMarketRegimes);
    sleeve_states_[s].trained = true;

    std::cerr << "[MarketRegimePipeline] Training complete for '"
              << sleeve_name(sleeve) << "'\n";
    return Result<void>();
}

// ============================================================================
// Update (runtime orchestration, per sleeve)
// ============================================================================

Result<MarketBelief> MarketRegimePipeline::update(
    SleeveId sleeve,
    const Eigen::VectorXd& hmm_state_probs,
    const Eigen::VectorXd& msar_state_probs,
    const GARCHFeatures& garch_features,
    const MarketFeatures& market_features,
    const Eigen::VectorXd& gmm_cluster_probs)
{
    int s = static_cast<int>(sleeve);
    if (!sleeve_states_[s].trained) {
        return make_error<MarketBelief>(ErrorCode::NOT_INITIALIZED,
            "Sleeve not trained: " + std::string(sleeve_name(sleeve)),
            "MarketRegimePipeline");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Map each model → 5 probs
    auto p_hmm   = map_hmm(sleeve, hmm_state_probs);
    auto p_msar  = map_msar(sleeve, msar_state_probs);
    auto p_garch = map_garch(sleeve, garch_features, market_features);
    auto p_gmm   = map_gmm(sleeve, gmm_cluster_probs);

    // ── Failure degradation ────────────────────────────────────────────────
    auto is_valid = [](const Eigen::Matrix<double, kNumMarketRegimes, 1>& p) -> bool {
        for (int i = 0; i < kNumMarketRegimes; ++i)
            if (!std::isfinite(p(i))) return false;
        return p.sum() > 1e-10;
    };

    const auto& sc = config_.sleeve_configs[s];
    bool hmm_ok   = is_valid(p_hmm);
    bool msar_ok  = is_valid(p_msar);
    bool garch_ok = is_valid(p_garch);
    bool gmm_ok   = is_valid(p_gmm);

    double w_hmm_eff   = hmm_ok   ? sc.w_hmm   : 0.0;
    double w_msar_eff  = msar_ok  ? sc.w_msar  : 0.0;
    double w_garch_eff = garch_ok ? sc.w_garch : 0.0;
    double w_gmm_eff   = gmm_ok   ? sc.w_gmm   : 0.0;

    double w_total = w_hmm_eff + w_msar_eff + w_garch_eff + w_gmm_eff;

    if (w_total < 1e-10) {
        MarketBelief fallback;
        fallback.sleeve_id = sleeve;
        for (int r = 0; r < kNumMarketRegimes; ++r)
            fallback.market_probs[static_cast<MarketRegimeL1>(r)] = 1.0 / kNumMarketRegimes;
        fallback.most_likely = sleeve_states_[s].last_belief.most_likely;
        fallback.confidence = 0.0;
        fallback.regime_age_bars = sleeve_states_[s].last_belief.regime_age_bars + 1;
        fallback.timestamp = std::chrono::system_clock::now();
        sleeve_states_[s].last_belief = fallback;
        return fallback;
    }

    // Renormalise weights
    w_hmm_eff   /= w_total;
    w_msar_eff  /= w_total;
    w_garch_eff /= w_total;
    w_gmm_eff   /= w_total;

    if (!hmm_ok)   std::cerr << "[WARN] HMM failed for " << sleeve_name(sleeve) << "\n";
    if (!msar_ok)  std::cerr << "[WARN] MSAR failed for " << sleeve_name(sleeve) << "\n";
    if (!garch_ok) std::cerr << "[WARN] GARCH failed for " << sleeve_name(sleeve) << "\n";
    if (!gmm_ok)   std::cerr << "[WARN] GMM failed for " << sleeve_name(sleeve) << "\n";

    if (!hmm_ok)   p_hmm.setZero();
    if (!msar_ok)  p_msar.setZero();
    if (!garch_ok) p_garch.setZero();
    if (!gmm_ok)   p_gmm.setZero();

    // Aggregate (explicit loop to avoid Eigen expression template issues)
    Eigen::Matrix<double, kNumMarketRegimes, 1> p_raw;
    for (int r = 0; r < kNumMarketRegimes; ++r) {
        p_raw(r) = w_hmm_eff   * p_hmm(r) +
                   w_msar_eff  * p_msar(r) +
                   w_garch_eff * p_garch(r) +
                   w_gmm_eff   * p_gmm(r);
        p_raw(r) = std::max(0.0, p_raw(r));
    }
    double raw_sum = p_raw.sum();
    if (raw_sum > kEps) p_raw /= raw_sum;
    else p_raw.setConstant(1.0 / kNumMarketRegimes);

    // A7: EWMA smoothing — p_final = λ p_raw + (1-λ) p_final_{t-1}
    auto p_smooth = smooth(sleeve, p_raw);

    // most_likely = argmax(p_final) per PDF (no hysteresis, no dwell)
    int argmax_idx;
    p_smooth.maxCoeff(&argmax_idx);
    auto dominant = static_cast<MarketRegimeL1>(argmax_idx);

    // Build MarketBelief (A7 output: p_final, confidence, regime age + stability, provenance)
    MarketBelief belief;
    belief.sleeve_id = sleeve;
    for (int r = 0; r < kNumMarketRegimes; ++r) {
        auto regime = static_cast<MarketRegimeL1>(r);
        belief.market_probs[regime] = p_smooth(r);
    }
    belief.most_likely = dominant;
    belief.confidence  = compute_confidence(p_smooth);
    belief.stability   = compute_stability(p_smooth);

    // Provenance (model_contributions)
    for (int r = 0; r < kNumMarketRegimes; ++r) {
        auto regime = static_cast<MarketRegimeL1>(r);
        belief.model_contributions["HMM"][regime]   = p_hmm(r);
        belief.model_contributions["MSAR"][regime]  = p_msar(r);
        belief.model_contributions["GARCH"][regime] = p_garch(r);
        belief.model_contributions["GMM"][regime]   = p_gmm(r);
    }

    // Regime age (bars since argmax last changed)
    if (dominant == sleeve_states_[s].last_belief.most_likely)
        belief.regime_age_bars = sleeve_states_[s].last_belief.regime_age_bars + 1;
    else
        belief.regime_age_bars = 1;

    belief.timestamp = std::chrono::system_clock::now();
    sleeve_states_[s].last_belief = belief;

    return belief;
}

// ============================================================================
// Accessors
// ============================================================================

bool MarketRegimePipeline::is_trained(SleeveId sleeve) const {
    return sleeve_states_[static_cast<int>(sleeve)].trained;
}

const MarketBelief& MarketRegimePipeline::last_belief(SleeveId sleeve) const {
    return sleeve_states_[static_cast<int>(sleeve)].last_belief;
}

} // namespace statistics
} // namespace trade_ngin
