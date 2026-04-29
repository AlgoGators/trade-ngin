#pragma once

#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/statistics/state_estimation/dynamic_factor_model.hpp"
#include "trade_ngin/statistics/state_estimation/ms_dfm.hpp"
#include "trade_ngin/statistics/state_estimation/macro_data_loader.hpp"
#include "trade_ngin/statistics/state_estimation/bsts_regime_detector.hpp"

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace trade_ngin {
namespace statistics {

// ============================================================================
// Macro Regime Ontology — 6 states (growth x inflation)
// ============================================================================

enum class MacroRegimeL1 : int {
    EXPANSION_DISINFLATION  = 0,
    EXPANSION_INFLATIONARY  = 1,
    SLOWDOWN_DISINFLATION   = 2,
    SLOWDOWN_INFLATIONARY   = 3,
    // M-08: relabelled from RECESSION_* to INDUSTRIAL_WEAKNESS_* (2026-04-29).
    // The DFM is services-blind (factor 1 captures industrial slack only —
    // cap-util, IP, manufacturing). Calling this "RECESSION" overstated what
    // the model actually identifies. Re-instate RECESSION_* once ISM-services
    // or PCE-services is added to the macro panel and DFM is re-fit.
    INDUSTRIAL_WEAKNESS_DEFLATIONARY  = 4,
    INDUSTRIAL_WEAKNESS_INFLATIONARY  = 5
};

static constexpr int kNumMacroRegimes = 6;
static constexpr int kDFMFactors      = 3;
static constexpr int kMSDFMNative     = 3;
static constexpr int kBSTSClusters    = 4;
static constexpr int kQuadrants       = 4;
static constexpr int kFingerprintDim  = 5;  // growth, inflation, credit, yield, policy

const char* macro_regime_name(MacroRegimeL1 r);

// ============================================================================
// MacroBelief — pipeline output
// ============================================================================

struct MacroBelief {
    std::map<MacroRegimeL1, double> macro_probs;
    MacroRegimeL1 most_likely = MacroRegimeL1::EXPANSION_DISINFLATION;
    double confidence = 0.0;

    // Overlay: structural break risk (populated from BSTS structural_break input)
    bool structural_break_risk = false;

    // L-31: policy_restrictive, credit_tightening, inflation_sticky removed
    // 2026-04-28. The spec (regime_aware_portfolio_engine.md) defines these
    // as outputs of dedicated overlay detectors that have not yet been built.
    // Leaving them silently `false` was worse than not having them — downstream
    // code could accidentally rely on a value that was never populated.
    // When the overlay detectors are implemented, re-add these fields with
    // their populating logic.

    // Per-model contributions: model_name -> {regime -> prob}
    std::map<std::string, std::map<MacroRegimeL1, double>> model_contributions;

    std::chrono::system_clock::time_point timestamp;
    int regime_age_bars = 0;
};

// ============================================================================
// MacroRegimePipelineConfig
// ============================================================================

struct MacroRegimePipelineConfig : public ConfigBase {

    // Model weights (sum to ~1.0; BSTS + future ML fill remainder)
    double w_dfm      = 0.25;
    double w_msdfm    = 0.40;
    double w_quadrant = 0.20;
    double w_bsts     = 0.10;

    // B1: DFM Gaussian training — percentile thresholds
    // CALIBRATION NOTE: The spec uses terciles (0.667/0.333) which assigns
    // 33% of history to each growth bucket equally. This labels too much
    // normal slowdown as "recession" (26.8% of 2011-2026 sample). Using
    // 70/30 splits: top 30% = expansion, bottom 20% = recession, middle
    // 50% = slowdown. This better reflects economic reality where
    // expansions are more common than recessions.
    double growth_upper_pctile = 0.70;    // top 30% = expansion
    double growth_lower_pctile = 0.20;    // bottom 20% = recession, middle 50% = slowdown
    double inflation_pctile    = 0.50;    // median (unchanged)

    // B1: DFM factor-to-axis mapping
    // DFM factors are identified only up to rotation — the pipeline needs to
    // know which factor index corresponds to growth and which to inflation.
    // Verify by inspecting DFM loadings: the growth factor should load heavily
    // on GDP, payrolls, industrial production; the inflation factor on CPI, PCE,
    // breakevens. If the factors are swapped, change these indices.
    // Based on observed DFM loadings (2011-2026 macro panel):
    //   Factor 0 (macro_level): CPI, retail_sales, GDP — overall scale, not clean growth
    //   Factor 1 (real_activity): mfg_capacity_util, industrial_prod, unemployment — this is growth
    //   Factor 2 (commodity_inflation): wti_crude, breakeven_5y — this is inflation
    int growth_factor_idx    = 1;  // factor 1 = real_activity → growth axis
    int inflation_factor_idx = 2;  // factor 2 = commodity_inflation → inflation axis

    // Sign flips for DFM factor axes.
    // DFM factors are identified up to sign — if the dominant loadings on a
    // factor are negative (e.g. manufacturing_capacity_util at -0.603), then
    // high factor values mean LOW activity. Set flip to -1 to invert so that
    // high values = expansion / high inflation as the labeling expects.
    double growth_factor_sign    = -1.0;  // factor 1 loads negatively on activity → flip
    double inflation_factor_sign = -1.0;  // factor 2 loads negatively on commodities → flip

    // B1: DFM EM iterations (spec default 200 is insufficient for 24-series panel)
    int    dfm_max_em_iterations = 500;

    // B2: MS-DFM fingerprint softmax temperature
    double fingerprint_tau = 1.0;

    // B3: Quadrant boundary blending bandwidth (normalised distance)
    double quadrant_blend_band = 0.3;

    // B4: BSTS contribution mode
    // CALIBRATION NOTE: The spec says BSTS outputs uniform when no break is
    // detected, deferring to other models. In practice this wastes 10% of
    // pipeline weight on uninformative uniform distributions for ~98% of
    // timesteps. Setting this to true makes BSTS always contribute its
    // fingerprint mapping, which better utilises its regime signal.
    bool bsts_always_contribute = true;

    // Persistence smoothing
    // CALIBRATION NOTE: The spec's base_lambda=0.10 with calm_scale=0.5
    // was designed for monthly data (~12 obs/year). On daily data (~252
    // obs/year) the effective lambda of 0.05 makes regime transitions
    // nearly impossible. Raised to base=0.20, calm_scale=1.0 so the
    // pipeline can respond to genuine regime shifts within ~2-3 weeks.
    double base_lambda       = 0.20;
    double calm_lambda_scale = 1.0;
    double shock_lambda_scale = 3.0;
    double shock_threshold   = 0.10;

    // Hysteresis — asymmetric enter/exit thresholds
    // CALIBRATION NOTE: The spec thresholds (enter_aggressive=0.70,
    // enter_riskoff=0.60) assume high concentration on single regimes.
    // With 6 regimes and 4 models that often disagree, the dominant
    // regime rarely exceeds 0.35. Thresholds set so that a regime with
    // clear plurality (>20%) can become dominant, while requiring the
    // current regime to have lost its lead (<15%) before transitioning.
    // This allows the pipeline to follow its own probability signals
    // rather than getting locked into stale regimes.
    double enter_defensive_thresh  = 0.20;
    double exit_defensive_thresh   = 0.15;
    double enter_riskoff_thresh    = 0.22;
    double exit_riskoff_thresh     = 0.15;
    double enter_aggressive_thresh = 0.20;
    double exit_aggressive_thresh  = 0.15;

    // Minimum dwell time before regime transition
    // CALIBRATION NOTE: Spec says 6 months. On daily data that's ~130 bars.
    // Using 26 bars (~1 month) to allow meaningful regime detection while
    // still preventing daily thrashing.
    int    min_dwell_bars = 26;
    double dwell_penalty  = 0.3;

    // Fingerprint column groups (populated at train time)
    std::vector<std::string> growth_columns = {
        "nonfarm_payrolls", "unemployment_rate", "gdp",
        "manufacturing_capacity_util", "industrial_production", "retail_sales"
    };
    std::vector<std::string> inflation_columns = {
        "cpi", "core_cpi", "core_pce", "breakeven_5y"
    };
    std::vector<std::string> credit_columns = {
        "ig_credit_spread", "high_yield_spread"
    };
    std::vector<std::string> yield_curve_columns = {
        "yield_spread_10y_2y"
    };
    std::vector<std::string> policy_columns = {
        "fed_funds_rate"
    };

    nlohmann::json to_json() const override;
    void from_json(const nlohmann::json& j) override;
};

// ============================================================================
// Internal trained-parameter structs
// ============================================================================

// B1: One Gaussian per regime for DFM soft assignment.
// M-04: 2D over factors [1, 2] = (real_activity, commodity_inflation).
// Factor 0 (macro_level) is non-discriminative trend (loads ~0.85 on
// secular-growth series); excluding it from the Gaussian sharpens the
// soft-classification probabilities. DFM still decomposes K=3 factors
// (factor 0 still consumed by MS-DFM via emission likelihood); only
// the Gaussian classifier operates in 2D.
struct RegimeGaussian {
    Eigen::Vector2d mean    = Eigen::Vector2d::Zero();
    Eigen::Matrix2d cov     = Eigen::Matrix2d::Identity();
    Eigen::Matrix2d cov_inv = Eigen::Matrix2d::Identity();
    double log_det = 0.0;
    int    n_samples = 0;
};

// B2: Fingerprint-based mapping for MS-DFM (and optionally BSTS)
struct FingerprintMapping {
    Eigen::MatrixXd mapping_matrix;  // J_native x 6 (rows sum to 1)
    std::vector<Eigen::VectorXd> native_fingerprints;  // J vectors, dim=kFingerprintDim
    std::vector<Eigen::VectorXd> target_fingerprints;  // 6 vectors, dim=kFingerprintDim
    double tau = 1.0;
};

// ============================================================================
// MacroRegimePipeline
//
// Combines 4 models into a single MacroBelief:
//   B1) DFM          → trained Gaussians      → 6 probs
//   B2) MS-DFM       → fingerprint mapping    → 6 probs
//   B3) Quadrant     → rule-based table       → 6 probs
//   B4) BSTS/GMM     → conditional fingerprint → 6 probs
//
// Aggregation: weighted sum → persistence smoothing → hysteresis → MacroBelief
// ============================================================================

class MacroRegimePipeline {
public:

    explicit MacroRegimePipeline(MacroRegimePipelineConfig config = {});

    // ── Training ─────────────────────────────────────────────────────────────

    // Full training from all model outputs + macro panel.
    Result<void> train(
        const DFMOutput& dfm_output,
        const MSDFMOutput& msdfm_output,
        const MacroPanel& panel,
        const Eigen::MatrixXd& bsts_probs,       // T x 4 GMM cluster probs
        const Eigen::VectorXd& growth_scores,     // T
        const Eigen::VectorXd& inflation_scores   // T
    );

    // Convenience: train from BSTSOutput directly (extracts probs + composites).
    Result<void> train(
        const DFMOutput& dfm_output,
        const MSDFMOutput& msdfm_output,
        const MacroPanel& panel,
        const BSTSOutput& bsts_output
    );

    // ── Runtime inference ────────────────────────────────────────────────────

    // Single-step update: provide current outputs from all 4 models.
    Result<MacroBelief> update(
        const Eigen::Vector3d& dfm_factors,         // B1: current f_t
        const Eigen::Vector3d& msdfm_native_probs,  // B2: current native probs
        double growth_score,                          // B3: current growth
        double inflation_score,                       // B3: current inflation
        const Eigen::Vector4d& bsts_cluster_probs,   // B4: current GMM probs
        bool structural_break = false                 // B4: break detected?
    );

    // ── Accessors ────────────────────────────────────────────────────────────

    bool is_trained() const { return trained_; }
    const MacroBelief& last_belief() const { return last_belief_; }
    const MacroRegimePipelineConfig& config() const { return config_; }

    // Expose trained parameters for diagnostics
    const std::array<RegimeGaussian, kNumMacroRegimes>& dfm_gaussians() const {
        return dfm_gaussians_;
    }
    const FingerprintMapping& msdfm_mapping() const { return msdfm_mapping_; }

    // L-32: refresh the Quadrant model's z-score basis (growth_median_,
    // growth_std_, inflation_median_, inflation_std_) from a fresh set of
    // composite scores without re-running full train(). Caller responsibility
    // to call periodically as live data drifts away from the training window.
    //
    // Pre-fix: stats were baked at train() time and never updated. Live
    // operation drifted away from the training distribution over months/
    // quarters, miscalibrating Quadrant's tanh-blended weights.
    Result<void> recalibrate_quadrant_stats(
        const Eigen::VectorXd& growth_scores,
        const Eigen::VectorXd& inflation_scores);

private:

    // ── B1: DFM → 6 probs via trained Gaussians ─────────────────────────────

    void train_dfm_gaussians(const DFMOutput& dfm_output);

    Eigen::Matrix<double, kNumMacroRegimes, 1>
    map_dfm(const Eigen::Vector3d& f_t) const;

    // ── B2: MS-DFM → 6 probs via fingerprint mapping ────────────────────────

    void train_msdfm_fingerprints(const MSDFMOutput& msdfm_output,
                                   const MacroPanel& panel);

    Eigen::Matrix<double, kNumMacroRegimes, 1>
    map_msdfm(const Eigen::Vector3d& p_native) const;

    // ── B3: Quadrant → 6 probs via rule-based table ─────────────────────────

    Eigen::Matrix<double, kNumMacroRegimes, 1>
    map_quadrant(double growth_score, double inflation_score) const;

    // ── B4: BSTS → 6 probs (uniform if no break, fingerprint if break) ─────

    void train_bsts_fingerprints(const Eigen::MatrixXd& bsts_probs,
                                  const MacroPanel& panel);

    Eigen::Matrix<double, kNumMacroRegimes, 1>
    map_bsts(const Eigen::Vector4d& cluster_probs, bool structural_break) const;

    // ── Aggregation ──────────────────────────────────────────────────────────

    Eigen::Matrix<double, kNumMacroRegimes, 1> aggregate(
        const Eigen::Matrix<double, kNumMacroRegimes, 1>& p_dfm,
        const Eigen::Matrix<double, kNumMacroRegimes, 1>& p_msdfm,
        const Eigen::Matrix<double, kNumMacroRegimes, 1>& p_quad,
        const Eigen::Matrix<double, kNumMacroRegimes, 1>& p_bsts
    ) const;

    // ── Persistence smoothing with adaptive lambda ───────────────────────────

    Eigen::Matrix<double, kNumMacroRegimes, 1>
    smooth(const Eigen::Matrix<double, kNumMacroRegimes, 1>& p_raw,
           bool shock_detected);

    // ── Hysteresis + minimum dwell ───────────────────────────────────────────

    MacroRegimeL1 apply_hysteresis(
        Eigen::Matrix<double, kNumMacroRegimes, 1>& p_smooth);

    // ── Helpers ──────────────────────────────────────────────────────────────

    double compute_confidence(
        const Eigen::Matrix<double, kNumMacroRegimes, 1>& probs) const;

    // Resolve column names → indices in MacroPanel
    void resolve_column_indices(const MacroPanel& panel);

    // Build the per-column standardized panel used by compute_fingerprint.
    // For growth columns: YoY absolute differences then z-scored against
    // the full-sample mean/std of that diff series ("momentum", per .md spec).
    // For all other columns: z-scored against full-sample level mean/std.
    // This eliminates unit mixing across columns and the time-trend
    // confound that previously locked MS-DFM onto SLOWDOWN_DISINFLATION.
    void prepare_fingerprint_data(const MacroPanel& panel);

    // Compute fingerprint from panel rows at given time indices.
    // Reads from fp_panel_ (built by prepare_fingerprint_data), not panel.data.
    Eigen::VectorXd compute_fingerprint(
        const MacroPanel& panel,
        const std::vector<int>& time_indices) const;

    // Get enter/exit thresholds for a regime class
    double get_enter_threshold(MacroRegimeL1 r) const;
    double get_exit_threshold(MacroRegimeL1 r) const;

    // Percentile helper
    static double percentile(const std::vector<double>& data, double p);

    // ── State ────────────────────────────────────────────────────────────────

    MacroRegimePipelineConfig config_;
    bool trained_ = false;

    // B1: trained Gaussians
    std::array<RegimeGaussian, kNumMacroRegimes> dfm_gaussians_;

    // B2: fingerprint mapping
    FingerprintMapping msdfm_mapping_;

    // B3: hardcoded quadrant table (4 x 6)
    Eigen::Matrix<double, kQuadrants, kNumMacroRegimes> quadrant_table_;

    // B4: BSTS fingerprint mapping (for break-detected case)
    FingerprintMapping bsts_mapping_;

    // Column index caches for fingerprint computation
    std::vector<int> growth_col_idx_;
    std::vector<int> inflation_col_idx_;
    std::vector<int> credit_col_idx_;
    std::vector<int> yield_col_idx_;
    std::vector<int> policy_col_idx_;

    // Per-column-standardized panel (T x N). Each column z-scored against
    // its full-sample mean/std; growth columns YoY-differenced first.
    // compute_fingerprint averages these standardized values per group.
    Eigen::MatrixXd fp_panel_;

    // Lookback for YoY momentum transform on growth columns (trading days).
    static constexpr int kYoYLag = 252;

    // Fingerprint standardisation (mean/std per fingerprint dimension)
    Eigen::VectorXd fp_mean_;
    Eigen::VectorXd fp_std_;

    // Quadrant medians (from training growth/inflation scores)
    double growth_median_    = 0.0;
    double inflation_median_ = 0.0;
    double growth_std_       = 1.0;
    double inflation_std_    = 1.0;

    // Runtime state
    MacroBelief last_belief_;
    Eigen::Matrix<double, kNumMacroRegimes, 1> prev_smoothed_;
    MacroRegimeL1 current_regime_ = MacroRegimeL1::EXPANSION_DISINFLATION;
    int dwell_counter_ = 0;
    int update_count_  = 0;  // for smoothing warmup

    mutable std::mutex mutex_;
};

} // namespace statistics
} // namespace trade_ngin
