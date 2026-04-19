#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include <Eigen/Dense>
#include <vector>
#include <string>
#include <unordered_map>
#include <optional>
#include <mutex>
#include <sstream>

namespace trade_ngin {
namespace statistics {

// ============================================================================
// Configuration Structures
// ============================================================================

/**
 * @brief Configuration for normalization transformers
 */
struct NormalizationConfig {
    enum class Method {
        Z_SCORE,        // Standardize to mean=0, std=1
        MIN_MAX,        // Scale to [0, 1] range
        ROBUST          // Use median and IQR for robustness to outliers
    };

    Method method{Method::Z_SCORE};
    bool fit_on_construct{false};  // Whether to fit immediately on construction
};

/**
 * @brief Configuration for PCA
 */
struct PCAConfig {
    int n_components{-1};           // Number of components (-1 = all)
    double variance_threshold{0.95}; // Cumulative variance to retain
    bool whiten{false};             // Apply whitening transformation
    bool fit_on_construct{false};
};

/**
 * @brief Configuration for ADF test
 */
struct ADFTestConfig {
    enum class RegressionType {
        CONSTANT,           // Include constant term
        CONSTANT_TREND,     // Include constant and linear trend
        NO_CONSTANT         // No constant or trend
    };

    RegressionType regression{RegressionType::CONSTANT};
    int max_lags{-1};       // Maximum lags to test (-1 = auto)
    double significance_level{0.05};
};

/**
 * @brief Configuration for KPSS test
 */
struct KPSSTestConfig {
    enum class RegressionType {
        CONSTANT,           // Level stationarity
        CONSTANT_TREND      // Trend stationarity
    };

    RegressionType regression{RegressionType::CONSTANT};
    int max_lags{-1};       // Maximum lags for variance estimation (-1 = auto)
    double significance_level{0.05};
};

/**
 * @brief Configuration for Johansen cointegration test
 */
struct JohansenTestConfig {
    enum class TestType {
        TRACE,              // Trace statistic
        MAX_EIGENVALUE      // Maximum eigenvalue statistic
    };

    TestType test_type{TestType::TRACE};
    int max_lags{1};
    double significance_level{0.05};
};

/**
 * @brief Configuration for Engle-Granger test
 */
struct EngleGrangerConfig {
    int max_lags{-1};
    double significance_level{0.05};
};

/**
 * @brief Configuration for GARCH model
 */
struct GARCHConfig {
    int p{1};               // GARCH order (lag order for variance)
    int q{1};               // ARCH order (lag order for squared residuals)
    double omega{0.0001};   // Constant term (initial)
    double alpha{0.1};      // ARCH coefficient (initial)
    double beta{0.85};      // GARCH coefficient (initial)
    int max_iterations{1000};
    double tolerance{1e-6};
};

/**
 * @brief Configuration for Kalman Filter
 */
struct KalmanFilterConfig {
    int state_dim{1};       // Dimensionality of state vector
    int obs_dim{1};         // Dimensionality of observation vector
    double process_noise{0.01};     // Process noise covariance
    double measurement_noise{0.1};  // Measurement noise covariance
    bool adaptive{false};   // Use adaptive noise estimation
};

/**
 * @brief Configuration for HMM
 */
struct HMMConfig {
    int n_states{2};        // Number of hidden states
    int max_iterations{100};
    double tolerance{1e-4};
    bool init_random{true}; // Random initialization vs uniform
};

// ============================================================================
// Test Result Structures
// ============================================================================

/**
 * @brief Result of a statistical test
 */
struct TestResult {
    double statistic;
    double p_value;
    double critical_value;
    bool reject_null;       // True if we reject the null hypothesis
    std::string interpretation;
    std::unordered_map<std::string, double> additional_stats;
};

/**
 * @brief Result of cointegration test
 */
struct CointegrationResult {
    std::vector<double> eigenvalues;
    std::vector<double> trace_statistics;
    std::vector<double> critical_values;
    int cointegration_rank;
    Eigen::MatrixXd cointegrating_vectors;
    bool is_cointegrated;
};

// ============================================================================
// Regression Configuration & Result Structures
// ============================================================================

struct OLSRegressionConfig {
    bool include_intercept{true};
};

struct OLSResult {
    Eigen::VectorXd coefficients;
    double r_squared{0.0};
    double adj_r_squared{0.0};
    Eigen::VectorXd residuals;
    Eigen::VectorXd standard_errors;
    Eigen::VectorXd t_statistics;
    Eigen::VectorXd p_values;
};

struct RidgeRegressionConfig {
    bool include_intercept{true};
    double alpha{1.0};
    int cv_folds{0};                    // 0 = no CV, >0 = k-fold CV for alpha selection
    std::vector<double> alpha_candidates; // Alphas to try during CV
};

struct RidgeResult {
    Eigen::VectorXd coefficients;
    double intercept{0.0};
    double r_squared{0.0};
    double best_alpha{0.0};
};

struct LassoRegressionConfig {
    bool include_intercept{true};
    double alpha{1.0};
    int max_iterations{1000};
    double tolerance{1e-6};
    int cv_folds{0};
    std::vector<double> alpha_candidates;
};

struct LassoResult {
    Eigen::VectorXd coefficients;
    double intercept{0.0};
    double r_squared{0.0};
    double best_alpha{0.0};
    std::vector<int> selected_features;
    int n_nonzero{0};
};

// ============================================================================
// EGARCH Configuration
// ============================================================================

struct EGARCHConfig {
    double omega{-0.1};        // Constant in log-variance equation
    double alpha{0.1};         // Magnitude effect coefficient
    double gamma{-0.05};       // Leverage/asymmetry coefficient
    double beta{0.95};         // Persistence coefficient
    int max_iterations{1000};
    double tolerance{1e-6};
};

// ============================================================================
// Hurst Exponent Configuration & Result
// ============================================================================

struct HurstExponentConfig {
    enum class Method {
        RS_ANALYSIS,    // Rescaled range analysis
        DFA,            // Detrended fluctuation analysis (default)
        PERIODOGRAM     // Log-periodogram regression
    };

    Method method{Method::DFA};
    int min_window{10};
    int max_window{-1};         // -1 = auto (N/4)
    int n_windows{20};          // Number of log-spaced window sizes
};

struct HurstResult {
    double hurst_exponent{0.0};
    double r_squared{0.0};      // Goodness of fit of log-log regression
    std::string interpretation;  // "mean-reverting", "random walk", or "trending"
};

// ============================================================================
// Markov Switching Configuration & Result
// ============================================================================

struct MarkovSwitchingConfig {
    int n_states{2};
    int max_iterations{100};
    double tolerance{1e-4};
};

struct MarkovSwitchingResult {
    Eigen::VectorXd state_means;
    Eigen::VectorXd state_variances;
    Eigen::MatrixXd transition_matrix;
    Eigen::MatrixXd smoothed_probabilities;
    std::vector<int> decoded_states;
    double log_likelihood{0.0};
    bool converged{false};
    int n_iterations{0};
};

// ============================================================================
// Phillips-Perron Test Configuration
// ============================================================================

struct PhillipsPerronConfig {
    enum class RegressionType {
        CONSTANT,
        CONSTANT_TREND,
        NO_CONSTANT
    };

    enum class KernelType {
        BARTLETT,
        PARZEN,
        QUADRATIC_SPECTRAL
    };

    RegressionType regression{RegressionType::CONSTANT};
    int bandwidth{-1};                  // -1 = auto (Newey-West)
    KernelType kernel{KernelType::BARTLETT};
    double significance_level{0.05};
};

// ============================================================================
// Variance Ratio Test Configuration
// ============================================================================

struct VarianceRatioConfig {
    std::vector<int> holding_periods{2, 5, 10};
    double significance_level{0.05};
    bool heteroskedasticity_robust{true};
};

struct VarianceRatioResult {
    std::vector<double> vr_statistics;      // VR(q) for each holding period
    std::vector<double> z_statistics;       // Z-stat for each holding period
    std::vector<double> p_values;
    std::vector<int> holding_periods;
    bool reject_random_walk{false};         // True if any period rejects
    std::string interpretation;
};

// ============================================================================
// GJR-GARCH Configuration
// ============================================================================

struct GJRGARCHConfig {
    double omega{0.0001};
    double alpha{0.05};
    double gamma{0.1};                      // Asymmetry coefficient for negative shocks
    double beta{0.85};
    int max_iterations{1000};
    double tolerance{1e-6};
};

// ============================================================================
// DCC-GARCH Configuration & Result
// ============================================================================

struct DCCGARCHConfig {
    double dcc_a{0.05};                     // DCC parameter a
    double dcc_b{0.93};                     // DCC parameter b
    GARCHConfig univariate_config;          // Config for univariate GARCH fits
    int max_iterations{500};
    double tolerance{1e-6};
};

struct DCCGARCHResult {
    double dcc_a{0.0};
    double dcc_b{0.0};
    Eigen::MatrixXd unconditional_correlation;  // Q̄
    std::vector<Eigen::MatrixXd> conditional_correlations;  // R_t for each t
    std::vector<Eigen::VectorXd> conditional_volatilities;  // σ_t for each t
};

// ============================================================================
// Extended Kalman Filter Configuration
// ============================================================================

struct ExtendedKalmanFilterConfig {
    int state_dim{1};
    int obs_dim{1};
};

// ============================================================================
// Convergence Diagnostics
// ============================================================================

struct ConvergenceInfo {
    int iterations{0};
    double final_tolerance{0.0};
    bool converged{false};
    std::string termination_reason;        // "tolerance", "max_iterations", "error"
    std::vector<double> objective_history;  // log-likelihood or loss per iteration

    bool is_successful() const { return converged && termination_reason == "tolerance"; }

    std::string summary() const {
        std::ostringstream oss;
        if (converged) {
            oss << "Converged in " << iterations << " iterations (tol=" << final_tolerance << ")";
        } else {
            oss << "Did not converge after " << iterations << " iterations";
            if (!termination_reason.empty()) {
                oss << " (" << termination_reason << ")";
            }
        }
        return oss.str();
    }
};

// ============================================================================
// Preprocessing Configurations
// ============================================================================

struct OutlierHandlerConfig {
    enum class Method {
        WINSORIZE,
        TRIM,
        MAD_FILTER
    };

    Method method{Method::WINSORIZE};
    double lower_percentile{0.05};
    double upper_percentile{0.95};
    double mad_threshold{3.0};  // For MAD_FILTER: median ± threshold * MAD
};

struct MissingDataHandlerConfig {
    enum class Strategy {
        ERROR,
        DROP,
        FORWARD_FILL,
        INTERPOLATE,
        MEAN_FILL
    };

    Strategy strategy{Strategy::ERROR};
};

} // namespace statistics
} // namespace trade_ngin
