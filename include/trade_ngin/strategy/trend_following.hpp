// include/trade_ngin/strategy/trend_following.hpp
#pragma once

#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/strategy/forecast_scaler.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include <vector>
#include <utility>
#include <memory>

namespace trade_ngin {

/**
 * @brief Configuration specific to trend following strategy
 */
struct TrendFollowingConfig {
    double risk_target{0.2};          // Target annualized risk level
    double fx_rate{1.0};              // FX conversion rate
    double idm{2.5};                  // Instrument diversification multiplier
    bool use_position_buffering{true}; // Whether to use position buffers to reduce trading
    std::vector<std::pair<int, int>> ema_windows{ // EMA window pairs for crossovers
        {2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}
    };
    int vol_lookback_short{22};    // Short lookback for volatility calculation
    int vol_lookback_long{2520};   // Long lookback for volatility calculation

    // Forecast scaling configuration
    ForecastScalerConfig forecast_config{
        252,    // volatility_lookback
        10.0,   // ewma_decay
        30.0,   // base_scalar_trend
        23.0,   // base_scalar_carry
        20.0    // forecast_cap
    };
};

/**
 * @brief Multi-timeframe trend following strategy using EMA crossovers
 */
class TrendFollowingStrategy : public BaseStrategy {
public:
    /**
     * @brief Constructor
     * @param id Strategy identifier
     * @param config Base strategy configuration
     * @param trend_config Trend following specific configuration
     * @param db Database interface
     */
    TrendFollowingStrategy(
        std::string id,
        StrategyConfig config,
        TrendFollowingConfig trend_config,
        std::shared_ptr<DatabaseInterface> db);

    /**
     * @brief Process new market data
     * @param data Vector of price bars
     * @return Result indicating success or failure
     */
    Result<void> on_data(const std::vector<Bar>& data) override;

protected:
    /**
     * @brief Validate strategy configuration
     * @return Result indicating if config is valid
     */
    Result<void> validate_config() const override;

private:
    TrendFollowingConfig trend_config_;
    
    // Price and signal storage
    std::unordered_map<std::string, std::vector<double>> price_history_;
    std::unordered_map<std::string, std::vector<double>> volatility_history_;

    // Forecast scaler for volatility-based position sizing
    ForecastScaler forecast_scaler_;
    
    /**
     * @brief Calculate EMA crossover signals
     * @param prices Price history for a symbol
     * @param short_window Shorter EMA window
     * @param long_window Longer EMA window
     * @return Vector of crossover signals
     */
    std::vector<double> calculate_ema_crossover(
        const std::vector<double>& prices,
        int short_window,
        int long_window) const;

    /**
     * @brief Calculate volatility estimate
     * @param prices Price history
     * @param short_lookback Short lookback period
     * @param long_lookback Long lookback period
     * @return Vector of volatility estimates
     */
    std::vector<double> calculate_volatility(
        const std::vector<double>& prices,
        int short_lookback,
        int long_lookback) const;

    /**
     * @brief Generate raw forecast from EMA crossovers
     * @param prices Price history
     * @param volatility Volatility estimates
     * @return Vector of raw forecasts
     */
    std::vector<double> generate_raw_forecasts(
        const std::vector<double>& prices,
        const std::vector<double>& volatility) const;

    /**
     * @brief Calculate position for a symbol
     * @param symbol Instrument symbol
     * @param forecast Trading forecast
     * @param price Current price
     * @param volatility Current volatility
     * @return Target position
     */
    double calculate_position(
        const std::string& symbol,
        double forecast,
        double price,
        double volatility) const;

    /**
     * @brief Apply position buffering
     * @param symbol Instrument symbol
     * @param raw_position Calculated position before buffering
     * @param price Current price
     * @param volatility Current volatility
     * @return Buffered position
     */
    double apply_position_buffer(
        const std::string& symbol,
        double raw_position,
        double price, 
        double volatility) const;
};

} // namespace trade_ngin