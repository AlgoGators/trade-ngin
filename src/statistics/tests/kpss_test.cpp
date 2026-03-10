#include "trade_ngin/statistics/tests/kpss_test.hpp"
#include "trade_ngin/statistics/critical_values.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>

namespace trade_ngin {
namespace statistics {

KPSSTest::KPSSTest(KPSSTestConfig config)
    : config_(config) {}

Result<TestResult> KPSSTest::test(const std::vector<double>& data) const {
    {
        auto valid = validation::validate_time_series(data, 10, "KPSSTest");
        if (valid.is_error()) {
            return make_error<TestResult>(valid.error()->code(), valid.error()->what(), "KPSSTest");
        }
    }

    int n = static_cast<int>(data.size());
    DEBUG("[KPSSTest::test] entry: n=" << n << " max_lags=" << config_.max_lags);
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
    result.p_value = critical_values::approximate_kpss_p_value(kpss_stat, has_trend);

    if (result.reject_null) {
        result.interpretation = "Reject null hypothesis: Series appears to be non-stationary";
    } else {
        result.interpretation = "Cannot reject null hypothesis: Series appears to be stationary";
    }

    result.additional_stats["lag_order"] = lag_order;
    result.additional_stats["long_run_variance"] = s2;

    DEBUG("[KPSSTest::test] exit: statistic=" << result.statistic
          << " reject_null=" << result.reject_null);

    return Result<TestResult>(std::move(result));
}

int KPSSTest::select_lag_order(int n_obs) const {
    // Hobijn et al. (1998): lag = floor(4 * (n/100)^(1/4))
    return std::max(1, static_cast<int>(std::floor(4.0 * std::pow(n_obs / 100.0, 0.25))));
}

double KPSSTest::calculate_critical_value(double significance, bool has_trend) const {
    return critical_values::kpss_critical_value(significance, has_trend);
}

} // namespace statistics
} // namespace trade_ngin
