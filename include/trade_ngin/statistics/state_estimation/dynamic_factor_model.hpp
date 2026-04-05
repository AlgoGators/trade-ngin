#pragma once

#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <limits>
#include <cmath>

namespace trade_ngin {

class PostgresDatabase;  // forward declaration

namespace statistics {

struct MacroPanel;  // forward declaration

// ============================================================================
// DFMConfig
// ============================================================================

struct DFMConfig : public ConfigBase {

    int    num_factors       = 3;
    int    factor_ar_order   = 1;    // only AR(1) implemented
    int    max_em_iterations = 200;
    double em_tol            = 1e-6;
    bool   standardise_data  = true;

    // Human-readable labels for each factor.
    // Must be length == num_factors or empty (auto-labels f0, f1, ...).
    std::vector<std::string> factor_labels = {"macro_level", "real_activity", "commodity_inflation"};

    nlohmann::json to_json() const override {
        return {
            {"num_factors",       num_factors},
            {"factor_ar_order",   factor_ar_order},
            {"max_em_iterations", max_em_iterations},
            {"em_tol",            em_tol},
            {"standardise_data",  standardise_data},
            {"factor_labels",     factor_labels}
        };
    }

    void from_json(const nlohmann::json& j) override {
        if (j.contains("num_factors"))       num_factors       = j["num_factors"];
        if (j.contains("factor_ar_order"))   factor_ar_order   = j["factor_ar_order"];
        if (j.contains("max_em_iterations")) max_em_iterations = j["max_em_iterations"];
        if (j.contains("em_tol"))            em_tol            = j["em_tol"];
        if (j.contains("standardise_data"))  standardise_data  = j["standardise_data"];
        if (j.contains("factor_labels"))
            factor_labels = j["factor_labels"].get<std::vector<std::string>>();
    }
};

// ============================================================================
// DFMOutput
// B1 output — consumed by MS-DFM (B2) and MacroBelief downstream.
// ============================================================================

struct DFMOutput {

    // ---- Factor time series (T x K) -------------------------------------
    // factors[t][k]            = smoothed factor k at time t
    // factor_uncertainty[t][k] = posterior std dev (sqrt diag of P_t^smoother)
    std::vector<std::vector<double>> factors;
    std::vector<std::vector<double>> factor_uncertainty;

    // Named accessor: factor_series["growth"][t], etc.
    std::unordered_map<std::string, std::vector<double>> factor_series;

    // ---- Fitted parameters (needed by MS-DFM and online filter) ---------
    Eigen::MatrixXd lambda;   // N x K  observation loadings
    Eigen::MatrixXd A;        // K x K  factor AR transition
    Eigen::MatrixXd Q;        // K x K  factor noise covariance
    Eigen::VectorXd R_diag;   // N      idiosyncratic observation variances

    // ---- Standardisation params (needed for online update) --------------
    Eigen::VectorXd data_mean;  // length N
    Eigen::VectorXd data_std;   // length N  (ones if standardise_data=false)

    // ---- Fit diagnostics ------------------------------------------------
    double log_likelihood = 0.0;
    int    em_iterations  = 0;
    bool   converged      = false;
    double final_ll_delta = 0.0;

    ConvergenceInfo convergence_info;

    // ---- Metadata -------------------------------------------------------
    std::vector<std::string> series_names;
    int T = 0;
    int N = 0;
    int K = 0;
};

// ============================================================================
// DynamicFactorModel  (B1)
//
// State space model:
//   y_t = Lambda * f_t + e_t,   e_t ~ N(0, diag(R))
//   f_t = A * f_{t-1} + q_t,   q_t ~ N(0, Q)
//
// Fit via EM:
//   E-step : Kalman filter + RTS smoother
//   M-step : closed-form updates for Lambda, R, A, Q
//
// After fit(), call:
//   filter() — forward-only pass on new data (no lookahead, for live use)
//   update() — single-step online assimilation (maintains internal state)
// ============================================================================

class DynamicFactorModel {
public:

    explicit DynamicFactorModel(DFMConfig config = DFMConfig{});

    // Full EM fit on historical macro panel. Rows = time, cols = series.
    // NaN entries are handled gracefully (missing macro releases).
    Result<DFMOutput> fit(const Eigen::MatrixXd& data,
                          const std::vector<std::string>& names = {});

    // Convenience: load macro_data from PostgreSQL and fit in one call.
    // Queries all tables in the macro_data schema, joins on date.
    Result<DFMOutput> fit_from_db(PostgresDatabase& db,
                                  const std::string& start_date = "",
                                  const std::string& end_date   = "");

    // Forward-only Kalman filter using fitted parameters.
    // Call fit() first. Appropriate for backtesting / live inference.
    Result<DFMOutput> filter(const Eigen::MatrixXd& data) const;

    // Single-step online update. Maintains internal Kalman state between calls.
    Result<Eigen::VectorXd> update(const Eigen::VectorXd& y_t);

    bool             is_fitted()   const { return fitted_; }
    const DFMOutput& last_output() const { return last_output_; }
    const DFMConfig& config()      const { return config_; }

private:

    // Internal Kalman filter + RTS smoother result
    struct KalmanResult {
        std::vector<Eigen::VectorXd> filtered_means;
        std::vector<Eigen::MatrixXd> filtered_covs;
        std::vector<Eigen::VectorXd> smoothed_means;
        std::vector<Eigen::MatrixXd> smoothed_covs;
        std::vector<Eigen::MatrixXd> cross_covs;   // P_{t+1,t}^smoother
        double log_likelihood = 0.0;
    };

    KalmanResult kalman_filter_smoother(const Eigen::MatrixXd& Y) const;
    void         initialise_parameters(const Eigen::MatrixXd& Y_std);

    Eigen::MatrixXd standardise(const Eigen::MatrixXd& Y,
                                Eigen::VectorXd& out_mean,
                                Eigen::VectorXd& out_std) const;

    static Eigen::MatrixXd symmetrise(const Eigen::MatrixXd& M) {
        return 0.5 * (M + M.transpose());
    }

    // ---- State ----------------------------------------------------------
    DFMConfig config_;
    bool      fitted_ = false;
    DFMOutput last_output_;

    Eigen::MatrixXd lambda_;
    Eigen::MatrixXd A_;
    Eigen::MatrixXd Q_;
    Eigen::VectorXd R_diag_;

    Eigen::VectorXd data_mean_;
    Eigen::VectorXd data_std_;

    // Online Kalman state (updated by update())
    Eigen::VectorXd x_filt_;
    Eigen::MatrixXd P_filt_;

    mutable std::mutex mutex_;
};

} // namespace statistics
} // namespace trade_ngin
