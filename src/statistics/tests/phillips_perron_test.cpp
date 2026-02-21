#include "trade_ngin/statistics/tests/phillips_perron_test.hpp"
#include "trade_ngin/statistics/statistics_utils.hpp"
#include "trade_ngin/statistics/critical_values.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>

namespace trade_ngin {
namespace statistics {

PhillipsPerronTest::PhillipsPerronTest(PhillipsPerronConfig config)
    : config_(config) {}

int PhillipsPerronTest::select_bandwidth(int n_obs) const {
    if (config_.bandwidth > 0) return config_.bandwidth;
    // Newey-West automatic bandwidth: floor(4 * (n/100)^(2/9))
    return std::max(1, static_cast<int>(std::floor(4.0 * std::pow(n_obs / 100.0, 2.0 / 9.0))));
}

double PhillipsPerronTest::kernel_weight(int lag, int bandwidth) const {
    double x = static_cast<double>(lag) / (bandwidth + 1);
    switch (config_.kernel) {
        case PhillipsPerronConfig::KernelType::BARTLETT:
            return (x <= 1.0) ? (1.0 - x) : 0.0;
        case PhillipsPerronConfig::KernelType::PARZEN:
            if (x <= 0.5) return 1.0 - 6.0 * x * x + 6.0 * x * x * x;
            if (x <= 1.0) return 2.0 * (1.0 - x) * (1.0 - x) * (1.0 - x);
            return 0.0;
        case PhillipsPerronConfig::KernelType::QUADRATIC_SPECTRAL: {
            if (lag == 0) return 1.0;
            double y = 6.0 * M_PI * x / 5.0;
            return 25.0 / (12.0 * M_PI * M_PI * x * x) *
                   (std::sin(y) / y - std::cos(y));
        }
    }
    return 0.0;
}

Result<TestResult> PhillipsPerronTest::test(const std::vector<double>& data) const {
    {
        auto valid = validation::validate_time_series(data, 10, "PhillipsPerronTest");
        if (valid.is_error()) {
            return make_error<TestResult>(valid.error()->code(), valid.error()->what(), "PhillipsPerronTest");
        }
    }

    int n = data.size();
    DEBUG("[PhillipsPerronTest::test] entry: n=" << n);

    // Step 1: OLS regression Δy_t = α + γ*y_{t-1} + ε_t (or with trend)
    std::vector<double> y_diff = utils::difference(data, 1);
    int T = y_diff.size();

    int n_regressors = 1; // y_{t-1}
    if (config_.regression == PhillipsPerronConfig::RegressionType::CONSTANT ||
        config_.regression == PhillipsPerronConfig::RegressionType::CONSTANT_TREND) {
        n_regressors++;
    }
    if (config_.regression == PhillipsPerronConfig::RegressionType::CONSTANT_TREND) {
        n_regressors++;
    }

    Eigen::MatrixXd X(T, n_regressors);
    Eigen::VectorXd y(T);

    for (int t = 0; t < T; ++t) {
        y(t) = y_diff[t];
        int col = 0;

        if (config_.regression != PhillipsPerronConfig::RegressionType::NO_CONSTANT) {
            X(t, col++) = 1.0;
        }
        if (config_.regression == PhillipsPerronConfig::RegressionType::CONSTANT_TREND) {
            X(t, col++) = t + 1;
        }
        X(t, col++) = data[t]; // y_{t-1}
    }

    // OLS
    Eigen::LDLT<Eigen::MatrixXd> ldlt = (X.transpose() * X).ldlt();
    Eigen::VectorXd beta = ldlt.solve(X.transpose() * y);
    Eigen::VectorXd residuals = y - X * beta;

    int gamma_idx = n_regressors - 1; // Last column is y_{t-1}
    double gamma_hat = beta(gamma_idx);

    double sigma2 = residuals.squaredNorm() / T;

    // Standard error of gamma via (X'X)^{-1}
    Eigen::VectorXd e_gamma = Eigen::VectorXd::Zero(n_regressors);
    e_gamma(gamma_idx) = 1.0;
    double se_gamma = std::sqrt(sigma2 * ldlt.solve(e_gamma)(gamma_idx));

    double t_stat = gamma_hat / se_gamma;

    // Step 2: Non-parametric long-run variance correction
    int bw = select_bandwidth(T);

    // Autocovariances of residuals
    double lambda2 = 0.0; // Long-run variance of residuals
    double s2_resid = residuals.squaredNorm() / T; // Short-run variance

    for (int lag = 1; lag <= bw; ++lag) {
        double gamma_l = 0.0;
        for (int t = lag; t < T; ++t) {
            gamma_l += residuals(t) * residuals(t - lag);
        }
        gamma_l /= T;
        double w = kernel_weight(lag, bw);
        lambda2 += 2.0 * w * gamma_l;
    }
    lambda2 += s2_resid;

    // Phillips-Perron adjusted t-statistic (Z_t)
    double correction = (lambda2 - s2_resid) * T * se_gamma / (2.0 * std::sqrt(lambda2));
    double pp_stat = t_stat * std::sqrt(s2_resid / lambda2) - correction / std::sqrt(lambda2);

    // Critical values (same distribution as ADF)
    int reg_type;
    switch (config_.regression) {
        case PhillipsPerronConfig::RegressionType::NO_CONSTANT: reg_type = 0; break;
        case PhillipsPerronConfig::RegressionType::CONSTANT_TREND: reg_type = 2; break;
        default: reg_type = 1; break;
    }
    double critical_val = critical_values::interpolate_adf_cv(T, reg_type, config_.significance_level);

    TestResult result;
    result.statistic = pp_stat;
    result.critical_value = critical_val;
    result.reject_null = pp_stat < critical_val;
    result.p_value = critical_values::approximate_adf_p_value(pp_stat, T, reg_type);

    if (result.reject_null) {
        result.interpretation = "Reject null hypothesis: Series appears to be stationary";
    } else {
        result.interpretation = "Cannot reject null hypothesis: Series appears to have a unit root (non-stationary)";
    }

    result.additional_stats["bandwidth"] = bw;
    result.additional_stats["short_run_variance"] = s2_resid;
    result.additional_stats["long_run_variance"] = lambda2;
    result.additional_stats["ols_t_stat"] = t_stat;

    DEBUG("[PhillipsPerronTest::test] exit: pp_stat=" << pp_stat
          << " reject_null=" << result.reject_null);

    return Result<TestResult>(std::move(result));
}

} // namespace statistics
} // namespace trade_ngin
