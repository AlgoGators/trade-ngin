#pragma once

#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/statistics/clustering/gmm.hpp"

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace trade_ngin {
namespace statistics {

// ============================================================================
// Market Regime Ontology — 5 L1 states per sleeve
// (from Regime Engine Algo PDF, section A6)
// ============================================================================

enum class MarketRegimeL1 : int {
    TREND_LOWVOL       = 0,  // Strong directional, low vol
    TREND_HIGHVOL      = 1,  // Moderate directional, high vol
    MEANREV_CHOPPY     = 2,  // Weak/no trend, choppy
    STRESS_PRICE       = 3,  // Drawdown, spiking vol, adequate liquidity
    STRESS_LIQUIDITY   = 4   // Extreme vol, collapsed liquidity
};

static constexpr int kNumMarketRegimes = 5;

const char* market_regime_name(MarketRegimeL1 r);

// ============================================================================
// Market Overlays — modulators, not L1 states
// ============================================================================

enum class MarketOverlay {
    EVENT_SHOCK_TRANSIENT,
    CORRELATION_SPIKE,
    VOL_OF_VOL_HIGH,
    DISPERSION_HIGH,
    GAP_RISK_HIGH
};

// ============================================================================
// Sleeve Identifier
// ============================================================================

enum class SleeveId : int {
    EQUITIES    = 0,
    RATES       = 1,
    FX          = 2,
    COMMODITIES = 3
};

static constexpr int kNumSleeves = 4;

const char* sleeve_name(SleeveId s);

// ============================================================================
// MarketBelief — pipeline output per sleeve (from PDF section A7)
// ============================================================================

struct MarketBelief {
    SleeveId sleeve_id = SleeveId::EQUITIES;

    std::map<MarketRegimeL1, double> market_probs;  // 5 probs summing to 1
    MarketRegimeL1 most_likely = MarketRegimeL1::TREND_LOWVOL;
    double confidence = 0.0;

    // Per-model contributions: model_name -> {regime -> prob}
    std::map<std::string, std::map<MarketRegimeL1, double>> model_contributions;

    std::chrono::system_clock::time_point timestamp;
    int regime_age_bars = 0;
    double stability = 0.0;  // dwell progress: 0.0 = just entered, 1.0 = fully settled
};

// ============================================================================
// Shared Features — computed from raw market data (PDF section A0)
// These are reused across models and later become overlays/confidence drivers
// ============================================================================

struct MarketFeatures {
    double realized_vol       = 0.0;   // rolling realised vol (annualised)
    double drawdown           = 0.0;   // current drawdown from peak
    double drawdown_speed     = 0.0;   // drawdown velocity
    double vol_of_vol         = 0.0;   // volatility of volatility
    double correlation_spike  = 0.0;   // cross-asset correlation z-score
    double liquidity_proxy    = 0.0;   // volume ratio vs 20-bar average
};

// ============================================================================
// GARCH Feature Output — A3 "stress intensity meter" (PDF section A3)
// GARCH outputs features, NOT regime probabilities directly
// ============================================================================

struct GARCHFeatures {
    double conditional_vol  = 0.0;   // σ_t from GARCH
    double vol_percentile   = 0.0;   // percentile vs trailing history [0,1]
    bool   vol_spike        = false; // vol spike flag
    bool   vol_of_vol_high  = false; // vol-of-vol elevated flag
    bool   asymmetry_flag   = false; // EGARCH leverage effect (γ < 0 significant)
};

// ============================================================================
// Per-Sleeve Thresholds (from synthesized spec)
// ============================================================================

struct SleeveThresholds {
    // Volatility thresholds
    double trend_lowvol_vol_upper  = 0.12;   // vol < this = low vol
    double stress_vol_lower        = 0.35;   // vol > this = stress territory

    // Liquidity collapse definition
    double liq_spread_multiplier   = 3.0;    // spread > Nx normal
    double liq_volume_ratio        = 0.40;   // volume < X% of average
};

// ============================================================================
// Sleeve-Specific Config
// ============================================================================

struct SleeveConfig {
    SleeveId sleeve_id = SleeveId::EQUITIES;
    SleeveThresholds thresholds;

    // Model weights for this sleeve (sum to 1.0)
    // PDF: HMM 35-45%, MSAR 20-30%, GARCH 10-20%, GMM 0-10%
    // Without ML confirmer, redistributed:
    double w_hmm   = 0.40;
    double w_msar  = 0.30;
    double w_garch = 0.20;
    double w_gmm   = 0.10;

    // Symbols for this sleeve
    std::vector<std::string> symbols;
    AssetClass asset_class = AssetClass::FUTURES;

    // GARCH mapping — percentile thresholds that define the 4 vol bins
    // consumed by map_garch() when binning the causal vol percentile.
    double garch_vol_low_pctile     = 0.30;   // below this = low vol
    double garch_vol_high_pctile    = 0.70;   // above this = high vol
    double garch_vol_extreme_pctile = 0.90;   // above this = stress
};

// Default sleeve configurations (from synthesized spec thresholds)
SleeveConfig default_equities_config();
SleeveConfig default_rates_config();
SleeveConfig default_fx_config();
SleeveConfig default_commodities_config();

// ============================================================================
// MarketRegimePipelineConfig
// ============================================================================

struct MarketRegimePipelineConfig : public ConfigBase {

    // Per-sleeve configs
    std::array<SleeveConfig, kNumSleeves> sleeve_configs;

    // HMM config (A1: 3 states for market)
    int hmm_n_states       = 3;
    int hmm_max_iterations = 200;

    // MSAR config (A2: 2 states)
    int msar_n_states = 2;
    int msar_ar_lag   = 1;

    // GMM config (A4)
    int gmm_n_clusters      = 5;
    int gmm_restarts         = 10;
    int gmm_max_iterations   = 300;

    // Fingerprint softmax temperature
    double fingerprint_tau = 1.0;

    // Persistence smoothing (PDF A7): p_final = λ·p_raw + (1-λ)·p_final_{t-1}
    // Single constant λ per PDF — no warmup, no adaptive scaling.
    double lambda = 0.30;

    // Feature computation
    int realized_vol_window = 20;     // rolling window for realized vol
    int vol_of_vol_window   = 60;     // rolling window for vol-of-vol
    int liquidity_window    = 20;     // volume average lookback
    int garch_vol_history   = 252;    // trailing bars for vol percentile

    MarketRegimePipelineConfig();

    nlohmann::json to_json() const override;
    void from_json(const nlohmann::json& j) override;
};

// ============================================================================
// Internal trained-parameter structs
// ============================================================================

// Fingerprint mapping: maps J native states → 5 ontology states
// Used by HMM (3→5), MSAR (2→5), GMM (K→5)
// Fingerprint dimensions are model-specific:
//   HMM:  2D [μ_i, σ_i]          — from emission parameters
//   MSAR: 3D [μ_i, σ_i, φ_i]    — from regime parameters
//   GMM:  5D [r, σ̂, dd_speed, vol_shock, corr_spike] — from feature averages

struct MarketFingerprintMapping {
    Eigen::MatrixXd mapping_matrix;                    // J × 5 (rows sum to 1)
    std::vector<Eigen::VectorXd> native_fingerprints;  // J vectors (dim varies per model)
    std::vector<Eigen::VectorXd> target_fingerprints;  // 5 vectors (same dim as native)
    double tau = 1.0;

    // Per-model standardisation (computed from native fingerprints)
    Eigen::VectorXd fp_mean;
    Eigen::VectorXd fp_std;
};

// GARCH mapping parameters: rule-based 4-bin vol table that maps
// vol_percentile (causal, trailing-window, supplied at runtime) to the
// 5 ontology states, plus post-table adjustments for spike/vov/asymmetry/
// liquidity flags in map_garch().
struct GARCHMappingParams {
    // Rule-based mapping table: 4 vol bins × 5 ontology states
    Eigen::MatrixXd mapping_table = Eigen::MatrixXd::Zero(4, kNumMarketRegimes);

    bool trained = false;
};

// ============================================================================
// Per-sleeve trained state
// ============================================================================

struct SleeveTrainedState {
    // A1: HMM fingerprint mapping
    MarketFingerprintMapping hmm_mapping;

    // A2: MSAR fingerprint mapping
    MarketFingerprintMapping msar_mapping;

    // A3: GARCH feature mapping
    GARCHMappingParams garch_mapping;

    // A4: GMM fingerprint mapping
    MarketFingerprintMapping gmm_mapping;
    GMMResult                gmm_model;    // trained GMM for runtime predict_proba

    // Runtime state — only the EWMA recurrence state (PDF A7)
    Eigen::Matrix<double, kNumMarketRegimes, 1> prev_smoothed;
    MarketBelief last_belief;

    // L-33: per-sleeve update counter for the warmup window.
    // First N updates use λ=1 (pure raw, bypassing the uniform prev_smoothed
    // init) so the pipeline can find its natural starting regime before
    // EWMA smoothing kicks in. Mirrors the macro pipeline's warmup logic
    // and structurally subsumes K-09 (smoothing init contamination from
    // the runner's last-5-bars update loop).
    int update_count = 0;

    bool trained = false;
};

// ============================================================================
// MarketRegimePipeline
//
// Combines 4 models into a single MarketBelief per sleeve:
//   A1) HMM          → fingerprint mapping   → 5 probs   (40%)
//   A2) MSAR         → fingerprint mapping   → 5 probs   (30%)
//   A3) GARCH        → feature-based mapping  → 5 probs   (20%)
//   A4) GMM          → fingerprint mapping   → 5 probs   (10%)
//
// Aggregation (A7): weighted sum → EWMA smoothing (single constant λ) → argmax
//
// Source: Regime Engine Algo (1).pdf, sections A0-A7 (A5 ML confirmer omitted).
// ============================================================================

class MarketRegimePipeline {
public:
    explicit MarketRegimePipeline(MarketRegimePipelineConfig config = {});

    // ── Training (per sleeve) ───────────────────────────────────────────────

    // Train all model mappings for one sleeve.
    // Uses model state signatures (not feature averages) per PDF A6.
    Result<void> train(
        SleeveId sleeve,
        const std::vector<double>& returns,            // T return series
        // A1: HMM state signatures
        const std::vector<Eigen::VectorXd>& hmm_means, // K emission means
        const std::vector<Eigen::MatrixXd>& hmm_covs,  // K emission covariances
        // A2: MSAR state signatures
        const Eigen::VectorXd& msar_state_means,       // J regime means (μ_i)
        const Eigen::VectorXd& msar_state_vars,        // J regime variances (σ²_i)
        const Eigen::MatrixXd& msar_ar_coeffs,         // J × lag AR coefficients (φ_i)
        // A3: GARCH vol series
        const std::vector<double>& garch_vol_series,   // T conditional vol from GARCH
        // A4: GMM (fitted on [r_t, σ̂_t, dd_speed, vol_shock, corr_spike])
        const GMMResult& gmm_result,
        const Eigen::MatrixXd& gmm_feature_matrix,     // T × 5 GMM feature space
        // K-05 (optional): liquidity 3rd dim for HMM fingerprint.
        //   liquidity_proxy_series: T-length per-bar volume ratio (NaN where unavailable)
        //   hmm_smoothed_probs:     T × K_hmm smoothed posteriors from MS/HMM EM
        // K-05+ (optional): drawdown 4th dim — per-bar rolling 60-day cumulative
        //   return. Captures slow-bear stress (moderate σ + persistent neg 60d).
        // Pass empty / default-constructed to fall back to lower-D HMM fingerprint.
        const std::vector<double>& liquidity_proxy_series = {},
        const Eigen::MatrixXd& hmm_smoothed_probs = Eigen::MatrixXd(),
        const std::vector<double>& ret60_series = {}
    );

    // ── Runtime inference (per sleeve) ──────────────────────────────────────

    // Single-step update for one sleeve.
    Result<MarketBelief> update(
        SleeveId sleeve,
        const Eigen::VectorXd& hmm_state_probs,    // A1: 3 native probs
        const Eigen::VectorXd& msar_state_probs,   // A2: 2 native probs
        const GARCHFeatures& garch_features,        // A3: stress features
        const MarketFeatures& market_features,       // shared features
        const Eigen::VectorXd& gmm_cluster_probs    // A4: K cluster probs
    );

    // ── Accessors ───────────────────────────────────────────────────────────

    bool is_trained(SleeveId sleeve) const;
    const MarketBelief& last_belief(SleeveId sleeve) const;
    const MarketRegimePipelineConfig& config() const { return config_; }

    const SleeveTrainedState& sleeve_state(SleeveId sleeve) const {
        return sleeve_states_[static_cast<int>(sleeve)];
    }

private:
    // ── A1: HMM fingerprint mapping (μ_i, σ_i, +liq if K-05, +ret60 if K-05+) ──
    void train_hmm_fingerprints(
        SleeveId sleeve,
        const std::vector<Eigen::VectorXd>& hmm_means,
        const std::vector<Eigen::MatrixXd>& hmm_covs,
        const std::vector<double>& liquidity_proxy_series = {},
        const Eigen::MatrixXd& hmm_smoothed_probs = Eigen::MatrixXd(),
        const std::vector<double>& ret60_series = {});

    Eigen::Matrix<double, kNumMarketRegimes, 1>
    map_hmm(SleeveId sleeve, const Eigen::VectorXd& p_native) const;

    // ── A2: MSAR fingerprint mapping (uses μ_i, σ_i, φ_i) ──────────────────
    void train_msar_fingerprints(
        SleeveId sleeve,
        const Eigen::VectorXd& state_means,
        const Eigen::VectorXd& state_vars,
        const Eigen::MatrixXd& ar_coeffs);

    Eigen::Matrix<double, kNumMarketRegimes, 1>
    map_msar(SleeveId sleeve, const Eigen::VectorXd& p_native) const;

    // ── A3: GARCH feature-based mapping ─────────────────────────────────────
    void train_garch_mapping(
        SleeveId sleeve,
        const Eigen::MatrixXd& features,
        const std::vector<double>& garch_vol_series);

    Eigen::Matrix<double, kNumMarketRegimes, 1>
    map_garch(SleeveId sleeve,
              const GARCHFeatures& garch,
              const MarketFeatures& market) const;

    // ── A4: GMM fingerprint mapping (on [r_t, σ̂_t, dd_speed, vol_shock, corr]) ─
    void train_gmm_fingerprints(
        SleeveId sleeve,
        const GMMResult& gmm_result,
        const Eigen::MatrixXd& gmm_features);  // T × 5 GMM-specific feature matrix

    Eigen::Matrix<double, kNumMarketRegimes, 1>
    map_gmm(SleeveId sleeve, const Eigen::VectorXd& p_cluster) const;

    // ── Aggregation ─────────────────────────────────────────────────────────
    Eigen::Matrix<double, kNumMarketRegimes, 1>
    aggregate(SleeveId sleeve,
              const Eigen::Matrix<double, kNumMarketRegimes, 1>& p_hmm,
              const Eigen::Matrix<double, kNumMarketRegimes, 1>& p_msar,
              const Eigen::Matrix<double, kNumMarketRegimes, 1>& p_garch,
              const Eigen::Matrix<double, kNumMarketRegimes, 1>& p_gmm) const;

    // ── A7 EWMA smoothing (PDF constant λ) ──────────────────────────────────
    Eigen::Matrix<double, kNumMarketRegimes, 1>
    smooth(SleeveId sleeve,
           const Eigen::Matrix<double, kNumMarketRegimes, 1>& p_raw);

    double compute_confidence(
        const Eigen::Matrix<double, kNumMarketRegimes, 1>& probs) const;
    double compute_stability(
        const Eigen::Matrix<double, kNumMarketRegimes, 1>& probs) const;

    // ── Fingerprint helpers ─────────────────────────────────────────────────
    // Compute mean feature vector for a set of timestep indices (for GMM)
    static Eigen::VectorXd compute_feature_fingerprint(
        const Eigen::MatrixXd& features,
        const std::vector<int>& indices);

    // Standardise + build softmax mapping matrix
    void standardise_and_build_mapping(
        MarketFingerprintMapping& mapping, double tau) const;

    static double percentile(const std::vector<double>& data, double p);

    // ── State ───────────────────────────────────────────────────────────────
    MarketRegimePipelineConfig config_;
    std::array<SleeveTrainedState, kNumSleeves> sleeve_states_;
    mutable std::mutex mutex_;
};

} // namespace statistics
} // namespace trade_ngin
