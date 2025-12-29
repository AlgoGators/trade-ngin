#include "trade_ngin/analysis/garch.hpp"
#include "trade_ngin/analysis/preprocessing.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <limits>

namespace trade_ngin {
namespace analysis {

namespace {
    // Helper: Calculate sample variance
    double calculate_variance(const std::vector<double>& data) {
        double mean = std::accumulate(data.begin(), data.end(), 0.0) / data.size();
        double variance = 0.0;
        for (double val : data) {
            variance += (val - mean) * (val - mean);
        }
        return variance / data.size();
    }

    // Helper: Nelder-Mead simplex optimization
    struct SimplexPoint {
        std::vector<double> params;
        double value;
    };

    class NelderMead {
    public:
        using ObjectiveFunc = std::function<double(const std::vector<double>&)>;

        static std::vector<double> optimize(
            ObjectiveFunc func,
            std::vector<double> initial,
            int max_iterations,
            double tolerance = 1e-6
        ) {
            int n = initial.size();
            std::vector<SimplexPoint> simplex(n + 1);

            // Initialize simplex
            simplex[0].params = initial;
            simplex[0].value = func(initial);

            for (int i = 1; i <= n; ++i) {
                simplex[i].params = initial;
                simplex[i].params[i-1] += 0.1; // Perturbation
                simplex[i].value = func(simplex[i].params);
            }

            // Optimization loop
            for (int iter = 0; iter < max_iterations; ++iter) {
                // Sort simplex by objective value
                std::sort(simplex.begin(), simplex.end(),
                         [](const SimplexPoint& a, const SimplexPoint& b) {
                             return a.value < b.value;
                         });

                // Check convergence
                double range = simplex[n].value - simplex[0].value;
                if (range < tolerance) {
                    break;
                }

                // Calculate centroid (excluding worst point)
                std::vector<double> centroid(n, 0.0);
                for (int i = 0; i < n; ++i) {
                    for (int j = 0; j < n; ++j) {
                        centroid[j] += simplex[i].params[j];
                    }
                }
                for (int j = 0; j < n; ++j) {
                    centroid[j] /= n;
                }

                // Reflection
                std::vector<double> reflected(n);
                for (int j = 0; j < n; ++j) {
                    reflected[j] = centroid[j] + 1.0 * (centroid[j] - simplex[n].params[j]);
                }
                double reflected_value = func(reflected);

                if (reflected_value < simplex[0].value) {
                    // Expansion
                    std::vector<double> expanded(n);
                    for (int j = 0; j < n; ++j) {
                        expanded[j] = centroid[j] + 2.0 * (reflected[j] - centroid[j]);
                    }
                    double expanded_value = func(expanded);

                    if (expanded_value < reflected_value) {
                        simplex[n].params = expanded;
                        simplex[n].value = expanded_value;
                    } else {
                        simplex[n].params = reflected;
                        simplex[n].value = reflected_value;
                    }
                } else if (reflected_value < simplex[n-1].value) {
                    simplex[n].params = reflected;
                    simplex[n].value = reflected_value;
                } else {
                    // Contraction
                    std::vector<double> contracted(n);
                    for (int j = 0; j < n; ++j) {
                        contracted[j] = centroid[j] + 0.5 * (simplex[n].params[j] - centroid[j]);
                    }
                    double contracted_value = func(contracted);

                    if (contracted_value < simplex[n].value) {
                        simplex[n].params = contracted;
                        simplex[n].value = contracted_value;
                    } else {
                        // Shrink
                        for (int i = 1; i <= n; ++i) {
                            for (int j = 0; j < n; ++j) {
                                simplex[i].params[j] = simplex[0].params[j] +
                                    0.5 * (simplex[i].params[j] - simplex[0].params[j]);
                            }
                            simplex[i].value = func(simplex[i].params);
                        }
                    }
                }
            }

            std::sort(simplex.begin(), simplex.end(),
                     [](const SimplexPoint& a, const SimplexPoint& b) {
                         return a.value < b.value;
                     });

            return simplex[0].params;
        }
    };
}

GARCH::GARCH(int p, int q)
    : p_(p)
    , q_(q)
    , fitted_(false)
    , omega_(0.0)
    , alpha_(0.0)
    , beta_(0.0)
    , mean_(0.0)
    , current_variance_(0.0)
    , current_volatility_(0.0)
    , last_return_(0.0) {
}

std::vector<double> GARCH::calculate_conditional_variance(
    const std::vector<double>& returns,
    double omega, double alpha, double beta
) const {
    int n = returns.size();
    std::vector<double> variance(n);

    // Initialize with unconditional variance
    double sample_var = calculate_variance(returns);
    variance[0] = sample_var;

    // Recursively calculate conditional variance
    for (int t = 1; t < n; ++t) {
        variance[t] = omega + alpha * returns[t-1] * returns[t-1] + beta * variance[t-1];

        // Ensure positive variance
        variance[t] = std::max(variance[t], 1e-8);
    }

    return variance;
}

double GARCH::calculate_log_likelihood(
    const std::vector<double>& returns,
    double omega, double alpha, double beta
) const {
    // Parameter constraints
    if (omega <= 0 || alpha < 0 || beta < 0 || alpha + beta >= 1.0) {
        return std::numeric_limits<double>::max();
    }

    std::vector<double> variance = calculate_conditional_variance(returns, omega, alpha, beta);

    double log_likelihood = 0.0;
    const double LOG_2PI = 1.83787706640935;

    for (size_t t = 0; t < returns.size(); ++t) {
        double sigma_t = std::sqrt(variance[t]);
        double z_t = returns[t] / sigma_t;

        log_likelihood += -0.5 * (LOG_2PI + std::log(variance[t]) + z_t * z_t);
    }

    // Return negative log-likelihood for minimization
    return -log_likelihood;
}

Result<GARCHFitResult> GARCH::optimize_parameters(
    const std::vector<double>& returns,
    int max_iterations
) {
    // Initial parameter guess
    double sample_var = calculate_variance(returns);
    std::vector<double> initial = {
        0.01 * sample_var,  // omega
        0.10,               // alpha
        0.80                // beta
    };

    // Define objective function
    auto objective = [this, &returns](const std::vector<double>& params) {
        return calculate_log_likelihood(returns, params[0], params[1], params[2]);
    };

    // Optimize using Nelder-Mead
    std::vector<double> optimal = NelderMead::optimize(
        objective,
        initial,
        max_iterations
    );

    // Extract parameters
    omega_ = optimal[0];
    alpha_ = optimal[1];
    beta_ = optimal[2];

    // Calculate fit statistics
    std::vector<double> variance = calculate_conditional_variance(returns, omega_, alpha_, beta_);
    std::vector<double> volatility(variance.size());
    std::vector<double> std_residuals(variance.size());

    for (size_t t = 0; t < variance.size(); ++t) {
        volatility[t] = std::sqrt(variance[t]);
        std_residuals[t] = returns[t] / volatility[t];
    }

    double log_likelihood = -calculate_log_likelihood(returns, omega_, alpha_, beta_);
    int n = returns.size();
    int k = 3; // Number of parameters

    GARCHFitResult result;
    result.omega = omega_;
    result.alpha = alpha_;
    result.beta = beta_;
    result.log_likelihood = log_likelihood;
    result.aic = -2 * log_likelihood + 2 * k;
    result.bic = -2 * log_likelihood + k * std::log(n);
    result.conditional_volatility = volatility;
    result.standardized_residuals = std_residuals;
    result.converged = true;

    // Update state variables
    current_variance_ = variance.back();
    current_volatility_ = volatility.back();
    last_return_ = returns.back();

    return Result<GARCHFitResult>(result);
}

Result<GARCHFitResult> GARCH::fit(
    const std::vector<double>& returns,
    int max_iterations
) {
    if (returns.size() < 50) {
        return make_error<GARCHFitResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Need at least 50 observations for GARCH model",
            "GARCH::fit"
        );
    }

    // Store returns
    returns_ = returns;

    // Calculate mean
    mean_ = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();

    // Demean returns
    std::vector<double> demeaned_returns(returns.size());
    for (size_t i = 0; i < returns.size(); ++i) {
        demeaned_returns[i] = returns[i] - mean_;
    }

    // Optimize parameters
    auto result = optimize_parameters(demeaned_returns, max_iterations);

    if (result) {
        fitted_ = true;
    }

    return result;
}

Result<GARCHFitResult> GARCH::fit(
    const std::vector<Bar>& bars,
    int max_iterations
) {
    if (bars.size() < 51) {
        return make_error<GARCHFitResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Need at least 51 bars for GARCH model",
            "GARCH::fit"
        );
    }

    // Extract prices and calculate log returns
    std::vector<double> prices = Normalization::extract_close_prices(bars);
    auto returns_result = Normalization::calculate_returns(prices, true);

    if (!returns_result) {
        return make_error<GARCHFitResult>(
            returns_result.error().code,
            returns_result.error().message,
            "GARCH::fit"
        );
    }

    return fit(returns_result.value(), max_iterations);
}

Result<GARCHForecast> GARCH::forecast(int horizon) {
    if (!fitted_) {
        return make_error<GARCHForecast>(
            ErrorCode::INVALID_STATE,
            "GARCH model must be fitted before forecasting",
            "GARCH::forecast"
        );
    }

    if (horizon <= 0) {
        return make_error<GARCHForecast>(
            ErrorCode::INVALID_ARGUMENT,
            "Forecast horizon must be positive",
            "GARCH::forecast"
        );
    }

    GARCHForecast forecast;
    forecast.horizon = horizon;
    forecast.variance_forecast.resize(horizon);
    forecast.volatility_forecast.resize(horizon);

    // Calculate unconditional variance
    double uncond_var = omega_ / (1.0 - alpha_ - beta_);

    // Forecast variance recursively
    forecast.variance_forecast[0] = omega_ + (alpha_ + beta_) * current_variance_;

    for (int h = 1; h < horizon; ++h) {
        double weight = std::pow(alpha_ + beta_, h);
        forecast.variance_forecast[h] = uncond_var * (1.0 - weight) +
                                        weight * current_variance_;
    }

    // Convert to volatility
    for (int h = 0; h < horizon; ++h) {
        forecast.volatility_forecast[h] = std::sqrt(forecast.variance_forecast[h]);
    }

    return Result<GARCHForecast>(forecast);
}

Result<double> GARCH::update(double new_return) {
    if (!fitted_) {
        return make_error<double>(
            ErrorCode::INVALID_STATE,
            "GARCH model must be fitted before updating",
            "GARCH::update"
        );
    }

    // Update conditional variance
    double demeaned_return = new_return - mean_;
    current_variance_ = omega_ + alpha_ * last_return_ * last_return_ +
                       beta_ * current_variance_;
    current_variance_ = std::max(current_variance_, 1e-8);
    current_volatility_ = std::sqrt(current_variance_);

    last_return_ = demeaned_return;

    return Result<double>(current_volatility_);
}

std::vector<double> GARCH::get_parameters() const {
    return {omega_, alpha_, beta_};
}

double GARCH::get_unconditional_volatility() const {
    if (!fitted_) return 0.0;
    double uncond_var = omega_ / (1.0 - alpha_ - beta_);
    return std::sqrt(uncond_var);
}

double GARCH::get_half_life() const {
    if (!fitted_) return 0.0;
    double persistence = alpha_ + beta_;
    return std::log(0.5) / std::log(persistence);
}

// Simple volatility estimation function
Result<double> estimate_garch_volatility(const std::vector<double>& returns) {
    GARCH model;
    auto fit_result = model.fit(returns);

    if (!fit_result) {
        return make_error<double>(
            fit_result.error().code,
            fit_result.error().message,
            "estimate_garch_volatility"
        );
    }

    return Result<double>(model.get_current_volatility());
}

} // namespace analysis
} // namespace trade_ngin
