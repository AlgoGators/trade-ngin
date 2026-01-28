#pragma once

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "trade_ngin/transaction_cost/asset_cost_config.hpp"

namespace trade_ngin {
namespace transaction_cost {

/**
 * @brief Tick-based spread model with volatility regime widening
 *
 * Computes bid-ask spread cost based on:
 * - Baseline spread in ticks (configured per symbol)
 * - Volatility regime widening (mild multiplier)
 *
 * Key design decisions:
 * - Spread is anchored to microstructure (ticks), NOT daily range
 * - Volatility widening is mild (0.8x to 1.5x) to represent timing/slippage
 * - We apply a per-asset spread cost multiplier (default 0.5)
 */
class SpreadModel {
public:
    /**
     * @brief Configuration for volatility regime widening
     */
    struct VolatilityConfig {
        double lambda;         // Sensitivity to vol z-score
        double min_multiplier;  // Floor for vol multiplier
        double max_multiplier;  // Cap for vol multiplier
        size_t lookback_days;    // Days for rolling vol calculation

        VolatilityConfig()
            : lambda(0.15), min_multiplier(0.8), max_multiplier(1.5), lookback_days(20) {}
    };

    explicit SpreadModel(const VolatilityConfig& vol_config = VolatilityConfig());

    /**
     * @brief Calculate spread price impact per contract
     *
     * @param config Asset configuration (tick_size, baseline_spread_ticks, etc.)
     * @param volatility_multiplier Volatility regime multiplier (0.8 to 1.5)
     * @return Spread cost in price units per contract
     *
     * Formula: spread_price_impact = spread_cost_multiplier * spread_ticks * tick_size
     * where: spread_ticks = clamp(baseline_spread_ticks * vol_mult, min, max)
     */
    double calculate_spread_price_impact(
        const AssetCostConfig& config,
        double volatility_multiplier) const;

    /**
     * @brief Calculate volatility multiplier from log returns
     *
     * Uses rolling standard deviation of log returns with z-score normalization.
     *
     * @param log_returns Vector of log returns (most recent at end)
     * @return Volatility multiplier clamped to [min_multiplier, max_multiplier]
     *
     * Formula:
     *   sigma = stdev(log_returns)
     *   z = clip((sigma - mean_sigma) / stdev_sigma, -2, 2)
     *   vol_mult = clip(1 + lambda * z, min_mult, max_mult)
     */
    double calculate_volatility_multiplier(const std::vector<double>& log_returns) const;

    /**
     * @brief Update rolling log returns for a symbol
     *
     * Call this daily with the new log return.
     *
     * @param symbol Instrument symbol
     * @param log_return Today's log return: ln(close_t / close_t-1)
     */
    void update_log_returns(const std::string& symbol, double log_return);

    /**
     * @brief Get volatility multiplier for a symbol using stored returns
     *
     * @param symbol Instrument symbol
     * @return Volatility multiplier (1.0 if insufficient data)
     */
    double get_volatility_multiplier(const std::string& symbol) const;

    /**
     * @brief Clear stored data for a symbol
     */
    void clear_symbol_data(const std::string& symbol);

    /**
     * @brief Clear all stored data
     */
    void clear_all();

private:
    VolatilityConfig vol_config_;

    // Rolling log returns per symbol (deque for efficient front removal)
    std::map<std::string, std::deque<double>> symbol_log_returns_;

    // Helper to compute mean of a vector
    static double compute_mean(const std::vector<double>& values);

    // Helper to compute standard deviation of a vector
    static double compute_stdev(const std::vector<double>& values, double mean);
};

}  // namespace transaction_cost
}  // namespace trade_ngin
