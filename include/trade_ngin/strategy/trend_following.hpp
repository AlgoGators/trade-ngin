// include/trade_ngin/strategy/trend_following.hpp
#pragma once

#include "trade_ngin/strategy/base_strategy.hpp"
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
    int vol_lookback_short{32};    // Short lookback for volatility calculation
    int vol_lookback_long{2520};   // Long lookback for volatility calculation
    std::vector<std::pair<int, double>> fdm {
        {1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}
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

    /**
     * @brief Initialize strategy
     * @return Result indicating success or failure
     */
    Result<void> initialize() override;

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

    /**
     * @brief Calculate EWMA for a price series
     * @param prices Price series
     * @param window EWMA window
     * @return Vector of EWMA values
     */
    std::vector<double> calculate_ewma(
        const std::vector<double>& prices,
        int window) const;

    /**
     * @brief Computes the blended EWMA standard deviation using short-term and long-term components.
     * @param prices Vector of price data.
     * @param N Lookback period for short-term EWMA std dev.
     * @param weight_short Weight for short-term EWMA (default: 70%).
     * @param weight_long Weight for long-term EWMA (default: 30%).
     * @param max_history Maximum historical records (default: 10 years).
     * @return Vector of blended EWMA standard deviation.
     */
    std::vector<double> blended_ewma_stddev(
        const std::vector<double>& prices,
        int N,
        double weight_short = 0.7,
        double weight_long = 0.3,
        size_t max_history = 2520) const;

    /**
     * @brief Computes the EWMA standard deviation using a lambda-based approach.
     * @param prices Vector of price data.
     * @param N Lookback period for EWMA.
     * @return Vector of EWMA standard deviation values.
     */
    std::vector<double> ewma_standard_deviation(
        const std::vector<double>& prices,
        int N) const;

    /**
     * @brief Computes the long-term average of EWMA standard deviations.
     * @param history Vector storing past EWMA standard deviations.
     * @param max_history Maximum number of historical periods (default: 10 years).
     * @return Long-term average EWMA standard deviation.
     */
    double compute_long_term_avg(
        const std::vector<double>& history,
        size_t max_history = 2520) const;

    /** 
    * @brief Calculate EMA crossover signals and scale by volatility
    * @param prices Price history for a symbol
    * @param short_window Shorter EMA window
    * @param long_window Longer EMA window
    * @return Vector of crossover signals
    */
    std::vector<double> get_raw_forecast(
        const std::vector<double>& prices,
        int short_window,
        int long_window) const;

    /**
     * @brief Scale raw forecasts by volatility
     * @param raw_forecasts Raw forecast values
     * @param blended_stddev Blended EWMA standard deviation
     * @return Scaled forecast values
     */
    std::vector<double> get_scaled_forecast(
        const std::vector<double>& raw_forecasts,
        const std::vector<double>& blended_stddev) const;

    /**
     * @brief Generate raw forecast from EMA crossovers
     * @param prices Price history
     * @return Vector of raw forecasts
     */
    std::vector<double> get_raw_combined_forecast(const std::vector<double>& prices) const;

    /**
     * @brief Calculate absolute value of a vector
     * @param values Input vector
     * @return Absolute sum of vector elements
     */
    double get_abs_value(const std::vector<double>& values) const;

    /**
     * @brief Generate scaled forecast from EMA crossovers
     * @param raw_combined_forecast Raw forecast values
     * @return Scaled forecast values
     */
    std::vector<double> get_scaled_combined_forecast(const std::vector<double>& raw_combined_forecast) const; 

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

    /**
     * @brief Calculate volatility regime multiplier
     * @param prices Price history
     * @param volatility Pre-calculated volatility series
     * @return Volatility regime multiplier
     */
    double calculate_vol_regime_multiplier(
        const std::vector<double>& prices,
        const std::vector<double>& volatility) const;
};

} // namespace trade_ngin