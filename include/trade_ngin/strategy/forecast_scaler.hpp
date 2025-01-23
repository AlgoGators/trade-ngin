// include/trade_ngin/strategy/forecast_scaler.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include <vector>
#include <unordered_map>
#include <deque>

namespace trade_ngin {

/**
 * @brief Configuration for forecast scaling
 */
struct ForecastScalerConfig {
    int volatility_lookback{252};     // Lookback period for volatility history
    double ewma_decay{10.0};          // Decay factor for EWMA smoothing
    double base_scalar_trend{30.0};   // Base forecast scalar for trend
    double base_scalar_carry{23.0};   // Base forecast scalar for carry
    double forecast_cap{20.0};        // Maximum absolute forecast value
};

/**
 * @brief Type of trading forecast
 */
enum class ForecastType {
    TREND,
    CARRY
};

/**
 * @brief Volatility-based forecast scaler
 */
class ForecastScaler {
public:
    explicit ForecastScaler(ForecastScalerConfig config);

    /**
     * @brief Update volatility history for a symbol
     * @param symbol Instrument symbol
     * @param volatility New volatility measure
     */
    void update_volatility(const std::string& symbol, double volatility);

    /**
     * @brief Scale a raw forecast based on volatility regime
     * @param symbol Instrument symbol
     * @param raw_forecast Raw trading signal
     * @param type Type of forecast (trend/carry)
     * @return Result containing scaled forecast
     */
    Result<double> scale_forecast(
        const std::string& symbol,
        double raw_forecast,
        ForecastType type) const;

    /**
     * @brief Get current volatility multiplier for a symbol
     * @param symbol Instrument symbol
     * @return Result containing current multiplier
     */
    Result<double> get_multiplier(const std::string& symbol) const;

    /**
     * @brief Get volatility quantile for a symbol
     * @param symbol Instrument symbol
     * @return Result containing current quantile (0-1)
     */
    Result<double> get_quantile(const std::string& symbol) const;

private:
    ForecastScalerConfig config_;
    
    // Store volatility history and EWMA multipliers for each symbol
    std::unordered_map<std::string, std::deque<double>> volatility_history_;
    std::unordered_map<std::string, std::deque<double>> multiplier_history_;
    
    /**
     * @brief Calculate volatility quantile
     * @param symbol Instrument symbol
     * @return Quantile in [0,1] range
     */
    double calculate_quantile(const std::string& symbol) const;

    /**
     * @brief Calculate volatility multiplier
     * @param quantile Volatility quantile
     * @return Multiplier based on quantile
     */
    double calculate_multiplier(double quantile) const;

    /**
     * @brief Update EWMA multiplier
     * @param symbol Instrument symbol
     * @param new_multiplier Latest multiplier value
     */
    void update_ewma_multiplier(
        const std::string& symbol,
        double new_multiplier);
};

} // namespace trade_ngin