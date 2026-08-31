#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include <Eigen/Dense>
#include <vector>
#include <string>
#include <unordered_map>
#include <optional>
#include <mutex>

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

} // namespace statistics
} // namespace trade_ngin
