#include "trade_ngin/statistics/statistics_tools.hpp"
#include "trade_ngin/statistics/critical_values.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <limits>

// Define M_PI if not available (Windows)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace trade_ngin {
namespace statistics {

// ============================================================================
// Helper Functions
// ============================================================================

namespace {

// Calculate mean of a vector
double calculate_mean(const std::vector<double>& data) {
    if (data.empty()) return 0.0;
    return std::accumulate(data.begin(), data.end(), 0.0) / data.size();
}

// Calculate variance
double calculate_variance(const std::vector<double>& data, double mean) {
    if (data.size() <= 1) return 0.0;
    double sum_sq = 0.0;
    for (double val : data) {
        double diff = val - mean;
        sum_sq += diff * diff;
    }
    return sum_sq / (data.size() - 1);
}

// Calculate standard deviation
double calculate_std(const std::vector<double>& data, double mean) {
    return std::sqrt(calculate_variance(data, mean));
}

// Calculate median
double calculate_median(std::vector<double> data) {
    if (data.empty()) return 0.0;
    size_t n = data.size();
    std::nth_element(data.begin(), data.begin() + n / 2, data.end());
    if (n % 2 == 0) {
        double median1 = data[n / 2];
        std::nth_element(data.begin(), data.begin() + n / 2 - 1, data.end());
        return (median1 + data[n / 2 - 1]) / 2.0;
    }
    return data[n / 2];
}

// Calculate IQR (interquartile range)
double calculate_iqr(std::vector<double> data) {
    if (data.size() < 4) return 0.0;
    std::sort(data.begin(), data.end());
    size_t n = data.size();
    size_t q1_idx = n / 4;
    size_t q3_idx = 3 * n / 4;
    return data[q3_idx] - data[q1_idx];
}

// Autocorrelation function
std::vector<double> autocorrelation(const std::vector<double>& data, int max_lag) {
    size_t n = data.size();
    double mean = calculate_mean(data);
    double variance = calculate_variance(data, mean);

    std::vector<double> acf(max_lag + 1);
    for (int lag = 0; lag <= max_lag; ++lag) {
        double sum = 0.0;
        for (size_t t = lag; t < n; ++t) {
            sum += (data[t] - mean) * (data[t - lag] - mean);
        }
        acf[lag] = sum / (n * variance);
    }
    return acf;
}

// Difference a time series
std::vector<double> difference(const std::vector<double>& data, int order = 1) {
    std::vector<double> result = data;
    for (int i = 0; i < order; ++i) {
        std::vector<double> diff;
        for (size_t j = 1; j < result.size(); ++j) {
            diff.push_back(result[j] - result[j - 1]);
        }
        result = diff;
    }
    return result;
}

} // anonymous namespace

// ============================================================================
// Normalizer Implementation
// ============================================================================

Normalizer::Normalizer(NormalizationConfig config)
    : config_(config) {}

Result<void> Normalizer::fit(const Eigen::MatrixXd& data) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (data.rows() == 0 || data.cols() == 0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Input data is empty",
            "Normalizer"
        );
    }

    switch (config_.method) {
        case NormalizationConfig::Method::Z_SCORE: {
            mean_ = data.colwise().mean();
            // Compute std column by column
            std_ = Eigen::VectorXd(data.cols());
            for (int i = 0; i < data.cols(); ++i) {
                Eigen::VectorXd centered_col = data.col(i).array() - mean_(i);
                std_(i) = std::sqrt(centered_col.squaredNorm() / (data.rows() - 1));
                // Prevent division by zero
                if (std_(i) < 1e-10) std_(i) = 1.0;
            }
            break;
        }

        case NormalizationConfig::Method::MIN_MAX:
            min_ = data.colwise().minCoeff();
            max_ = data.colwise().maxCoeff();
            // Prevent division by zero
            for (int i = 0; i < min_.size(); ++i) {
                if (std::abs(max_(i) - min_(i)) < 1e-10) {
                    max_(i) = min_(i) + 1.0;
                }
            }
            break;

        case NormalizationConfig::Method::ROBUST: {
            int n_features = data.cols();
            median_.resize(n_features);
            iqr_.resize(n_features);

            for (int i = 0; i < n_features; ++i) {
                std::vector<double> col_data(data.rows());
                for (int j = 0; j < data.rows(); ++j) {
                    col_data[j] = data(j, i);
                }
                median_(i) = calculate_median(col_data);
                iqr_(i) = calculate_iqr(col_data);
                if (iqr_(i) < 1e-10) iqr_(i) = 1.0;
            }
            break;
        }
    }

    fitted_ = true;
    return Result<void>();
}

Result<Eigen::MatrixXd> Normalizer::transform(const Eigen::MatrixXd& data) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::NOT_INITIALIZED,
            "Normalizer has not been fitted",
            "Normalizer"
        );
    }

    if (data.cols() != mean_.size() && config_.method == NormalizationConfig::Method::Z_SCORE) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::INVALID_ARGUMENT,
            "Data dimensionality does not match fitted model",
            "Normalizer"
        );
    }

    Eigen::MatrixXd result(data.rows(), data.cols());

    switch (config_.method) {
        case NormalizationConfig::Method::Z_SCORE:
            for (int i = 0; i < data.cols(); ++i) {
                result.col(i) = (data.col(i).array() - mean_(i)) / std_(i);
            }
            break;

        case NormalizationConfig::Method::MIN_MAX:
            for (int i = 0; i < data.cols(); ++i) {
                result.col(i) = (data.col(i).array() - min_(i)) / (max_(i) - min_(i));
            }
            break;

        case NormalizationConfig::Method::ROBUST:
            for (int i = 0; i < data.cols(); ++i) {
                result.col(i) = (data.col(i).array() - median_(i)) / iqr_(i);
            }
            break;
    }

    return Result<Eigen::MatrixXd>(std::move(result));
}

Result<Eigen::MatrixXd> Normalizer::inverse_transform(const Eigen::MatrixXd& data) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::NOT_INITIALIZED,
            "Normalizer has not been fitted",
            "Normalizer"
        );
    }

    Eigen::MatrixXd result(data.rows(), data.cols());

    switch (config_.method) {
        case NormalizationConfig::Method::Z_SCORE:
            for (int i = 0; i < data.cols(); ++i) {
                result.col(i) = data.col(i).array() * std_(i) + mean_(i);
            }
            break;

        case NormalizationConfig::Method::MIN_MAX:
            for (int i = 0; i < data.cols(); ++i) {
                result.col(i) = data.col(i).array() * (max_(i) - min_(i)) + min_(i);
            }
            break;

        case NormalizationConfig::Method::ROBUST:
            for (int i = 0; i < data.cols(); ++i) {
                result.col(i) = data.col(i).array() * iqr_(i) + median_(i);
            }
            break;
    }

    return Result<Eigen::MatrixXd>(std::move(result));
}

// ============================================================================
// PCA Implementation
// ============================================================================

PCA::PCA(PCAConfig config)
    : config_(config) {}

Result<void> PCA::fit(const Eigen::MatrixXd& data) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (data.rows() == 0 || data.cols() == 0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Input data is empty",
            "PCA"
        );
    }

    // Center the data
    mean_ = data.colwise().mean();
    Eigen::MatrixXd centered(data.rows(), data.cols());
    for (int i = 0; i < data.cols(); ++i) {
        centered.col(i) = data.col(i).array() - mean_(i);
    }

    // Compute covariance matrix
    Eigen::MatrixXd cov = (centered.transpose() * centered) / (data.rows() - 1);

    // Compute eigendecomposition
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(cov);
    if (solver.info() != Eigen::Success) {
        return make_error<void>(
            ErrorCode::INVALID_DATA,
            "Eigenvalue decomposition failed",
            "PCA"
        );
    }

    // Eigenvalues are in ascending order, we want descending
    Eigen::VectorXd eigenvalues = solver.eigenvalues().reverse();
    Eigen::MatrixXd eigenvectors = solver.eigenvectors().colwise().reverse();

    // Determine number of components
    if (config_.n_components > 0) {
        n_components_ = std::min(config_.n_components,
                                static_cast<int>(eigenvalues.size()));
    } else {
        // Use variance threshold
        double total_variance = eigenvalues.sum();
        double cumsum = 0.0;
        n_components_ = 0;
        for (int i = 0; i < eigenvalues.size(); ++i) {
            cumsum += eigenvalues(i);
            n_components_++;
            if (cumsum / total_variance >= config_.variance_threshold) {
                break;
            }
        }
    }

    // Store components and explained variance
    components_ = eigenvectors.leftCols(n_components_);
    explained_variance_ = eigenvalues.head(n_components_);

    double total_var = eigenvalues.sum();
    explained_variance_ratio_ = explained_variance_ / total_var;

    // Apply whitening if requested
    if (config_.whiten) {
        for (int i = 0; i < n_components_; ++i) {
            components_.col(i) /= std::sqrt(explained_variance_(i));
        }
    }

    fitted_ = true;
    return Result<void>();
}

Result<Eigen::MatrixXd> PCA::transform(const Eigen::MatrixXd& data) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::NOT_INITIALIZED,
            "PCA has not been fitted",
            "PCA"
        );
    }

    // Center and project
    Eigen::MatrixXd centered(data.rows(), data.cols());
    for (int i = 0; i < data.cols(); ++i) {
        centered.col(i) = data.col(i).array() - mean_(i);
    }
    Eigen::MatrixXd transformed = centered * components_;

    return Result<Eigen::MatrixXd>(std::move(transformed));
}

Result<Eigen::MatrixXd> PCA::inverse_transform(const Eigen::MatrixXd& data) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::NOT_INITIALIZED,
            "PCA has not been fitted",
            "PCA"
        );
    }

    // Project back and add mean
    Eigen::MatrixXd reconstructed = data * components_.transpose();
    for (int i = 0; i < reconstructed.cols(); ++i) {
        reconstructed.col(i).array() += mean_(i);
    }

    return Result<Eigen::MatrixXd>(std::move(reconstructed));
}

// ============================================================================
// ADF Test Implementation
// ============================================================================

ADFTest::ADFTest(ADFTestConfig config)
    : config_(config) {}

Result<TestResult> ADFTest::test(const std::vector<double>& data) const {
    if (data.size() < 10) {
        return make_error<TestResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Insufficient data for ADF test (minimum 10 observations required)",
            "ADFTest"
        );
    }

    int n = data.size();
    int lag_order = config_.max_lags;
    if (lag_order < 0) {
        lag_order = select_lag_order(data);
    }

    // Prepare regression: Δy_t = α + βt + γy_{t-1} + Σδ_i Δy_{t-i} + ε_t
    std::vector<double> y_diff = difference(data, 1);
    int n_obs = y_diff.size() - lag_order;

    if (n_obs < 3) {
        return make_error<TestResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Insufficient observations after differencing",
            "ADFTest"
        );
    }

    // Build design matrix
    int n_regressors = 1 + lag_order;  // y_{t-1} + lagged differences
    if (config_.regression == ADFTestConfig::RegressionType::CONSTANT ||
        config_.regression == ADFTestConfig::RegressionType::CONSTANT_TREND) {
        n_regressors++;  // Add constant
    }
    if (config_.regression == ADFTestConfig::RegressionType::CONSTANT_TREND) {
        n_regressors++;  // Add trend
    }

    Eigen::MatrixXd X(n_obs, n_regressors);
    Eigen::VectorXd y(n_obs);

    for (int t = 0; t < n_obs; ++t) {
        int actual_t = lag_order + t;
        y(t) = y_diff[actual_t];

        int col = 0;

        // Constant
        if (config_.regression != ADFTestConfig::RegressionType::NO_CONSTANT) {
            X(t, col++) = 1.0;
        }

        // Trend
        if (config_.regression == ADFTestConfig::RegressionType::CONSTANT_TREND) {
            X(t, col++) = actual_t + 1;
        }

        // Lagged level y_{t-1}
        X(t, col++) = data[actual_t];

        // Lagged differences
        for (int i = 0; i < lag_order; ++i) {
            X(t, col++) = y_diff[actual_t - i - 1];
        }
    }

    // OLS estimation using LDLT decomposition (numerically stable)
    Eigen::LDLT<Eigen::MatrixXd> ldlt_decomp = (X.transpose() * X).ldlt();
    Eigen::VectorXd beta = ldlt_decomp.solve(X.transpose() * y);
    Eigen::VectorXd residuals = y - X * beta;
    double rss = residuals.squaredNorm();
    double sigma2 = rss / (n_obs - n_regressors);

    // Standard error of gamma (coefficient on y_{t-1})
    int gamma_idx = (config_.regression == ADFTestConfig::RegressionType::NO_CONSTANT) ? 0 :
                    (config_.regression == ADFTestConfig::RegressionType::CONSTANT) ? 1 : 2;
    double gamma = beta(gamma_idx);

    // Extract diagonal element via solve instead of full inverse
    Eigen::VectorXd e_gamma = Eigen::VectorXd::Zero(n_regressors);
    e_gamma(gamma_idx) = 1.0;
    double se_gamma = std::sqrt(sigma2 * ldlt_decomp.solve(e_gamma)(gamma_idx));

    // Test statistic
    double adf_stat = gamma / se_gamma;

    // Critical value (approximate)
    double critical_val = calculate_critical_value(n_obs, config_.significance_level);

    TestResult result;
    result.statistic = adf_stat;
    result.critical_value = critical_val;
    result.reject_null = adf_stat < critical_val;  // Reject if stat < critical (more negative)
    result.p_value = -1.0;  // Exact p-value requires MacKinnon tables

    if (result.reject_null) {
        result.interpretation = "Reject null hypothesis: Series appears to be stationary";
    } else {
        result.interpretation = "Cannot reject null hypothesis: Series appears to have a unit root (non-stationary)";
    }

    result.additional_stats["lag_order"] = lag_order;
    result.additional_stats["n_observations"] = n_obs;

    return Result<TestResult>(std::move(result));
}

int ADFTest::select_lag_order(const std::vector<double>& data) const {
    // Use Schwarz criterion: lag = floor(12 * (n/100)^(1/4))
    int n = data.size();
    return std::max(1, static_cast<int>(std::floor(12.0 * std::pow(n / 100.0, 0.25))));
}

double ADFTest::calculate_critical_value(int n_obs, double significance) const {
    // MacKinnon (1996) critical values interpolated by sample size and regression type
    int reg_type;
    switch (config_.regression) {
        case ADFTestConfig::RegressionType::NO_CONSTANT: reg_type = 0; break;
        case ADFTestConfig::RegressionType::CONSTANT_TREND: reg_type = 2; break;
        default: reg_type = 1; break;
    }
    return critical_values::interpolate_adf_cv(n_obs, reg_type, significance);
}

// ============================================================================
// KPSS Test Implementation
// ============================================================================

KPSSTest::KPSSTest(KPSSTestConfig config)
    : config_(config) {}

Result<TestResult> KPSSTest::test(const std::vector<double>& data) const {
    if (data.size() < 10) {
        return make_error<TestResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Insufficient data for KPSS test (minimum 10 observations required)",
            "KPSSTest"
        );
    }

    int n = data.size();
    Eigen::VectorXd y = Eigen::Map<const Eigen::VectorXd>(data.data(), n);

    // Regression on constant or constant + trend
    Eigen::MatrixXd X;
    if (config_.regression == KPSSTestConfig::RegressionType::CONSTANT) {
        X = Eigen::MatrixXd::Ones(n, 1);
    } else {
        X = Eigen::MatrixXd(n, 2);
        X.col(0) = Eigen::VectorXd::Ones(n);
        X.col(1) = Eigen::VectorXd::LinSpaced(n, 1, n);
    }

    // OLS residuals
    Eigen::VectorXd beta = (X.transpose() * X).ldlt().solve(X.transpose() * y);
    Eigen::VectorXd residuals = y - X * beta;

    // Partial sum process
    Eigen::VectorXd S(n);
    S(0) = residuals(0);
    for (int i = 1; i < n; ++i) {
        S(i) = S(i - 1) + residuals(i);
    }

    // Long-run variance estimate
    int lag_order = config_.max_lags;
    if (lag_order < 0) {
        lag_order = select_lag_order(n);
    }

    double s2 = residuals.squaredNorm() / n;
    for (int lag = 1; lag <= lag_order; ++lag) {
        double gamma = 0.0;
        for (int t = lag; t < n; ++t) {
            gamma += residuals(t) * residuals(t - lag);
        }
        gamma /= n;
        double weight = 1.0 - static_cast<double>(lag) / (lag_order + 1);
        s2 += 2.0 * weight * gamma;
    }

    // KPSS statistic
    double kpss_stat = S.squaredNorm() / (n * n * s2);

    bool has_trend = (config_.regression == KPSSTestConfig::RegressionType::CONSTANT_TREND);
    double critical_val = calculate_critical_value(config_.significance_level, has_trend);

    TestResult result;
    result.statistic = kpss_stat;
    result.critical_value = critical_val;
    result.reject_null = kpss_stat > critical_val;
    result.p_value = -1.0;  // Exact p-value requires tables

    if (result.reject_null) {
        result.interpretation = "Reject null hypothesis: Series appears to be non-stationary";
    } else {
        result.interpretation = "Cannot reject null hypothesis: Series appears to be stationary";
    }

    result.additional_stats["lag_order"] = lag_order;
    result.additional_stats["long_run_variance"] = s2;

    return Result<TestResult>(std::move(result));
}

int KPSSTest::select_lag_order(int n_obs) const {
    // Hobijn et al. (1998): lag = floor(4 * (n/100)^(1/4))
    return std::max(1, static_cast<int>(std::floor(4.0 * std::pow(n_obs / 100.0, 0.25))));
}

double KPSSTest::calculate_critical_value(double significance, bool has_trend) const {
    return critical_values::kpss_critical_value(significance, has_trend);
}

// ============================================================================
// Johansen Test Implementation
// ============================================================================

JohansenTest::JohansenTest(JohansenTestConfig config)
    : config_(config) {}

Result<CointegrationResult> JohansenTest::test(const Eigen::MatrixXd& data) const {
    if (data.rows() < 20 || data.cols() < 2) {
        return make_error<CointegrationResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Insufficient data for Johansen test (minimum 20 observations, 2 series)",
            "JohansenTest"
        );
    }

    int n = data.rows();
    int p = data.cols();
    int k = config_.max_lags;

    // Difference the data
    Eigen::MatrixXd dY = data.bottomRows(n - 1) - data.topRows(n - 1);
    Eigen::MatrixXd Y_lagged = data.topRows(n - k - 1);
    Eigen::MatrixXd dY_current = dY.bottomRows(n - k - 1);

    int n_obs = n - k - 1;

    // Residuals from regressing dY on lagged differences (if k > 0)
    Eigen::MatrixXd R0 = dY_current;
    Eigen::MatrixXd R1 = Y_lagged;

    if (k > 0) {
        Eigen::MatrixXd dY_lagged(n_obs, p * k);
        for (int lag = 0; lag < k; ++lag) {
            dY_lagged.block(0, lag * p, n_obs, p) =
                dY.block(k - lag - 1, 0, n_obs, p);
        }

        // Residuals from regressions
        Eigen::MatrixXd M0 = (dY_lagged.transpose() * dY_lagged).ldlt().solve(
            dY_lagged.transpose() * dY_current);
        R0 = dY_current - dY_lagged * M0;

        Eigen::MatrixXd M1 = (dY_lagged.transpose() * dY_lagged).ldlt().solve(
            dY_lagged.transpose() * Y_lagged);
        R1 = Y_lagged - dY_lagged * M1;
    }

    // Product moment matrices
    Eigen::MatrixXd S00 = (R0.transpose() * R0) / n_obs;
    Eigen::MatrixXd S11 = (R1.transpose() * R1) / n_obs;
    Eigen::MatrixXd S01 = (R0.transpose() * R1) / n_obs;
    Eigen::MatrixXd S10 = S01.transpose();

    // Solve eigenvalue problem: S11^{-1} S10 S00^{-1} S01
    Eigen::MatrixXd M = S11.ldlt().solve(S10) * S00.ldlt().solve(S01);

    Eigen::EigenSolver<Eigen::MatrixXd> solver(M);
    if (solver.info() != Eigen::Success) {
        return make_error<CointegrationResult>(
            ErrorCode::INVALID_DATA,
            "Eigenvalue decomposition failed in Johansen test",
            "JohansenTest"
        );
    }

    Eigen::VectorXcd eigenvalues_complex = solver.eigenvalues();
    Eigen::MatrixXcd eigenvectors_complex = solver.eigenvectors();

    // Extract real eigenvalues and sort
    std::vector<std::pair<double, int>> eig_pairs;
    for (int i = 0; i < p; ++i) {
        eig_pairs.push_back({eigenvalues_complex(i).real(), i});
    }
    std::sort(eig_pairs.begin(), eig_pairs.end(),
             [](const auto& a, const auto& b) { return a.first > b.first; });

    CointegrationResult result;
    result.eigenvalues.resize(p);
    result.cointegrating_vectors = Eigen::MatrixXd(p, p);

    for (int i = 0; i < p; ++i) {
        result.eigenvalues[i] = eig_pairs[i].first;
        int idx = eig_pairs[i].second;
        result.cointegrating_vectors.col(i) = eigenvectors_complex.col(idx).real();
    }

    // Calculate trace statistics
    result.trace_statistics.resize(p);
    for (int r = 0; r < p; ++r) {
        double trace = 0.0;
        for (int i = r; i < p; ++i) {
            trace += std::log(1.0 - result.eigenvalues[i]);
        }
        result.trace_statistics[r] = -n_obs * trace;
    }

    // Critical values (approximate for p=2, can be extended)
    result.critical_values = get_critical_values(p, 0);

    // Determine cointegration rank
    result.cointegration_rank = 0;
    result.is_cointegrated = false;
    for (int r = 0; r < p; ++r) {
        if (result.trace_statistics[r] > result.critical_values[r]) {
            result.cointegration_rank = r + 1;
            result.is_cointegrated = true;
        } else {
            break;
        }
    }

    return Result<CointegrationResult>(std::move(result));
}

std::vector<double> JohansenTest::get_critical_values(int n_series, int rank) const {
    return critical_values::johansen_trace_critical_values(n_series, config_.significance_level);
}

// ============================================================================
// Engle-Granger Test Implementation
// ============================================================================

EngleGrangerTest::EngleGrangerTest(EngleGrangerConfig config)
    : config_(config)
    , adf_test_(ADFTestConfig{}) {}

Result<TestResult> EngleGrangerTest::test(const std::vector<double>& y,
                                         const std::vector<double>& x) const {
    if (y.size() != x.size() || y.size() < 20) {
        return make_error<TestResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Invalid input: series must have same length and at least 20 observations",
            "EngleGrangerTest"
        );
    }

    // Step 1: OLS regression of y on x
    auto [beta, residuals] = ols_regression(y, x);

    // Step 2: ADF test on residuals
    auto adf_result = adf_test_.test(residuals);
    if (adf_result.is_error()) {
        return make_error<TestResult>(
            adf_result.error()->code(),
            adf_result.error()->what(),
            "EngleGrangerTest"
        );
    }

    TestResult result = adf_result.value();
    result.additional_stats["regression_coefficient"] = beta;

    // Modify interpretation for cointegration context
    if (result.reject_null) {
        result.interpretation = "Reject null hypothesis: Series appear to be cointegrated";
    } else {
        result.interpretation = "Cannot reject null hypothesis: No evidence of cointegration";
    }

    return Result<TestResult>(std::move(result));
}

std::pair<double, std::vector<double>> EngleGrangerTest::ols_regression(
    const std::vector<double>& y,
    const std::vector<double>& x) const {

    int n = y.size();

    // Calculate means
    double mean_x = calculate_mean(x);
    double mean_y = calculate_mean(y);

    // Calculate beta = cov(x,y) / var(x)
    double cov_xy = 0.0;
    double var_x = 0.0;
    for (int i = 0; i < n; ++i) {
        cov_xy += (x[i] - mean_x) * (y[i] - mean_y);
        var_x += (x[i] - mean_x) * (x[i] - mean_x);
    }
    double beta = cov_xy / var_x;
    double alpha = mean_y - beta * mean_x;

    // Calculate residuals
    std::vector<double> residuals(n);
    for (int i = 0; i < n; ++i) {
        residuals[i] = y[i] - (alpha + beta * x[i]);
    }

    return {beta, residuals};
}

// ============================================================================
// GARCH Implementation
// ============================================================================

GARCH::GARCH(GARCHConfig config)
    : config_(config)
    , omega_(config.omega)
    , alpha_(config.alpha)
    , beta_(config.beta) {}

Result<void> GARCH::fit(const std::vector<double>& returns) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (returns.size() < 50) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Insufficient data for GARCH model (minimum 50 observations)",
            "GARCH"
        );
    }

    // Calculate mean return (assume zero for simplicity)
    double mean_return = calculate_mean(returns);

    // Demeaned returns (residuals)
    residuals_.resize(returns.size());
    for (size_t i = 0; i < returns.size(); ++i) {
        residuals_[i] = returns[i] - mean_return;
    }

    // Estimate parameters
    auto result = estimate_parameters(residuals_);
    if (result.is_error()) {
        return result;
    }

    // Calculate conditional variances
    conditional_variances_.resize(residuals_.size());

    // Initial variance (unconditional variance estimate)
    double initial_var = calculate_variance(residuals_, 0.0);
    conditional_variances_[0] = initial_var;

    for (size_t t = 1; t < residuals_.size(); ++t) {
        conditional_variances_[t] = omega_ +
                                   alpha_ * residuals_[t-1] * residuals_[t-1] +
                                   beta_ * conditional_variances_[t-1];
    }

    current_volatility_ = std::sqrt(conditional_variances_.back());
    fitted_ = true;

    return Result<void>();
}

Result<void> GARCH::estimate_parameters(const std::vector<double>& returns) {
    // Simple estimation using method of moments / grid search
    // In practice, use maximum likelihood with optimization library

    double unconditional_var = calculate_variance(returns, 0.0);

    // Initialize with reasonable values
    omega_ = unconditional_var * 0.01;
    alpha_ = config_.alpha;
    beta_ = config_.beta;

    // Ensure stationarity constraint: alpha + beta < 1
    if (alpha_ + beta_ >= 1.0) {
        alpha_ = 0.1;
        beta_ = 0.85;
    }

    // Simple grid search for better parameters
    double best_ll = log_likelihood(returns, omega_, alpha_, beta_);

    for (double a = 0.05; a <= 0.3; a += 0.05) {
        for (double b = 0.6; b <= 0.9; b += 0.05) {
            if (a + b < 0.995) {
                double w = unconditional_var * (1.0 - a - b);
                double ll = log_likelihood(returns, w, a, b);
                if (ll > best_ll) {
                    best_ll = ll;
                    omega_ = w;
                    alpha_ = a;
                    beta_ = b;
                }
            }
        }
    }

    return Result<void>();
}

double GARCH::log_likelihood(const std::vector<double>& returns,
                            double omega, double alpha, double beta) const {
    std::vector<double> var(returns.size());
    double initial_var = calculate_variance(returns, 0.0);
    var[0] = initial_var;

    double ll = -0.5 * (std::log(2.0 * M_PI) + std::log(var[0]) +
                       returns[0] * returns[0] / var[0]);

    for (size_t t = 1; t < returns.size(); ++t) {
        var[t] = omega + alpha * returns[t-1] * returns[t-1] + beta * var[t-1];
        if (var[t] <= 0) return -std::numeric_limits<double>::infinity();
        ll += -0.5 * (std::log(2.0 * M_PI) + std::log(var[t]) +
                     returns[t] * returns[t] / var[t]);
    }

    return ll;
}

Result<std::vector<double>> GARCH::forecast(int n_periods) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<std::vector<double>>(
            ErrorCode::NOT_INITIALIZED,
            "GARCH model has not been fitted",
            "GARCH"
        );
    }

    std::vector<double> forecasts(n_periods);
    double h = conditional_variances_.back();

    for (int i = 0; i < n_periods; ++i) {
        h = omega_ + (alpha_ + beta_) * h;
        forecasts[i] = std::sqrt(h);
    }

    return Result<std::vector<double>>(std::move(forecasts));
}

Result<double> GARCH::get_current_volatility() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<double>(
            ErrorCode::NOT_INITIALIZED,
            "GARCH model has not been fitted",
            "GARCH"
        );
    }

    return Result<double>(current_volatility_);
}

Result<void> GARCH::update(double new_return) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<void>(
            ErrorCode::NOT_INITIALIZED,
            "GARCH model has not been fitted",
            "GARCH"
        );
    }

    residuals_.push_back(new_return);

    double new_var = omega_ +
                    alpha_ * new_return * new_return +
                    beta_ * conditional_variances_.back();

    conditional_variances_.push_back(new_var);
    current_volatility_ = std::sqrt(new_var);

    return Result<void>();
}

// ============================================================================
// Kalman Filter Implementation
// ============================================================================

KalmanFilter::KalmanFilter(KalmanFilterConfig config)
    : config_(config) {

    // Initialize matrices with default values
    F_ = Eigen::MatrixXd::Identity(config.state_dim, config.state_dim);
    H_ = Eigen::MatrixXd::Identity(config.obs_dim, config.state_dim);
    Q_ = Eigen::MatrixXd::Identity(config.state_dim, config.state_dim) * config.process_noise;
    R_ = Eigen::MatrixXd::Identity(config.obs_dim, config.obs_dim) * config.measurement_noise;
}

Result<void> KalmanFilter::initialize(const Eigen::VectorXd& initial_state) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initial_state.size() != config_.state_dim) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Initial state dimension mismatch",
            "KalmanFilter"
        );
    }

    x_ = initial_state;
    P_ = Eigen::MatrixXd::Identity(config_.state_dim, config_.state_dim);
    initialized_ = true;

    return Result<void>();
}

Result<Eigen::VectorXd> KalmanFilter::predict() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::NOT_INITIALIZED,
            "Kalman filter has not been initialized",
            "KalmanFilter"
        );
    }

    // Predict state: x_k|k-1 = F * x_k-1|k-1
    x_ = F_ * x_;

    // Predict covariance: P_k|k-1 = F * P_k-1|k-1 * F^T + Q
    P_ = F_ * P_ * F_.transpose() + Q_;

    return Result<Eigen::VectorXd>(x_);
}

Result<Eigen::VectorXd> KalmanFilter::update(const Eigen::VectorXd& observation) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::NOT_INITIALIZED,
            "Kalman filter has not been initialized",
            "KalmanFilter"
        );
    }

    if (observation.size() != config_.obs_dim) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::INVALID_ARGUMENT,
            "Observation dimension mismatch",
            "KalmanFilter"
        );
    }

    // Innovation: y = z - H * x_k|k-1
    Eigen::VectorXd y = observation - H_ * x_;

    // Innovation covariance: S = H * P_k|k-1 * H^T + R
    Eigen::MatrixXd S = H_ * P_ * H_.transpose() + R_;

    // Kalman gain: K = P*H' * S^{-1} = (S^{-1} * (P*H')')' using Cholesky
    Eigen::MatrixXd PH_t = P_ * H_.transpose();
    Eigen::MatrixXd K;
    Eigen::LLT<Eigen::MatrixXd> llt_S(S);
    if (llt_S.info() == Eigen::Success) {
        K = llt_S.solve(PH_t.transpose()).transpose();
    } else {
        K = Eigen::LDLT<Eigen::MatrixXd>(S).solve(PH_t.transpose()).transpose();
    }

    // Update state: x_k|k = x_k|k-1 + K * y
    x_ = x_ + K * y;

    // Update covariance: P_k|k = (I - K * H) * P_k|k-1
    Eigen::MatrixXd I = Eigen::MatrixXd::Identity(config_.state_dim, config_.state_dim);
    P_ = (I - K * H_) * P_;

    return Result<Eigen::VectorXd>(x_);
}

Result<Eigen::VectorXd> KalmanFilter::get_state() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::NOT_INITIALIZED,
            "Kalman filter has not been initialized",
            "KalmanFilter"
        );
    }

    return Result<Eigen::VectorXd>(x_);
}

// ============================================================================
// HMM Implementation
// ============================================================================

HMM::HMM(HMMConfig config)
    : config_(config) {}

Result<void> HMM::initialize(const Eigen::VectorXd& initial_state) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initial_state.size() != config_.n_states) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Initial state dimension must match number of states",
            "HMM"
        );
    }

    state_probs_ = initial_state;
    initial_probs_ = initial_state;

    // Initialize default emission parameters if not already set
    if (means_.empty()) {
        means_.resize(config_.n_states);
        covariances_.resize(config_.n_states);

        // Simple default initialization: states at different locations
        for (int k = 0; k < config_.n_states; ++k) {
            means_[k] = Eigen::VectorXd::Zero(1);  // Default to 1D
            means_[k](0) = static_cast<double>(k);
            covariances_[k] = Eigen::MatrixXd::Identity(1, 1);
        }
    }

    // Initialize default transition matrix if not set
    if (transition_matrix_.size() == 0) {
        transition_matrix_ = Eigen::MatrixXd::Ones(config_.n_states, config_.n_states) / config_.n_states;
    }

    initialized_ = true;

    return Result<void>();
}

Result<void> HMM::fit(const Eigen::MatrixXd& observations) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (observations.rows() < 10) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Insufficient observations for HMM fitting",
            "HMM"
        );
    }

    int T = observations.rows();
    int D = observations.cols();

    // Initialize parameters
    initialize_parameters(observations);

    // Baum-Welch algorithm (EM)
    double prev_log_likelihood = -std::numeric_limits<double>::infinity();

    for (int iter = 0; iter < config_.max_iterations; ++iter) {
        Eigen::MatrixXd gamma(T, config_.n_states);
        Eigen::MatrixXd xi(T - 1, config_.n_states * config_.n_states);

        // E-step: Forward-backward algorithm
        double log_likelihood = forward_backward(observations, gamma, xi);

        // Check convergence
        if (std::abs(log_likelihood - prev_log_likelihood) < config_.tolerance) {
            break;
        }
        prev_log_likelihood = log_likelihood;

        // M-step: Update parameters

        // Update initial probabilities
        initial_probs_ = gamma.row(0).transpose();

        // Update transition matrix
        for (int i = 0; i < config_.n_states; ++i) {
            double row_sum = gamma.col(i).segment(0, T - 1).sum();
            for (int j = 0; j < config_.n_states; ++j) {
                double xi_sum = 0.0;
                for (int t = 0; t < T - 1; ++t) {
                    xi_sum += xi(t, i * config_.n_states + j);
                }
                transition_matrix_(i, j) = xi_sum / row_sum;
            }
        }

        // Update emission parameters (Gaussian means and covariances)
        for (int k = 0; k < config_.n_states; ++k) {
            double gamma_sum = gamma.col(k).sum();

            // Update mean
            means_[k] = Eigen::VectorXd::Zero(D);
            for (int t = 0; t < T; ++t) {
                means_[k] += gamma(t, k) * observations.row(t).transpose();
            }
            means_[k] /= gamma_sum;

            // Update covariance
            covariances_[k] = Eigen::MatrixXd::Zero(D, D);
            for (int t = 0; t < T; ++t) {
                Eigen::VectorXd diff = observations.row(t).transpose() - means_[k];
                covariances_[k] += gamma(t, k) * (diff * diff.transpose());
            }
            covariances_[k] /= gamma_sum;

            // Add small regularization to prevent singularity
            covariances_[k] += Eigen::MatrixXd::Identity(D, D) * 1e-6;
        }
    }

    initialized_ = true;
    return Result<void>();
}

void HMM::initialize_parameters(const Eigen::MatrixXd& observations) {
    int D = observations.cols();

    // Initialize state probabilities uniformly
    initial_probs_ = Eigen::VectorXd::Ones(config_.n_states) / config_.n_states;
    state_probs_ = initial_probs_;

    // Initialize transition matrix
    if (config_.init_random) {
        transition_matrix_ = Eigen::MatrixXd::Random(config_.n_states, config_.n_states).array().abs();
        // Normalize rows to sum to 1
        for (int i = 0; i < config_.n_states; ++i) {
            double row_sum = transition_matrix_.row(i).sum();
            transition_matrix_.row(i) /= row_sum;
        }
    } else {
        transition_matrix_ = Eigen::MatrixXd::Ones(config_.n_states, config_.n_states) / config_.n_states;
    }

    // Initialize emission parameters using k-means-like approach
    means_.resize(config_.n_states);
    covariances_.resize(config_.n_states);

    int step = observations.rows() / config_.n_states;
    for (int k = 0; k < config_.n_states; ++k) {
        int idx = k * step;
        means_[k] = observations.row(idx).transpose();
        covariances_[k] = Eigen::MatrixXd::Identity(D, D);
    }
}

double HMM::forward_backward(const Eigen::MatrixXd& observations,
                            Eigen::MatrixXd& gamma,
                            Eigen::MatrixXd& xi) const {
    int T = observations.rows();
    int N = config_.n_states;

    // Pre-compute log quantities
    Eigen::MatrixXd log_emit(T, N);
    for (int t = 0; t < T; ++t) {
        for (int j = 0; j < N; ++j) {
            log_emit(t, j) = log_emission_probability(observations.row(t), j);
        }
    }

    Eigen::MatrixXd log_A(N, N);
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            log_A(i, j) = (transition_matrix_(i, j) > 0)
                ? std::log(transition_matrix_(i, j))
                : -std::numeric_limits<double>::infinity();
        }
    }

    Eigen::VectorXd log_pi(N);
    for (int i = 0; i < N; ++i) {
        log_pi(i) = (initial_probs_(i) > 0)
            ? std::log(initial_probs_(i))
            : -std::numeric_limits<double>::infinity();
    }

    // Log forward pass
    Eigen::MatrixXd log_alpha(T, N);
    for (int i = 0; i < N; ++i) {
        log_alpha(0, i) = log_pi(i) + log_emit(0, i);
    }

    std::vector<double> temp(N);
    for (int t = 1; t < T; ++t) {
        for (int j = 0; j < N; ++j) {
            for (int i = 0; i < N; ++i) {
                temp[i] = log_alpha(t - 1, i) + log_A(i, j);
            }
            log_alpha(t, j) = critical_values::log_sum_exp(temp.data(), N) + log_emit(t, j);
        }
    }

    // Log backward pass
    Eigen::MatrixXd log_beta(T, N);
    log_beta.row(T - 1).setZero();  // log(1) = 0

    for (int t = T - 2; t >= 0; --t) {
        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < N; ++j) {
                temp[j] = log_A(i, j) + log_emit(t + 1, j) + log_beta(t + 1, j);
            }
            log_beta(t, i) = critical_values::log_sum_exp(temp.data(), N);
        }
    }

    // Compute gamma: convert back to probability space for M-step
    for (int t = 0; t < T; ++t) {
        // log_norm for this time step
        for (int i = 0; i < N; ++i) {
            temp[i] = log_alpha(t, i) + log_beta(t, i);
        }
        double log_norm = critical_values::log_sum_exp(temp.data(), N);
        for (int i = 0; i < N; ++i) {
            gamma(t, i) = std::exp(log_alpha(t, i) + log_beta(t, i) - log_norm);
        }
    }

    // Compute xi: transition posterior
    std::vector<double> temp_xi(N * N);
    for (int t = 0; t < T - 1; ++t) {
        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < N; ++j) {
                temp_xi[i * N + j] = log_alpha(t, i) + log_A(i, j) +
                                     log_emit(t + 1, j) + log_beta(t + 1, j);
            }
        }
        double log_norm = critical_values::log_sum_exp(temp_xi.data(), N * N);
        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < N; ++j) {
                xi(t, i * N + j) = std::exp(temp_xi[i * N + j] - log_norm);
            }
        }
    }

    // Return log-likelihood from forward variables
    for (int i = 0; i < N; ++i) {
        temp[i] = log_alpha(T - 1, i);
    }
    return critical_values::log_sum_exp(temp.data(), N);
}

double HMM::log_emission_probability(const Eigen::VectorXd& obs, int state) const {
    int D = obs.size();
    Eigen::VectorXd diff = obs - means_[state];

    // Use LLT Cholesky for numerical stability
    Eigen::LLT<Eigen::MatrixXd> llt(covariances_[state]);
    if (llt.info() == Eigen::Success) {
        // Mahalanobis distance via triangular solve
        Eigen::VectorXd v = llt.matrixL().solve(diff);
        double mahal_sq = v.squaredNorm();
        // Log-determinant = 2 * sum(log(diag(L)))
        double log_det = 2.0 * llt.matrixL().toDenseMatrix().diagonal().array().log().sum();
        return -0.5 * (D * std::log(2.0 * M_PI) + log_det + mahal_sq);
    } else {
        // LDLT fallback
        Eigen::LDLT<Eigen::MatrixXd> ldlt(covariances_[state]);
        Eigen::VectorXd solved = ldlt.solve(diff);
        double mahal_sq = diff.dot(solved);
        // Log-determinant from LDLT: sum of log of |D diagonal|
        double log_det = ldlt.vectorD().array().abs().log().sum();
        return -0.5 * (D * std::log(2.0 * M_PI) + log_det + mahal_sq);
    }
}

double HMM::emission_probability(const Eigen::VectorXd& obs, int state) const {
    return std::exp(log_emission_probability(obs, state));
}

Result<std::vector<int>> HMM::decode(const Eigen::MatrixXd& observations) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<std::vector<int>>(
            ErrorCode::NOT_INITIALIZED,
            "HMM has not been fitted or initialized",
            "HMM"
        );
    }

    int T = observations.rows();

    // Viterbi algorithm
    Eigen::MatrixXd delta(T, config_.n_states);
    Eigen::MatrixXi psi(T, config_.n_states);

    // Initialize
    for (int i = 0; i < config_.n_states; ++i) {
        double log_pi = (initial_probs_(i) > 0)
            ? std::log(initial_probs_(i))
            : -std::numeric_limits<double>::infinity();
        delta(0, i) = log_pi + log_emission_probability(observations.row(0), i);
        psi(0, i) = 0;
    }

    // Recursion
    for (int t = 1; t < T; ++t) {
        for (int j = 0; j < config_.n_states; ++j) {
            double max_val = -std::numeric_limits<double>::infinity();
            int max_state = 0;

            for (int i = 0; i < config_.n_states; ++i) {
                double log_a = (transition_matrix_(i, j) > 0)
                    ? std::log(transition_matrix_(i, j))
                    : -std::numeric_limits<double>::infinity();
                double val = delta(t - 1, i) + log_a;
                if (val > max_val) {
                    max_val = val;
                    max_state = i;
                }
            }

            delta(t, j) = max_val + log_emission_probability(observations.row(t), j);
            psi(t, j) = max_state;
        }
    }

    // Backtrack
    std::vector<int> states(T);

    // Find most likely final state
    int max_idx;
    delta.row(T - 1).maxCoeff(&max_idx);
    states[T - 1] = max_idx;

    // Backtrack through time
    for (int t = T - 2; t >= 0; --t) {
        states[t] = psi(t + 1, states[t + 1]);
    }

    return Result<std::vector<int>>(std::move(states));
}

Result<Eigen::VectorXd> HMM::predict() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::NOT_INITIALIZED,
            "HMM has not been initialized",
            "HMM"
        );
    }

    // Predict next state probabilities
    Eigen::VectorXd next_probs = transition_matrix_.transpose() * state_probs_;
    return Result<Eigen::VectorXd>(next_probs);
}

Result<Eigen::VectorXd> HMM::update(const Eigen::VectorXd& observation) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::NOT_INITIALIZED,
            "HMM has not been initialized",
            "HMM"
        );
    }

    // Predict
    Eigen::VectorXd predicted_probs = transition_matrix_.transpose() * state_probs_;

    // Update with observation likelihood
    for (int i = 0; i < config_.n_states; ++i) {
        predicted_probs(i) *= emission_probability(observation, i);
    }

    // Normalize
    double sum = predicted_probs.sum();
    if (sum > 0) {
        state_probs_ = predicted_probs / sum;
    }

    return Result<Eigen::VectorXd>(state_probs_);
}

Result<Eigen::VectorXd> HMM::get_state() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::NOT_INITIALIZED,
            "HMM has not been initialized",
            "HMM"
        );
    }

    return Result<Eigen::VectorXd>(state_probs_);
}

} // namespace statistics
} // namespace trade_ngin
