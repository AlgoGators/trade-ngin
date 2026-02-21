#include "trade_ngin/statistics/tests/adf_test.hpp"
#include "trade_ngin/statistics/statistics_utils.hpp"
#include "trade_ngin/statistics/critical_values.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>

namespace trade_ngin {
namespace statistics {

ADFTest::ADFTest(ADFTestConfig config)
    : config_(config) {}

Result<TestResult> ADFTest::test(const std::vector<double>& data) const {
    {
        auto valid = validation::validate_time_series(data, 10, "ADFTest");
        if (valid.is_error()) {
            return make_error<TestResult>(valid.error()->code(), valid.error()->what(), "ADFTest");
        }
    }

    int n = data.size();
    DEBUG("[ADFTest::test] entry: n=" << n << " max_lags=" << config_.max_lags);
    int lag_order = config_.max_lags;
    if (lag_order < 0) {
        lag_order = select_lag_order(data);
    }

    // Prepare regression: Δy_t = α + βt + γy_{t-1} + Σδ_i Δy_{t-i} + ε_t
    std::vector<double> y_diff = utils::difference(data, 1);
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

    DEBUG("[ADFTest::test] exit: statistic=" << result.statistic
          << " reject_null=" << result.reject_null);

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

} // namespace statistics
} // namespace trade_ngin
