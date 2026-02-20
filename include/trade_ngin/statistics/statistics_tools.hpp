#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include <Eigen/Dense>
#include <vector>
#include <memory>
#include <optional>
#include <unordered_map>
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

// ============================================================================
// Base Classes
// ============================================================================

/**
 * @brief Base class for all data transformation operations
 */
class DataTransformer {
public:
    virtual ~DataTransformer() = default;

    /**
     * @brief Fit the transformer to data
     * @param data Input data matrix (samples x features)
     * @return Result indicating success or failure
     */
    virtual Result<void> fit(const Eigen::MatrixXd& data) = 0;

    /**
     * @brief Transform data using fitted parameters
     * @param data Input data matrix
     * @return Transformed data matrix
     */
    virtual Result<Eigen::MatrixXd> transform(const Eigen::MatrixXd& data) const = 0;

    /**
     * @brief Fit and transform in one step
     * @param data Input data matrix
     * @return Transformed data matrix
     */
    virtual Result<Eigen::MatrixXd> fit_transform(const Eigen::MatrixXd& data) {
        auto fit_result = fit(data);
        if (fit_result.is_error()) {
            return make_error<Eigen::MatrixXd>(
                fit_result.error()->code(),
                fit_result.error()->what(),
                "DataTransformer"
            );
        }
        return transform(data);
    }

    /**
     * @brief Inverse transform (if applicable)
     * @param data Transformed data matrix
     * @return Original scale data matrix
     */
    virtual Result<Eigen::MatrixXd> inverse_transform(const Eigen::MatrixXd& data) const = 0;

    /**
     * @brief Check if transformer is fitted
     */
    virtual bool is_fitted() const = 0;
};

/**
 * @brief Base class for all statistical tests
 */
class StatisticalTest {
public:
    virtual ~StatisticalTest() = default;

    /**
     * @brief Perform the statistical test
     * @param data Input time series data
     * @return Test result with statistics and interpretation
     */
    virtual Result<TestResult> test(const std::vector<double>& data) const = 0;

    /**
     * @brief Get test name
     */
    virtual std::string get_name() const = 0;
};

/**
 * @brief Base class for volatility models
 */
class VolatilityModel {
public:
    virtual ~VolatilityModel() = default;

    /**
     * @brief Fit the volatility model to returns data
     * @param returns Vector of return series
     * @return Result indicating success or failure
     */
    virtual Result<void> fit(const std::vector<double>& returns) = 0;

    /**
     * @brief Forecast volatility for next n periods
     * @param n_periods Number of periods ahead to forecast
     * @return Vector of forecasted volatilities
     */
    virtual Result<std::vector<double>> forecast(int n_periods = 1) const = 0;

    /**
     * @brief Get current conditional volatility
     */
    virtual Result<double> get_current_volatility() const = 0;

    /**
     * @brief Update model with new observation
     * @param new_return New return observation
     */
    virtual Result<void> update(double new_return) = 0;

    /**
     * @brief Check if model is fitted
     */
    virtual bool is_fitted() const = 0;
};

/**
 * @brief Base class for state estimation models
 */
class StateEstimator {
public:
    virtual ~StateEstimator() = default;

    /**
     * @brief Initialize the state estimator
     * @param initial_state Initial state vector
     * @return Result indicating success or failure
     */
    virtual Result<void> initialize(const Eigen::VectorXd& initial_state) = 0;

    /**
     * @brief Predict next state
     * @return Predicted state vector
     */
    virtual Result<Eigen::VectorXd> predict() = 0;

    /**
     * @brief Update state with new observation
     * @param observation New observation vector
     * @return Updated state vector
     */
    virtual Result<Eigen::VectorXd> update(const Eigen::VectorXd& observation) = 0;

    /**
     * @brief Get current state estimate
     */
    virtual Result<Eigen::VectorXd> get_state() const = 0;

    /**
     * @brief Check if estimator is initialized
     */
    virtual bool is_initialized() const = 0;
};

// ============================================================================
// Data Transformers
// ============================================================================

/**
 * @brief Data normalization transformer
 */
class Normalizer : public DataTransformer {
public:
    explicit Normalizer(NormalizationConfig config);

    Result<void> fit(const Eigen::MatrixXd& data) override;
    Result<Eigen::MatrixXd> transform(const Eigen::MatrixXd& data) const override;
    Result<Eigen::MatrixXd> inverse_transform(const Eigen::MatrixXd& data) const override;
    bool is_fitted() const override { return fitted_; }

    const Eigen::VectorXd& get_mean() const { return mean_; }
    const Eigen::VectorXd& get_std() const { return std_; }

private:
    NormalizationConfig config_;
    Eigen::VectorXd mean_;
    Eigen::VectorXd std_;
    Eigen::VectorXd min_;
    Eigen::VectorXd max_;
    Eigen::VectorXd median_;
    Eigen::VectorXd iqr_;
    bool fitted_{false};
    mutable std::mutex mutex_;
};

/**
 * @brief Principal Component Analysis transformer
 */
class PCA : public DataTransformer {
public:
    explicit PCA(PCAConfig config);

    Result<void> fit(const Eigen::MatrixXd& data) override;
    Result<Eigen::MatrixXd> transform(const Eigen::MatrixXd& data) const override;
    Result<Eigen::MatrixXd> inverse_transform(const Eigen::MatrixXd& data) const override;
    bool is_fitted() const override { return fitted_; }

    const Eigen::VectorXd& get_explained_variance() const { return explained_variance_; }
    const Eigen::VectorXd& get_explained_variance_ratio() const { return explained_variance_ratio_; }
    const Eigen::MatrixXd& get_components() const { return components_; }
    int get_n_components() const { return n_components_; }

private:
    PCAConfig config_;
    Eigen::MatrixXd components_;        // Principal components (eigenvectors)
    Eigen::VectorXd explained_variance_;
    Eigen::VectorXd explained_variance_ratio_;
    Eigen::VectorXd mean_;
    int n_components_{0};
    bool fitted_{false};
    mutable std::mutex mutex_;
};

// ============================================================================
// Statistical Tests
// ============================================================================

/**
 * @brief Augmented Dickey-Fuller test for stationarity
 */
class ADFTest : public StatisticalTest {
public:
    explicit ADFTest(ADFTestConfig config);

    Result<TestResult> test(const std::vector<double>& data) const override;
    std::string get_name() const override { return "Augmented Dickey-Fuller Test"; }

private:
    ADFTestConfig config_;

    // Helper methods
    int select_lag_order(const std::vector<double>& data) const;
    double calculate_critical_value(int n_obs, double significance) const;
};

/**
 * @brief KPSS test for stationarity
 */
class KPSSTest : public StatisticalTest {
public:
    explicit KPSSTest(KPSSTestConfig config);

    Result<TestResult> test(const std::vector<double>& data) const override;
    std::string get_name() const override { return "KPSS Test"; }

private:
    KPSSTestConfig config_;

    int select_lag_order(int n_obs) const;
    double calculate_critical_value(double significance, bool has_trend) const;
};

/**
 * @brief Johansen cointegration test
 */
class JohansenTest {
public:
    explicit JohansenTest(JohansenTestConfig config);

    /**
     * @brief Test for cointegration among multiple time series
     * @param data Matrix where each column is a time series
     * @return Cointegration test result
     */
    Result<CointegrationResult> test(const Eigen::MatrixXd& data) const;

private:
    JohansenTestConfig config_;

    std::vector<double> get_critical_values(int n_series, int rank) const;
};

/**
 * @brief Engle-Granger two-step cointegration test
 */
class EngleGrangerTest {
public:
    explicit EngleGrangerTest(EngleGrangerConfig config);

    /**
     * @brief Test for pairwise cointegration
     * @param y Dependent variable time series
     * @param x Independent variable time series
     * @return Test result
     */
    Result<TestResult> test(const std::vector<double>& y,
                           const std::vector<double>& x) const;

private:
    EngleGrangerConfig config_;
    ADFTest adf_test_;

    // Helper: perform OLS regression
    std::pair<double, std::vector<double>> ols_regression(
        const std::vector<double>& y,
        const std::vector<double>& x) const;
};

// ============================================================================
// Volatility Models
// ============================================================================

/**
 * @brief GARCH(p,q) volatility model
 */
class GARCH : public VolatilityModel {
public:
    explicit GARCH(GARCHConfig config);

    Result<void> fit(const std::vector<double>& returns) override;
    Result<std::vector<double>> forecast(int n_periods = 1) const override;
    Result<double> get_current_volatility() const override;
    Result<void> update(double new_return) override;
    bool is_fitted() const override { return fitted_; }

    double get_omega() const { return omega_; }
    double get_alpha() const { return alpha_; }
    double get_beta() const { return beta_; }

private:
    GARCHConfig config_;
    double omega_;      // Constant term
    double alpha_;      // ARCH coefficient
    double beta_;       // GARCH coefficient
    std::vector<double> residuals_;
    std::vector<double> conditional_variances_;
    double current_volatility_{0.0};
    bool fitted_{false};
    mutable std::mutex mutex_;

    // Estimate parameters using maximum likelihood
    Result<void> estimate_parameters(const std::vector<double>& returns);
    double log_likelihood(const std::vector<double>& returns,
                         double omega, double alpha, double beta) const;
};

// ============================================================================
// State Estimators
// ============================================================================

/**
 * @brief Kalman Filter for state estimation
 */
class KalmanFilter : public StateEstimator {
public:
    explicit KalmanFilter(KalmanFilterConfig config);

    Result<void> initialize(const Eigen::VectorXd& initial_state) override;
    Result<Eigen::VectorXd> predict() override;
    Result<Eigen::VectorXd> update(const Eigen::VectorXd& observation) override;
    Result<Eigen::VectorXd> get_state() const override;
    bool is_initialized() const override { return initialized_; }

    /**
     * @brief Set transition matrix (state dynamics)
     */
    void set_transition_matrix(const Eigen::MatrixXd& F) { F_ = F; }

    /**
     * @brief Set observation matrix
     */
    void set_observation_matrix(const Eigen::MatrixXd& H) { H_ = H; }

    /**
     * @brief Set process noise covariance
     */
    void set_process_noise(const Eigen::MatrixXd& Q) { Q_ = Q; }

    /**
     * @brief Set measurement noise covariance
     */
    void set_measurement_noise(const Eigen::MatrixXd& R) { R_ = R; }

    const Eigen::MatrixXd& get_state_covariance() const { return P_; }

private:
    KalmanFilterConfig config_;

    // State variables
    Eigen::VectorXd x_;         // State estimate
    Eigen::MatrixXd P_;         // State covariance

    // Model matrices
    Eigen::MatrixXd F_;         // State transition matrix
    Eigen::MatrixXd H_;         // Observation matrix
    Eigen::MatrixXd Q_;         // Process noise covariance
    Eigen::MatrixXd R_;         // Measurement noise covariance

    bool initialized_{false};
    mutable std::mutex mutex_;
};

/**
 * @brief Hidden Markov Model for regime detection
 */
class HMM : public StateEstimator {
public:
    explicit HMM(HMMConfig config);

    Result<void> initialize(const Eigen::VectorXd& initial_state) override;
    Result<Eigen::VectorXd> predict() override;
    Result<Eigen::VectorXd> update(const Eigen::VectorXd& observation) override;
    Result<Eigen::VectorXd> get_state() const override;
    bool is_initialized() const override { return initialized_; }

    /**
     * @brief Fit HMM to observation sequence using Baum-Welch algorithm
     * @param observations Matrix of observations (time steps x features)
     * @return Result indicating success or failure
     */
    Result<void> fit(const Eigen::MatrixXd& observations);

    /**
     * @brief Decode most likely state sequence (Viterbi algorithm)
     * @param observations Matrix of observations
     * @return Vector of most likely states
     */
    Result<std::vector<int>> decode(const Eigen::MatrixXd& observations) const;

    /**
     * @brief Get current state probabilities
     */
    const Eigen::VectorXd& get_state_probabilities() const { return state_probs_; }

    /**
     * @brief Get transition matrix
     */
    const Eigen::MatrixXd& get_transition_matrix() const { return transition_matrix_; }

    /**
     * @brief Get emission parameters (Gaussian means and covariances)
     */
    const std::vector<Eigen::VectorXd>& get_means() const { return means_; }
    const std::vector<Eigen::MatrixXd>& get_covariances() const { return covariances_; }

private:
    HMMConfig config_;

    // HMM parameters
    Eigen::VectorXd state_probs_;           // Current state probabilities
    Eigen::VectorXd initial_probs_;         // Initial state distribution
    Eigen::MatrixXd transition_matrix_;     // State transition probabilities
    std::vector<Eigen::VectorXd> means_;    // Emission means for each state
    std::vector<Eigen::MatrixXd> covariances_; // Emission covariances

    bool initialized_{false};
    mutable std::mutex mutex_;

    // Helper methods for Baum-Welch
    void initialize_parameters(const Eigen::MatrixXd& observations);
    double forward_backward(const Eigen::MatrixXd& observations,
                           Eigen::MatrixXd& gamma,
                           Eigen::MatrixXd& xi) const;
    double log_emission_probability(const Eigen::VectorXd& obs, int state) const;
    double emission_probability(const Eigen::VectorXd& obs, int state) const;
};

} // namespace statistics
} // namespace trade_ngin
