#pragma once

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include <vector>

namespace trade_ngin {
namespace analysis {

/**
 * @brief GARCH model fit result
 */
struct GARCHFitResult {
    double omega;           // Constant term in variance equation
    double alpha;           // ARCH coefficient (impact of lagged squared returns)
    double beta;            // GARCH coefficient (persistence of volatility)
    double log_likelihood;  // Log-likelihood of the fitted model
    double aic;             // Akaike Information Criterion
    double bic;             // Bayesian Information Criterion
    std::vector<double> conditional_volatility;  // Fitted conditional volatility (sigma_t)
    std::vector<double> standardized_residuals;  // Standardized residuals
    bool converged;         // Whether optimization converged
};

/**
 * @brief GARCH model forecast result
 */
struct GARCHForecast {
    std::vector<double> volatility_forecast;  // Forecasted volatility
    std::vector<double> variance_forecast;    // Forecasted variance
    int horizon;                              // Forecast horizon
};

/**
 * @brief GARCH(1,1) model for volatility modeling
 *
 * Models conditional heteroskedasticity in financial time series.
 * The GARCH(1,1) model specifies:
 *   r_t = μ + ε_t
 *   ε_t = σ_t * z_t, where z_t ~ N(0,1)
 *   σ_t² = ω + α*ε_{t-1}² + β*σ_{t-1}²
 *
 * Constraints:
 *   - ω > 0
 *   - α ≥ 0, β ≥ 0
 *   - α + β < 1 (stationarity condition)
 */
class GARCH {
public:
    /**
     * @brief Constructor
     * @param p ARCH order (default = 1)
     * @param q GARCH order (default = 1)
     */
    explicit GARCH(int p = 1, int q = 1);

    /**
     * @brief Fit GARCH model to return series
     * @param returns Vector of returns (not prices!)
     * @param max_iterations Maximum optimization iterations
     * @return Fit result with parameters and diagnostics
     */
    Result<GARCHFitResult> fit(
        const std::vector<double>& returns,
        int max_iterations = 1000
    );

    /**
     * @brief Fit GARCH model to price bars (computes log returns internally)
     * @param bars Vector of OHLCV bars
     * @param max_iterations Maximum optimization iterations
     * @return Fit result with parameters and diagnostics
     */
    Result<GARCHFitResult> fit(
        const std::vector<Bar>& bars,
        int max_iterations = 1000
    );

    /**
     * @brief Forecast volatility for h steps ahead
     * @param horizon Number of steps to forecast
     * @return Forecast result
     */
    Result<GARCHForecast> forecast(int horizon);

    /**
     * @brief Update model with new observation (online mode)
     * @param new_return New return observation
     * @return Updated conditional volatility
     */
    Result<double> update(double new_return);

    /**
     * @brief Get current conditional volatility estimate
     * @return Current volatility
     */
    double get_current_volatility() const { return current_volatility_; }

    /**
     * @brief Get current conditional variance estimate
     * @return Current variance
     */
    double get_current_variance() const { return current_variance_; }

    /**
     * @brief Get model parameters
     * @return Vector [omega, alpha, beta]
     */
    std::vector<double> get_parameters() const;

    /**
     * @brief Check if model has been fitted
     * @return True if fit() has been called successfully
     */
    bool is_fitted() const { return fitted_; }

    /**
     * @brief Get unconditional volatility (long-run average)
     * @return Unconditional volatility
     */
    double get_unconditional_volatility() const;

    /**
     * @brief Get half-life of volatility shocks
     * @return Half-life in same units as data
     */
    double get_half_life() const;

private:
    int p_;  // ARCH order
    int q_;  // GARCH order
    bool fitted_;

    // Model parameters
    double omega_;
    double alpha_;
    double beta_;
    double mean_;

    // State variables
    double current_variance_;
    double current_volatility_;
    double last_return_;
    std::vector<double> returns_;

    // Helper functions
    double calculate_log_likelihood(
        const std::vector<double>& returns,
        double omega, double alpha, double beta
    ) const;

    Result<GARCHFitResult> optimize_parameters(
        const std::vector<double>& returns,
        int max_iterations
    );

    std::vector<double> calculate_conditional_variance(
        const std::vector<double>& returns,
        double omega, double alpha, double beta
    ) const;
};

/**
 * @brief Simple function to estimate current volatility using GARCH
 * @param returns Vector of returns
 * @return Current volatility estimate (annualized if returns are daily)
 */
Result<double> estimate_garch_volatility(const std::vector<double>& returns);

} // namespace analysis
} // namespace trade_ngin
