#pragma once

#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"
#include "trade_ngin/regime_detection/macro/macro_data_loader.hpp"

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace trade_ngin {

class PostgresDatabase;  // forward declaration

namespace statistics {

// ============================================================================
// BSTSConfig
// ============================================================================

struct BSTSConfig : public ConfigBase {

    // Kalman filter / sigma optimisation
    bool   optimise_sigma = true;   // MLE grid search vs manual sigmas

    // Feature engineering
    int    innovation_window        = 8;
    int    gaussian_smooth_radius   = 4;
    double gaussian_smooth_sigma    = 2.0;

    // PCA
    int    pca_components           = 12;

    // GMM
    int    num_regimes              = 4;
    int    gmm_max_iterations       = 300;
    double gmm_tolerance            = 1e-5;
    int    gmm_restarts             = 10;
    double high_conviction_threshold = 0.20;

    // Series definitions (default 8 ETFs + 12 macro)
    std::vector<std::string> etf_series = {
        "SPY", "EEM", "TLT", "HYG", "GLD", "UUP", "USO", "CPER"
    };
    std::vector<std::string> macro_series = {
        "cpi", "core_pce", "breakeven_5y", "gdp",
        "manufacturing_capacity_util", "industrial_production",
        "yield_spread_10y_2y", "tips_10y", "high_yield_spread",
        "unemployment_rate", "nonfarm_payrolls", "ted_spread"
    };

    nlohmann::json to_json() const override;
    void from_json(const nlohmann::json& j) override;
};

// ============================================================================
// BSTSOutput
// ============================================================================

struct BSTSOutput {

    // ---- Regime assignments (T rows) ────────────────────────────────────
    Eigen::VectorXi regime_labels;          // T-dim, mapped (0-3 semantic)
    Eigen::VectorXi regime_labels_raw;      // T-dim, raw GMM cluster
    Eigen::MatrixXd regime_posteriors;      // T x K soft probabilities
    Eigen::VectorXd regime_entropy;         // T-dim normalised Shannon

    // ---- Macro composites (T rows) ─────────────────────────────────────
    Eigen::VectorXd risk_on_score;
    Eigen::VectorXd growth_score;
    Eigen::VectorXd inflation_score;
    Eigen::VectorXd growth_inflation_quad;

    // ---- Feature matrix + PCA ──────────────────────────────────────────
    Eigen::MatrixXd features;              // T x 50 (raw feature matrix)
    Eigen::MatrixXd pca_transformed;       // T x pca_components
    Eigen::VectorXd pca_variance_explained;
    std::vector<std::string> feature_names; // 50 names

    // ---- Per-series BSTS results ───────────────────────────────────────
    // Smoothed levels and slopes for all 20 series (8 ETF + 12 macro)
    Eigen::MatrixXd smoothed_levels;       // T x 20
    Eigen::MatrixXd smoothed_slopes;       // T x 20

    // ---- GMM parameters ────────────────────────────────────────────────
    Eigen::VectorXd gmm_weights;           // K cluster prior weights
    std::vector<Eigen::VectorXd> gmm_means;
    std::vector<Eigen::MatrixXd> gmm_covariances;
    std::vector<int> regime_label_mapping; // raw cluster → semantic regime

    // ---- Dates + metadata ──────────────────────────────────────────────
    std::vector<std::string> dates;
    std::vector<std::string> etf_names;
    std::vector<std::string> macro_names;

    // ---- Diagnostics ───────────────────────────────────────────────────
    double gmm_log_likelihood = 0.0;
    double pca_variance_total = 0.0;
    double high_conviction_pct = 0.0;
    ConvergenceInfo convergence_info;
    int T = 0;
    int num_etfs = 0;
    int num_macros = 0;
    int num_regimes = 0;
};

// ============================================================================
// BSTSRegimeDetector
//
// Multi-asset BSTS macro regime detector (B4).
// Runs Local Linear Trend BSTS per series (Kalman + RTS smoother),
// extracts 50-dim posterior feature matrix, PCA to 12, GMM 4 regimes.
//
// Usage:
//   BSTSRegimeDetector bsts(config);
//   auto result = bsts.fit_from_db(db);
//   const auto& out = result.value();
//   // out.regime_posteriors, out.growth_score, etc.
// ============================================================================

class BSTSRegimeDetector {
public:

    explicit BSTSRegimeDetector(BSTSConfig config = BSTSConfig{});

    // Fit from raw data matrices.
    // etf_prices:  T x num_etfs  (raw prices, log-transform done internally)
    // macro_data:  T x num_macros (raw levels/rates)
    Result<BSTSOutput> fit(const Eigen::MatrixXd& etf_prices,
                           const Eigen::MatrixXd& macro_data,
                           const std::vector<std::string>& dates,
                           const std::vector<std::string>& etf_names = {},
                           const std::vector<std::string>& macro_names = {});

    // Convenience: load from DB and fit in one call.
    Result<BSTSOutput> fit_from_db(PostgresDatabase& db,
                                    const std::string& start_date = "",
                                    const std::string& end_date = "");

    bool              is_fitted()   const { return fitted_; }
    const BSTSOutput& last_output() const { return last_output_; }
    const BSTSConfig& config()      const { return config_; }

    // Regime name lookup
    static const char* regime_name(int label);

    // ── Stateless utility helpers (exposed for testing/diagnostics) ─────
    // These are pure transformations on data — no state, no dependencies.
    // Public for the regression test suite (Phase 0 substrate guards).
    static Eigen::VectorXd gaussian_smooth(const Eigen::VectorXd& x,
                                           int radius, double sigma);
    static void forward_fill(Eigen::MatrixXd& X);
    // backward_fill removed (lookahead bias) — see L-19 in audit doc.
    // leading_pad_with_first_valid replaces it for the legitimate
    // state-space init case (leading-NaN-only, not mid-panel).
    static void leading_pad_with_first_valid(Eigen::MatrixXd& X);

private:

    // ── Internal structs (match original BSTS exactly) ───────────────────

    struct KalmanResult {
        std::vector<Eigen::Vector2d> filtered_state;
        std::vector<Eigen::Matrix2d> filtered_cov;
        std::vector<Eigen::Vector2d> predicted_state;
        std::vector<Eigen::Matrix2d> predicted_cov;
        std::vector<double>          innovations;
        std::vector<double>          innovation_var;
        double log_likelihood = 0.0;
    };

    struct SmoothedResult {
        std::vector<Eigen::Vector2d> smoothed_state;
        std::vector<Eigen::Matrix2d> smoothed_cov;
    };

    struct SeriesPosterior {
        std::string         name;
        bool                is_etf;
        std::vector<double> raw_values;
        KalmanResult        kf;
        SmoothedResult      sm;
    };

    struct PCAResult {
        Eigen::MatrixXd transformed;
        Eigen::MatrixXd components;
        Eigen::VectorXd mean;
        Eigen::VectorXd std_dev;
        Eigen::VectorXd explained_variance_ratio;
    };

    struct GMMResult {
        int k = 0;
        Eigen::VectorXd              weights;
        std::vector<Eigen::VectorXd> means;
        std::vector<Eigen::MatrixXd> covariances;
        Eigen::MatrixXd              responsibilities;
        Eigen::VectorXi              labels;
        Eigen::VectorXd              entropy;
    };

    // ── BSTS core algorithms ─────────────────────────────────────────────

    static KalmanResult run_kalman(
        const std::vector<double>& y,
        double sigma_obs, double sigma_level, double sigma_slope);

    static SmoothedResult run_rts(const KalmanResult& kf);

    static std::tuple<double, double, double>
    mle_sigma(const std::vector<double>& y);

    SeriesPosterior fit_series(
        const std::string& name, const std::vector<double>& raw, bool is_etf) const;

    // ── Feature helpers ──────────────────────────────────────────────────

    static double smooth_level(const SeriesPosterior& sp, int t);
    static double smooth_slope(const SeriesPosterior& sp, int t);
    static Eigen::VectorXd rolling_innov_vol(const std::vector<double>& innov, int win);
    // gaussian_smooth moved to public section (testing/diagnostics access).

    // ── Feature extraction ───────────────────────────────────────────────

    Eigen::MatrixXd build_feature_matrix(
        const std::vector<SeriesPosterior>& etfs,
        const std::vector<SeriesPosterior>& macros,
        const std::unordered_map<std::string, int>& midx,
        std::vector<std::string>& feat_names,
        Eigen::VectorXd& ros_out,
        Eigen::VectorXd& growth_out,
        Eigen::VectorXd& inflation_out,
        Eigen::VectorXd& gi_quad_out) const;

    // ── PCA ──────────────────────────────────────────────────────────────

    static PCAResult run_pca(const Eigen::MatrixXd& X, int n_comp);

    // ── GMM ──────────────────────────────────────────────────────────────

    static double log_mvn_pdf(const Eigen::VectorXd& x,
                               const Eigen::VectorXd& mu,
                               const Eigen::MatrixXd& cov);

    static std::vector<Eigen::VectorXd> kmeans_plus_plus(
        const Eigen::MatrixXd& X, int K, std::mt19937& rng);

    std::pair<double, GMMResult> run_em(
        const Eigen::MatrixXd& X, int K,
        const std::vector<Eigen::VectorXd>& init_means) const;

    GMMResult fit_gmm(const Eigen::MatrixXd& X, int K, int seed = 42) const;

    // ── Regime labeling ──────────────────────────────────────────────────

    static std::vector<int> label_regimes(
        const GMMResult& gmm,
        const Eigen::VectorXd& growth,
        const Eigen::VectorXd& inflation,
        const Eigen::VectorXd& gi_quad,
        const Eigen::VectorXd& fin_stress,
        const Eigen::VectorXd& ros);

    // ── Database loading ─────────────────────────────────────────────────

    struct DataFrame {
        std::vector<std::string> dates;
        std::vector<std::string> columns;
        Eigen::MatrixXd          values;
    };

    DataFrame load_from_database(PostgresDatabase& db,
                                  const std::string& start_date,
                                  const std::string& end_date) const;

    // forward_fill, leading_pad_with_first_valid, gaussian_smooth moved
    // to public section for testing/diagnostics access.
    static int col_idx(const std::vector<std::string>& cols, const std::string& name);
    static std::vector<double> extract_col(const Eigen::MatrixXd& X, int j);

    // ── State ────────────────────────────────────────────────────────────

    BSTSConfig config_;
    bool       fitted_ = false;
    BSTSOutput last_output_;

    mutable std::mutex mutex_;
};

} // namespace statistics
} // namespace trade_ngin
