#pragma once

#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"
#include "trade_ngin/statistics/state_estimation/dynamic_factor_model.hpp"

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <mutex>

namespace trade_ngin {

class PostgresDatabase;

namespace statistics {

// ============================================================================
// MSDFMConfig
// ============================================================================

struct MSDFMConfig : public ConfigBase {

    int    n_regimes              = 3;
    int    max_em_iterations      = 200;
    double em_tol                 = 1e-5;
    double transition_persistence = 0.95;

    std::vector<std::string> regime_labels = {"expansion", "slowdown", "stress"};

    // DFM config used by fit_from_db pipeline
    DFMConfig dfm_config;

    nlohmann::json to_json() const override {
        return {
            {"n_regimes",              n_regimes},
            {"max_em_iterations",      max_em_iterations},
            {"em_tol",                 em_tol},
            {"transition_persistence", transition_persistence},
            {"regime_labels",          regime_labels},
            {"dfm_config",             dfm_config.to_json()}
        };
    }

    void from_json(const nlohmann::json& j) override {
        if (j.contains("n_regimes"))              n_regimes              = j["n_regimes"];
        if (j.contains("max_em_iterations"))      max_em_iterations      = j["max_em_iterations"];
        if (j.contains("em_tol"))                 em_tol                 = j["em_tol"];
        if (j.contains("transition_persistence")) transition_persistence = j["transition_persistence"];
        if (j.contains("regime_labels"))
            regime_labels = j["regime_labels"].get<std::vector<std::string>>();
        if (j.contains("dfm_config"))
            dfm_config.from_json(j["dfm_config"]);
    }
};

// ============================================================================
// RegimeSignature — summary of one regime's fitted parameters
// ============================================================================

struct RegimeSignature {
    Eigen::MatrixXd A;             // K x K  factor transition in this regime
    Eigen::MatrixXd Q;             // K x K  factor noise covariance
    Eigen::VectorXd mean_factors;  // K      mean factor level when in regime
    double mean_volatility = 0.0;  // trace(Q) / K
};

// ============================================================================
// MSDFMOutput
// B2 output — macro regime probabilities for downstream consumption.
// ============================================================================

struct MSDFMOutput {

    // ---- Regime probabilities (T x J) -----------------------------------
    Eigen::MatrixXd filtered_probs;     // P(Z_t=j | y_{1:t})
    Eigen::MatrixXd smoothed_probs;     // P(Z_t=j | y_{1:T})
    std::vector<int> decoded_regimes;   // argmax of smoothed per t

    // ---- Regime-dependent parameters ------------------------------------
    std::vector<RegimeSignature> regime_signatures;  // length J
    Eigen::MatrixXd transition_matrix;               // J x J
    Eigen::VectorXd ergodic_probs;                   // stationary distribution

    // ---- Fit diagnostics ------------------------------------------------
    double log_likelihood = 0.0;
    ConvergenceInfo convergence_info;

    // ---- Metadata -------------------------------------------------------
    std::vector<std::string> regime_labels;
    int T = 0;
    int K = 0;
    int J = 0;
};

// ============================================================================
// MarkovSwitchingDFM  (B2)
//
// Two-stage estimation:
//   1. DFM (B1) extracts factors f_t from macro panel
//   2. MS-DFM fits regime-switching VAR(1) on f_t:
//        f_t | Z_t=j ~ N(A_j * f_{t-1}, Q_j)
//        P(Z_t=j | Z_{t-1}=i) = P_ij
//
// Estimation: EM with Hamilton filter + Kim smoother
// ============================================================================

class MarkovSwitchingDFM {
public:

    explicit MarkovSwitchingDFM(MSDFMConfig config = MSDFMConfig{});

    // Fit on pre-estimated factors (T x K matrix).
    Result<MSDFMOutput> fit(const Eigen::MatrixXd& factors);

    // Convenience: extract factors from a fitted DFMOutput.
    Result<MSDFMOutput> fit(const DFMOutput& dfm_output);

    // Full pipeline: load macro data → fit DFM → fit MS-DFM.
    Result<MSDFMOutput> fit_from_db(PostgresDatabase& db,
                                     const std::string& start_date = "",
                                     const std::string& end_date   = "");

    // Online: given new factor vector f_t, update regime probabilities.
    Result<Eigen::VectorXd> update(const Eigen::VectorXd& f_t);

    bool               is_fitted()    const { return fitted_; }
    const MSDFMOutput& last_output()  const { return last_output_; }
    const MSDFMConfig& config()       const { return config_; }

private:

    void initialize_parameters(const Eigen::MatrixXd& factors);

    // E-step: Hamilton filter (forward pass)
    // Returns log-likelihood, populates filtered_probs (T x J)
    // Also stores pred_probs_ (T x J) for use in smoother and M-step
    double hamilton_filter_step(const Eigen::MatrixXd& factors,
                                Eigen::MatrixXd& filtered_probs);

    // E-step: Kim smoother (backward pass)
    void kim_smoother_step(const Eigen::MatrixXd& filtered_probs,
                           Eigen::MatrixXd& smoothed_probs);

    // M-step: update A_j, Q_j, P from sufficient statistics
    void m_step(const Eigen::MatrixXd& factors,
                const Eigen::MatrixXd& smoothed_probs,
                const Eigen::MatrixXd& filtered_probs);

    // Multivariate Gaussian log-density using Cholesky
    static double mvn_log_prob(const Eigen::VectorXd& x,
                               const Eigen::VectorXd& mean,
                               const Eigen::MatrixXd& cov);

    // Compute stationary distribution of transition matrix
    static Eigen::VectorXd ergodic_distribution(const Eigen::MatrixXd& P);

    // Sort regimes by trace(Q_j) ascending: 0=calm, J-1=stress
    void order_regimes_by_volatility(MSDFMOutput& out);

    static Eigen::MatrixXd symmetrise(const Eigen::MatrixXd& M) {
        return 0.5 * (M + M.transpose());
    }

    // ---- State ----------------------------------------------------------
    MSDFMConfig config_;
    bool        fitted_ = false;
    MSDFMOutput last_output_;

    // Regime-dependent parameters
    std::vector<Eigen::MatrixXd> A_;   // J copies, each K x K
    std::vector<Eigen::MatrixXd> Q_;   // J copies, each K x K
    Eigen::MatrixXd P_;                // J x J  transition matrix
    Eigen::VectorXd pi0_;              // J      initial regime probs

    // Stored during hamilton_filter_step for use in smoother & M-step
    Eigen::MatrixXd pred_probs_;       // T x J

    int K_ = 0;
    int J_ = 0;

    // Online state
    Eigen::VectorXd online_regime_probs_;
    Eigen::VectorXd f_prev_;

    mutable std::mutex mutex_;
};

} // namespace statistics
} // namespace trade_ngin
