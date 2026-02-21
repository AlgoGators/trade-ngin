#include "trade_ngin/statistics/volatility/garch.hpp"
#include "trade_ngin/statistics/statistics_utils.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>
#include <limits>

// Define M_PI if not available (Windows)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace trade_ngin {
namespace statistics {

GARCH::GARCH(GARCHConfig config)
    : config_(config)
    , omega_(config.omega)
    , alpha_(config.alpha)
    , beta_(config.beta) {}

Result<void> GARCH::fit(const std::vector<double>& returns) {
    std::lock_guard<std::mutex> lock(mutex_);

    {
        auto valid = validation::validate_time_series(returns, 50, "GARCH");
        if (valid.is_error()) return valid;
    }

    DEBUG("[GARCH::fit] entry: n=" << returns.size()
          << " p=" << config_.p << " q=" << config_.q);

    // Calculate mean return (assume zero for simplicity)
    double mean_return = utils::calculate_mean(returns);

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
    double initial_var = utils::calculate_variance(residuals_, 0.0);
    conditional_variances_[0] = initial_var;

    for (size_t t = 1; t < residuals_.size(); ++t) {
        conditional_variances_[t] = omega_ +
                                   alpha_ * residuals_[t-1] * residuals_[t-1] +
                                   beta_ * conditional_variances_[t-1];
    }

    current_volatility_ = std::sqrt(conditional_variances_.back());
    fitted_ = true;

    if (alpha_ + beta_ > 0.98) {
        WARN("[GARCH::fit] near non-stationarity: alpha+beta=" << (alpha_ + beta_));
    }

    DEBUG("[GARCH::fit] exit: omega=" << omega_ << " alpha=" << alpha_ << " beta=" << beta_);

    return Result<void>();
}

Result<void> GARCH::estimate_parameters(const std::vector<double>& returns) {
    // Simple estimation using method of moments / grid search
    // In practice, use maximum likelihood with optimization library

    double unconditional_var = utils::calculate_variance(returns, 0.0);

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
                    TRACE("[GARCH::estimate_parameters] new best: omega=" << w
                          << " alpha=" << a << " beta=" << b << " ll=" << ll);
                }
            }
        }
    }

    return Result<void>();
}

double GARCH::log_likelihood(const std::vector<double>& returns,
                            double omega, double alpha, double beta) const {
    std::vector<double> var(returns.size());
    double initial_var = utils::calculate_variance(returns, 0.0);
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

} // namespace statistics
} // namespace trade_ngin
