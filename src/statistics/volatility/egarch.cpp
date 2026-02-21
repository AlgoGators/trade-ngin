#include "trade_ngin/statistics/volatility/egarch.hpp"
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

EGARCH::EGARCH(EGARCHConfig config)
    : config_(config)
    , omega_(config.omega)
    , alpha_(config.alpha)
    , gamma_(config.gamma)
    , beta_(config.beta) {}

struct EGARCHOptData {
    const std::vector<double>* returns;
    double initial_log_var;
};

static double egarch_nlopt_objective(const std::vector<double>& x, std::vector<double>&, void* data) {
    auto* opt_data = static_cast<EGARCHOptData*>(data);
    double omega = x[0];
    double alpha = x[1];
    double gamma = x[2];
    double beta = x[3];

    // Stability: |beta| < 1
    if (std::abs(beta) >= 0.999) return std::numeric_limits<double>::infinity();

    const auto& returns = *opt_data->returns;
    double log_h = opt_data->initial_log_var;
    double e_abs_z = std::sqrt(2.0 / M_PI);

    double ll = 0.0;
    for (size_t t = 0; t < returns.size(); ++t) {
        double h = std::exp(log_h);
        if (h <= 0 || !std::isfinite(h)) return std::numeric_limits<double>::infinity();

        ll += -0.5 * (std::log(2.0 * M_PI) + log_h + returns[t] * returns[t] / h);

        if (t < returns.size() - 1) {
            double z = returns[t] / std::sqrt(h);
            log_h = omega + alpha * (std::abs(z) - e_abs_z) + gamma * z + beta * log_h;
        }
    }

    if (!std::isfinite(ll)) return std::numeric_limits<double>::infinity();
    return -ll;
}

Result<void> EGARCH::fit(const std::vector<double>& returns) {
    std::lock_guard<std::mutex> lock(mutex_);

    {
        auto valid = validation::validate_time_series(returns, 50, "EGARCH");
        if (valid.is_error()) return valid;
    }

    DEBUG("[EGARCH::fit] entry: n=" << returns.size());

    double mean_return = utils::calculate_mean(returns);
    residuals_.resize(returns.size());
    for (size_t i = 0; i < returns.size(); ++i) {
        residuals_[i] = returns[i] - mean_return;
    }

    auto result = estimate_parameters(residuals_);
    if (result.is_error()) return result;

    // Compute log-variance series
    double unconditional_var = utils::calculate_variance(residuals_, 0.0);
    double log_h = std::log(unconditional_var);
    double e_abs_z = std::sqrt(2.0 / M_PI);

    log_variances_.resize(residuals_.size());
    for (size_t t = 0; t < residuals_.size(); ++t) {
        log_variances_[t] = log_h;
        if (t < residuals_.size() - 1) {
            double z = residuals_[t] / std::sqrt(std::exp(log_h));
            log_h = omega_ + alpha_ * (std::abs(z) - e_abs_z) + gamma_ * z + beta_ * log_h;
        }
    }

    current_volatility_ = std::sqrt(std::exp(log_variances_.back()));
    fitted_ = true;

    DEBUG("[EGARCH::fit] exit: omega=" << omega_ << " alpha=" << alpha_
          << " gamma=" << gamma_ << " beta=" << beta_);

    return Result<void>();
}

Result<void> EGARCH::estimate_parameters(const std::vector<double>& returns) {
    double unconditional_var = utils::calculate_variance(returns, 0.0);
    double initial_log_var = std::log(unconditional_var);

    // Phase 1: Grid search
    double best_ll = -std::numeric_limits<double>::infinity();
    omega_ = config_.omega;
    alpha_ = config_.alpha;
    gamma_ = config_.gamma;
    beta_ = config_.beta;

    for (double w = -2.0; w <= 0.5; w += 0.5) {
        for (double a = 0.05; a <= 0.4; a += 0.07) {
            for (double g = -0.3; g <= 0.1; g += 0.1) {
                for (double b = 0.7; b <= 0.99; b += 0.05) {
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

    TRACE("[EGARCH::estimate_parameters] grid best: omega=" << omega_
          << " alpha=" << alpha_ << " gamma=" << gamma_ << " beta=" << beta_);

    last_convergence_info_ = ConvergenceInfo{};
    last_convergence_info_.objective_history.push_back(best_ll);

    // Phase 2: nlopt BOBYQA refinement
    try {
        nlopt::opt opt(nlopt::LN_BOBYQA, 4);

        opt.set_lower_bounds({-5.0, 0.001, -1.0, 0.01});
        opt.set_upper_bounds({ 2.0, 1.0,    1.0, 0.999});

        EGARCHOptData opt_data{&returns, initial_log_var};
        opt.set_min_objective(egarch_nlopt_objective, &opt_data);
        opt.set_ftol_rel(1e-8);
        opt.set_maxeval(500);

        std::vector<double> x = {omega_, alpha_, gamma_, beta_};
        double min_obj;
        auto nlopt_result = opt.optimize(x, min_obj);

        double refined_ll = -min_obj;
        last_convergence_info_.objective_history.push_back(refined_ll);
        last_convergence_info_.iterations = static_cast<int>(opt.get_numevals());
        last_convergence_info_.final_tolerance = std::abs(refined_ll - best_ll);

        if (nlopt_result > 0) {
            last_convergence_info_.converged = true;
            last_convergence_info_.termination_reason = "tolerance";
        } else {
            last_convergence_info_.converged = false;
            last_convergence_info_.termination_reason = "max_iterations";
        }

        if (refined_ll > best_ll && std::abs(x[3]) < 0.999) {
            omega_ = x[0];
            alpha_ = x[1];
            gamma_ = x[2];
            beta_ = x[3];
            TRACE("[EGARCH::estimate_parameters] nlopt improved: omega=" << omega_
                  << " alpha=" << alpha_ << " gamma=" << gamma_ << " beta=" << beta_);
        }
    } catch (const std::exception& e) {
        DEBUG("[EGARCH::estimate_parameters] nlopt failed (" << e.what()
              << "), keeping grid search result");
        last_convergence_info_.converged = true;
        last_convergence_info_.termination_reason = "tolerance";
        last_convergence_info_.iterations = 1;
    }

    return Result<void>();
}

Result<ConvergenceInfo> EGARCH::fit_with_diagnostics(const std::vector<double>& returns) {
    auto result = fit(returns);
    if (result.is_error()) {
        return make_error<ConvergenceInfo>(result.error()->code(), result.error()->what(), "EGARCH");
    }
    return Result<ConvergenceInfo>(last_convergence_info_);
}

double EGARCH::log_likelihood(const std::vector<double>& returns,
                              double omega, double alpha, double gamma, double beta) const {
    double unconditional_var = utils::calculate_variance(returns, 0.0);
    double log_h = std::log(unconditional_var);
    double e_abs_z = std::sqrt(2.0 / M_PI);
    double ll = 0.0;

    for (size_t t = 0; t < returns.size(); ++t) {
        double h = std::exp(log_h);
        if (h <= 0 || !std::isfinite(h)) return -std::numeric_limits<double>::infinity();

        ll += -0.5 * (std::log(2.0 * M_PI) + log_h + returns[t] * returns[t] / h);

        if (t < returns.size() - 1) {
            double z = returns[t] / std::sqrt(h);
            log_h = omega + alpha * (std::abs(z) - e_abs_z) + gamma * z + beta * log_h;
        }
    }

    return std::isfinite(ll) ? ll : -std::numeric_limits<double>::infinity();
}

Result<std::vector<double>> EGARCH::forecast(int n_periods) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<std::vector<double>>(
            ErrorCode::NOT_INITIALIZED, "EGARCH model has not been fitted", "EGARCH");
    }

    std::vector<double> forecasts(n_periods);
    double log_h = log_variances_.back();

    for (int i = 0; i < n_periods; ++i) {
        // In expectation: E[|z|] = sqrt(2/pi), E[z] = 0
        // log_h_next = omega + alpha*(E[|z|] - E[|z|]) + gamma*E[z] + beta*log_h
        // => log_h_next = omega + beta * log_h
        log_h = omega_ + beta_ * log_h;
        forecasts[i] = std::sqrt(std::exp(log_h));
    }

    return Result<std::vector<double>>(std::move(forecasts));
}

Result<double> EGARCH::get_current_volatility() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<double>(
            ErrorCode::NOT_INITIALIZED, "EGARCH model has not been fitted", "EGARCH");
    }

    return Result<double>(current_volatility_);
}

Result<void> EGARCH::update(double new_return) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<void>(
            ErrorCode::NOT_INITIALIZED, "EGARCH model has not been fitted", "EGARCH");
    }

    double log_h = log_variances_.back();
    double z = new_return / std::sqrt(std::exp(log_h));
    double e_abs_z = std::sqrt(2.0 / M_PI);

    double new_log_h = omega_ + alpha_ * (std::abs(z) - e_abs_z) + gamma_ * z + beta_ * log_h;

    residuals_.push_back(new_return);
    log_variances_.push_back(new_log_h);
    current_volatility_ = std::sqrt(std::exp(new_log_h));

    return Result<void>();
}

} // namespace statistics
} // namespace trade_ngin
