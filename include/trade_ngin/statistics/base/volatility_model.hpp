#pragma once
#define TRADE_NGIN_STATISTICS_BASE_VOLATILITY_MODEL_HPP

#include "trade_ngin/core/error.hpp"
#include <vector>

namespace trade_ngin {
namespace statistics {

/**
 * @brief Base class for volatility models
 */
class VolatilityModel {
public:
    virtual ~VolatilityModel() = default;

    /**
     * @brief Fit the volatility model to returns data
     * @param returns Vector of return series
     * @return Result indicating success or failure
     */
    virtual Result<void> fit(const std::vector<double>& returns) = 0;

    /**
     * @brief Forecast volatility for next n periods
     * @param n_periods Number of periods ahead to forecast
     * @return Vector of forecasted volatilities
     */
    virtual Result<std::vector<double>> forecast(int n_periods = 1) const = 0;

    /**
     * @brief Get current conditional volatility
     */
    virtual Result<double> get_current_volatility() const = 0;

    /**
     * @brief Update model with new observation
     * @param new_return New return observation
     */
    virtual Result<void> update(double new_return) = 0;

    /**
     * @brief Check if model is fitted
     */
    virtual bool is_fitted() const = 0;
};

} // namespace statistics
} // namespace trade_ngin
