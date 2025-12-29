#include "trade_ngin/analysis/stationarity_tests.hpp"
#include "trade_ngin/analysis/preprocessing.hpp"
#include "trade_ngin/analysis/statistical_distributions.hpp"
#include <Eigen/Dense>
#include <cmath>
#include <algorithm>

namespace trade_ngin {
namespace analysis {

namespace {
    // Critical values for ADF test (MacKinnon 1996)
    // [no constant, constant, constant+trend]
    const double ADF_CRITICAL_VALUES[3][3] = {
        {-2.66, -1.95, -1.60},  // No constant
        {-3.75, -3.00, -2.62},  // Constant
        {-4.38, -3.60, -3.24}   // Constant + trend
    };

    // Critical values for KPSS test (Kwiatkowski et al. 1992)
    // [level, trend]
    const double KPSS_CRITICAL_VALUES[2][3] = {
        {0.739, 0.463, 0.347},  // Level stationarity
        {0.216, 0.146, 0.119}   // Trend stationarity
    };

    // Auto-select lag using Schwarz Information Criterion (SIC)
    int select_lag_sic(const std::vector<double>& data, int max_lag) {
        int n = data.size();
        if (max_lag <= 0) {
            // Rule of thumb: max_lag = 12 * (n/100)^(1/4)
            max_lag = std::min(static_cast<int>(12.0 * std::pow(n / 100.0, 0.25)), n / 2);
        }

        double best_sic = std::numeric_limits<double>::max();
        int best_lag = 0;

        for (int lag = 0; lag <= max_lag; ++lag) {
            int n_obs = n - lag - 1;
            if (n_obs < 10) break;

            // Calculate RSS for this lag
            double rss = 0.0;
            for (int i = lag + 1; i < n; ++i) {
                double diff = data[i] - data[i-1];
                rss += diff * diff;
            }

            // SIC = log(RSS/n) + k*log(n)/n, where k is number of parameters
            int k = lag + 2; // intercept + lagged level + lag terms
            double sic = std::log(rss / n_obs) + k * std::log(n_obs) / n_obs;

            if (sic < best_sic) {
                best_sic = sic;
                best_lag = lag;
            }
        }

        return best_lag;
    }

    // Perform OLS regression
    struct OLSResult {
        Eigen::VectorXd coefficients;
        double rss;  // Residual sum of squares
        double tss;  // Total sum of squares
        int n_obs;
    };

    OLSResult ols_regression(const Eigen::MatrixXd& X, const Eigen::VectorXd& y) {
        // Solve: beta = (X'X)^(-1) X'y
        Eigen::VectorXd beta = (X.transpose() * X).ldlt().solve(X.transpose() * y);

        // Calculate residuals
        Eigen::VectorXd residuals = y - X * beta;
        double rss = residuals.squaredNorm();

        // Calculate TSS
        double y_mean = y.mean();
        double tss = (y.array() - y_mean).square().sum();

        OLSResult result;
        result.coefficients = beta;
        result.rss = rss;
        result.tss = tss;
        result.n_obs = y.size();

        return result;
    }
}

// Augmented Dickey-Fuller test implementation
Result<ADFResult> augmented_dickey_fuller_test(
    const std::vector<double>& data,
    int max_lag,
    ADFRegressionType regression)
{
    if (data.size() < 20) {
        return make_error<ADFResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Need at least 20 observations for ADF test",
            "augmented_dickey_fuller_test"
        );
    }

    int n = data.size();

    // Auto-select lag if needed
    int lag = (max_lag > 0) ? max_lag : select_lag_sic(data, max_lag);
    lag = std::min(lag, n / 3); // Safety check

    // Construct regression data
    // Δy_t = α + βt + γy_{t-1} + δ₁Δy_{t-1} + ... + δₚΔy_{t-p} + ε_t

    int n_obs = n - lag - 1;
    if (n_obs < 10) {
        return make_error<ADFResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Not enough observations after accounting for lags",
            "augmented_dickey_fuller_test"
        );
    }

    // Determine number of regressors
    int n_regressors = 1; // y_{t-1}
    if (regression == ADFRegressionType::CONSTANT || regression == ADFRegressionType::CONSTANT_TREND) {
        n_regressors++; // constant
    }
    if (regression == ADFRegressionType::CONSTANT_TREND) {
        n_regressors++; // trend
    }
    n_regressors += lag; // lagged differences

    Eigen::MatrixXd X(n_obs, n_regressors);
    Eigen::VectorXd y(n_obs);

    // Build regression matrices
    for (int i = 0; i < n_obs; ++i) {
        int t = lag + 1 + i;

        // Dependent variable: Δy_t
        y(i) = data[t] - data[t-1];

        int col = 0;

        // Constant term
        if (regression == ADFRegressionType::CONSTANT || regression == ADFRegressionType::CONSTANT_TREND) {
            X(i, col++) = 1.0;
        }

        // Trend term
        if (regression == ADFRegressionType::CONSTANT_TREND) {
            X(i, col++) = t;
        }

        // Lagged level y_{t-1}
        X(i, col++) = data[t-1];

        // Lagged differences
        for (int k = 1; k <= lag; ++k) {
            X(i, col++) = data[t-k] - data[t-k-1];
        }
    }

    // Perform OLS regression
    OLSResult ols = ols_regression(X, y);

    // The coefficient of interest is y_{t-1}
    int gamma_idx = (regression == ADFRegressionType::NO_CONSTANT) ? 0 :
                    (regression == ADFRegressionType::CONSTANT) ? 1 : 2;

    double gamma = ols.coefficients(gamma_idx);

    // Calculate standard error of gamma
    double sigma_squared = ols.rss / (n_obs - n_regressors);
    Eigen::MatrixXd XtX_inv = (X.transpose() * X).inverse();
    double se_gamma = std::sqrt(sigma_squared * XtX_inv(gamma_idx, gamma_idx));

    // ADF test statistic
    double test_stat = gamma / se_gamma;

    // Get critical values
    int reg_type = static_cast<int>(regression);
    double cv_1 = ADF_CRITICAL_VALUES[reg_type][0];
    double cv_5 = ADF_CRITICAL_VALUES[reg_type][1];
    double cv_10 = ADF_CRITICAL_VALUES[reg_type][2];

    // Approximate p-value using interpolation
    double p_value = 0.5;
    if (test_stat < cv_1) {
        p_value = 0.01;
    } else if (test_stat < cv_5) {
        p_value = 0.01 + 0.04 * (test_stat - cv_1) / (cv_5 - cv_1);
    } else if (test_stat < cv_10) {
        p_value = 0.05 + 0.05 * (test_stat - cv_5) / (cv_10 - cv_5);
    } else {
        p_value = 0.10 + 0.40 * std::min(1.0, (test_stat - cv_10) / std::abs(cv_10));
    }

    ADFResult result;
    result.test_statistic = test_stat;
    result.p_value = p_value;
    result.lags_used = lag;
    result.n_obs = n_obs;
    result.critical_value_1 = cv_1;
    result.critical_value_5 = cv_5;
    result.critical_value_10 = cv_10;
    result.is_stationary_1 = test_stat < cv_1;
    result.is_stationary_5 = test_stat < cv_5;
    result.is_stationary_10 = test_stat < cv_10;

    return Result<ADFResult>(result);
}

// ADF test on Bar vector
Result<ADFResult> augmented_dickey_fuller_test(
    const std::vector<Bar>& bars,
    int max_lag,
    ADFRegressionType regression)
{
    std::vector<double> prices = Normalization::extract_close_prices(bars);
    return augmented_dickey_fuller_test(prices, max_lag, regression);
}

// KPSS test implementation
Result<KPSSResult> kpss_test(
    const std::vector<double>& data,
    int max_lag,
    KPSSType test_type)
{
    if (data.size() < 20) {
        return make_error<KPSSResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Need at least 20 observations for KPSS test",
            "kpss_test"
        );
    }

    int n = data.size();

    // Auto-select lag if needed
    int lag = max_lag;
    if (lag <= 0) {
        // Newey-West lag selection: l = floor(4 * (n/100)^(2/9))
        lag = std::max(1, static_cast<int>(4.0 * std::pow(n / 100.0, 2.0 / 9.0)));
    }

    // Detrend the data
    Eigen::VectorXd y(n);
    for (int i = 0; i < n; ++i) {
        y(i) = data[i];
    }

    Eigen::VectorXd residuals;

    if (test_type == KPSSType::LEVEL) {
        // Demean only
        double mean = y.mean();
        residuals = y.array() - mean;
    } else {
        // Detrend (remove linear trend)
        Eigen::MatrixXd X(n, 2);
        for (int i = 0; i < n; ++i) {
            X(i, 0) = 1.0;  // constant
            X(i, 1) = i;    // trend
        }

        OLSResult ols = ols_regression(X, y);
        residuals = y - X * ols.coefficients;
    }

    // Calculate partial sums
    Eigen::VectorXd S(n);
    S(0) = residuals(0);
    for (int i = 1; i < n; ++i) {
        S(i) = S(i-1) + residuals(i);
    }

    // Calculate LM statistic numerator
    double lm_num = S.squaredNorm();

    // Calculate long-run variance using Newey-West estimator
    double s_squared = residuals.squaredNorm() / n;

    for (int i = 1; i <= lag; ++i) {
        double autocovariance = 0.0;
        for (int t = i; t < n; ++t) {
            autocovariance += residuals(t) * residuals(t - i);
        }
        autocovariance /= n;

        double weight = 1.0 - static_cast<double>(i) / (lag + 1);
        s_squared += 2.0 * weight * autocovariance;
    }

    // KPSS test statistic
    double test_stat = lm_num / (n * n * s_squared);

    // Get critical values
    int type_idx = (test_type == KPSSType::LEVEL) ? 0 : 1;
    double cv_1 = KPSS_CRITICAL_VALUES[type_idx][0];
    double cv_5 = KPSS_CRITICAL_VALUES[type_idx][1];
    double cv_10 = KPSS_CRITICAL_VALUES[type_idx][2];

    // Approximate p-value
    double p_value = 0.5;
    if (test_stat > cv_1) {
        p_value = 0.01;
    } else if (test_stat > cv_5) {
        p_value = 0.01 + 0.04 * (cv_1 - test_stat) / (cv_1 - cv_5);
    } else if (test_stat > cv_10) {
        p_value = 0.05 + 0.05 * (cv_5 - test_stat) / (cv_5 - cv_10);
    } else {
        p_value = std::min(0.5, 0.10 + 0.40 * (cv_10 - test_stat) / cv_10);
    }

    KPSSResult result;
    result.test_statistic = test_stat;
    result.p_value = p_value;
    result.lags_used = lag;
    result.n_obs = n;
    result.critical_value_1 = cv_1;
    result.critical_value_5 = cv_5;
    result.critical_value_10 = cv_10;
    result.is_stationary_1 = test_stat < cv_1;
    result.is_stationary_5 = test_stat < cv_5;
    result.is_stationary_10 = test_stat < cv_10;

    return Result<KPSSResult>(result);
}

// KPSS test on Bar vector
Result<KPSSResult> kpss_test(
    const std::vector<Bar>& bars,
    int max_lag,
    KPSSType test_type)
{
    std::vector<double> prices = Normalization::extract_close_prices(bars);
    return kpss_test(prices, max_lag, test_type);
}

// Simple stationarity check
Result<bool> is_stationary(const std::vector<double>& data, double significance) {
    // Run both ADF and KPSS tests
    auto adf_result = augmented_dickey_fuller_test(data);
    if (adf_result.is_error()) {
        return make_error<bool>(
            adf_result.error()->code(),
            adf_result.error()->what(),
            "is_stationary"
        );
    }

    auto kpss_result = kpss_test(data);
    if (kpss_result.is_error()) {
        return make_error<bool>(
            kpss_result.error()->code(),
            kpss_result.error()->what(),
            "is_stationary"
        );
    }

    bool adf_stationary;
    bool kpss_stationary;

    // Determine significance level
    if (std::abs(significance - 0.01) < 1e-6) {
        adf_stationary = adf_result.value().is_stationary_1;
        kpss_stationary = kpss_result.value().is_stationary_1;
    } else if (std::abs(significance - 0.05) < 1e-6) {
        adf_stationary = adf_result.value().is_stationary_5;
        kpss_stationary = kpss_result.value().is_stationary_5;
    } else if (std::abs(significance - 0.10) < 1e-6) {
        adf_stationary = adf_result.value().is_stationary_10;
        kpss_stationary = kpss_result.value().is_stationary_10;
    } else {
        return make_error<bool>(
            ErrorCode::INVALID_ARGUMENT,
            "Significance must be 0.01, 0.05, or 0.10",
            "is_stationary"
        );
    }

    // Series is stationary if ADF rejects null AND KPSS fails to reject null
    return Result<bool>(adf_stationary && kpss_stationary);
}

} // namespace analysis
} // namespace trade_ngin
