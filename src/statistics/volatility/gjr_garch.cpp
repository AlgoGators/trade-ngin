#include "trade_ngin/statistics/volatility/gjr_garch.hpp"
#include "trade_ngin/statistics/statistics_utils.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"
#include <nlopt.hpp>
#include <cmath>
#include <limits>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace trade_ngin {
namespace statistics {

GJRGARCH::GJRGARCH(GJRGARCHConfig config)
    : config_(config)
    , omega_(config.omega)
    , alpha_(config.alpha)
    , gamma_(config.gamma)
    , beta_(config.beta) {}

struct GJRGARCHOptData {
    const std::vector<double>* returns;
    double initial_var;
};

static double gjr_garch_nlopt_objective(const std::vector<double>& x, std::vector<double>&, void* data) {
    auto* opt_data = static_cast<GJRGARCHOptData*>(data);
    double omega = x[0];
    double alpha = x[1];
    double gamma = x[2];
    double beta = x[3];

    // Stationarity: alpha + gamma/2 + beta < 1
    if (alpha + gamma / 2.0 + beta >= 0.995) {
        return std::numeric_limits<double>::infinity();
    }

    const auto& returns = *opt_data->returns;
    double var = opt_data->initial_var;
    double ll = -0.5 * (std::log(2.0 * M_PI) + std::log(var) +
                       returns[0] * returns[0] / var);

    for (size_t t = 1; t < returns.size(); ++t) {
        double indicator = (returns[t - 1] < 0) ? 1.0 : 0.0;
        var = omega + alpha * returns[t - 1] * returns[t - 1]
              + gamma * indicator * returns[t - 1] * returns[t - 1]
              + beta * var;
        if (var <= 0) return std::numeric_limits<double>::infinity();
        ll += -0.5 * (std::log(2.0 * M_PI) + std::log(var) +
                     returns[t] * returns[t] / var);
    }

    if (!std::isfinite(ll)) return std::numeric_limits<double>::infinity();
    return -ll;
}

Result<void> GJRGARCH::fit(const std::vector<double>& returns) {
    std::lock_guard<std::mutex> lock(mutex_);

    {
        auto valid = validation::validate_time_series(returns, 50, "GJRGARCH");
        if (valid.is_error()) return valid;
    }

    DEBUG("[GJRGARCH::fit] entry: n=" << returns.size());

    double mean_return = utils::calculate_mean(returns);
    residuals_.resize(returns.size());
    for (size_t i = 0; i < returns.size(); ++i) {
        residuals_[i] = returns[i] - mean_return;
    }

    auto result = estimate_parameters(residuals_);
    if (result.is_error()) return result;

    // Compute conditional variances
    conditional_variances_.resize(residuals_.size());
    double initial_var = utils::calculate_variance(residuals_, 0.0);
    conditional_variances_[0] = initial_var;

    for (size_t t = 1; t < residuals_.size(); ++t) {
        double indicator = (residuals_[t - 1] < 0) ? 1.0 : 0.0;
        conditional_variances_[t] = omega_
            + alpha_ * residuals_[t - 1] * residuals_[t - 1]
            + gamma_ * indicator * residuals_[t - 1] * residuals_[t - 1]
            + beta_ * conditional_variances_[t - 1];
    }

    current_volatility_ = std::sqrt(conditional_variances_.back());
    fitted_ = true;

    DEBUG("[GJRGARCH::fit] exit: omega=" << omega_ << " alpha=" << alpha_
          << " gamma=" << gamma_ << " beta=" << beta_);

    return Result<void>();
}

Result<void> GJRGARCH::estimate_parameters(const std::vector<double>& returns) {
    double unconditional_var = utils::calculate_variance(returns, 0.0);

    // Phase 1: Grid search
    omega_ = config_.omega;
    alpha_ = config_.alpha;
    gamma_ = config_.gamma;
    beta_ = config_.beta;

    double best_ll = log_likelihood(returns, omega_, alpha_, gamma_, beta_);

    for (double a = 0.01; a <= 0.3; a += 0.05) {
        for (double g = 0.0; g <= 0.4; g += 0.05) {
            for (double b = 0.3; b <= 0.97; b += 0.07) {
                if (a + g / 2.0 + b < 0.995) {
                    double w = unconditional_var * (1.0 - a - g / 2.0 - b);
                    if (w > 0) {
                        double ll = log_likelihood(returns, w, a, g, b);
                        if (ll > best_ll) {
                            best_ll = ll;
                            omega_ = w;
                            alpha_ = a;
                            gamma_ = g;
                            beta_ = b;
                        }
                    }
                }
            }
        }
    }

    TRACE("[GJRGARCH::estimate_parameters] grid best: omega=" << omega_
          << " alpha=" << alpha_ << " gamma=" << gamma_ << " beta=" << beta_);

    // Phase 2: nlopt BOBYQA
    try {
        nlopt::opt opt(nlopt::LN_BOBYQA, 4);

        opt.set_lower_bounds({1e-8, 0.001, 0.0, 0.01});
        opt.set_upper_bounds({unconditional_var * 10.0, 0.5, 0.5, 0.99});

        double initial_var = utils::calculate_variance(returns, 0.0);
        GJRGARCHOptData opt_data{&returns, initial_var};
        opt.set_min_objective(gjr_garch_nlopt_objective, &opt_data);
        opt.set_ftol_rel(1e-8);
        opt.set_maxeval(500);

        std::vector<double> x = {omega_, alpha_, gamma_, beta_};
        double min_obj;
        opt.optimize(x, min_obj);

        double refined_ll = -min_obj;
        if (refined_ll > best_ll && x[1] + x[2] / 2.0 + x[3] < 0.995 && x[0] > 0) {
            omega_ = x[0];
            alpha_ = x[1];
            gamma_ = x[2];
            beta_ = x[3];
        }
    } catch (const std::exception& e) {
        DEBUG("[GJRGARCH::estimate_parameters] nlopt failed (" << e.what()
              << "), keeping grid search result");
    }

    return Result<void>();
}

double GJRGARCH::log_likelihood(const std::vector<double>& returns,
                                double omega, double alpha, double gamma, double beta) const {
    double initial_var = utils::calculate_variance(returns, 0.0);
    double var = initial_var;

    double ll = -0.5 * (std::log(2.0 * M_PI) + std::log(var) +
                       returns[0] * returns[0] / var);

    for (size_t t = 1; t < returns.size(); ++t) {
        double indicator = (returns[t - 1] < 0) ? 1.0 : 0.0;
        var = omega + alpha * returns[t - 1] * returns[t - 1]
              + gamma * indicator * returns[t - 1] * returns[t - 1]
              + beta * var;
        if (var <= 0) return -std::numeric_limits<double>::infinity();
        ll += -0.5 * (std::log(2.0 * M_PI) + std::log(var) +
                     returns[t] * returns[t] / var);
    }

    return std::isfinite(ll) ? ll : -std::numeric_limits<double>::infinity();
}

Result<std::vector<double>> GJRGARCH::forecast(int n_periods) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<std::vector<double>>(
            ErrorCode::NOT_INITIALIZED, "GJR-GARCH model has not been fitted", "GJRGARCH");
    }

    std::vector<double> forecasts(n_periods);
    double h = conditional_variances_.back();

    for (int i = 0; i < n_periods; ++i) {
        // E[I_t * ε²_t] = 0.5 * h (assuming symmetric distribution)
        h = omega_ + (alpha_ + gamma_ / 2.0 + beta_) * h;
        forecasts[i] = std::sqrt(h);
    }

    return Result<std::vector<double>>(std::move(forecasts));
}

Result<double> GJRGARCH::get_current_volatility() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<double>(
            ErrorCode::NOT_INITIALIZED, "GJR-GARCH model has not been fitted", "GJRGARCH");
    }

    return Result<double>(current_volatility_);
}

Result<void> GJRGARCH::update(double new_return) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<void>(
            ErrorCode::NOT_INITIALIZED, "GJR-GARCH model has not been fitted", "GJRGARCH");
    }

    double indicator = (new_return < 0) ? 1.0 : 0.0;
    double new_var = omega_
        + alpha_ * new_return * new_return
        + gamma_ * indicator * new_return * new_return
        + beta_ * conditional_variances_.back();

    residuals_.push_back(new_return);
    conditional_variances_.push_back(new_var);
    current_volatility_ = std::sqrt(new_var);

    return Result<void>();
}

} // namespace statistics
} // namespace trade_ngin
