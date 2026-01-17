#pragma once

#include "trade_ngin/live/price_manager_base.hpp"
#include "trade_ngin/core/types.hpp"
#include <unordered_map>
#include <vector>
#include <string>

namespace trade_ngin {
namespace backtest {

/**
 * Backtest implementation of PriceManager
 * Manages price history for the beginning-of-day execution model.
 *
 * This replaces the static variables previously in:
 * - process_bar() lines 798-799
 * - process_strategy_signals() line 1065
 * - process_portfolio_data() line 1431
 *
 * Key responsibilities:
 * - Track current day close prices
 * - Track previous day close prices (for execution pricing - no lookahead)
 * - Track two days ago prices (for T-2 reference)
 * - Provide clean reset() for multi-run support
 */
class BacktestPriceManager : public PriceManagerBase {
private:
    // Price caches by symbol
    std::unordered_map<std::string, double> current_prices_;
    std::unordered_map<std::string, double> previous_day_prices_;
    std::unordered_map<std::string, double> two_days_ago_prices_;

    // Price history for each symbol (for calculating returns)
    std::unordered_map<std::string, std::vector<double>> price_history_;

    // Track whether we have valid previous prices
    bool has_previous_prices_ = false;

public:
    BacktestPriceManager() = default;
    ~BacktestPriceManager() override = default;

    /**
     * Update prices from a batch of bars
     * Automatically shifts prices: current -> previous -> two_days_ago
     * @param bars Vector of bars for the current day
     */
    void update_from_bars(const std::vector<Bar>& bars);

    /**
     * Manually shift prices without updating from new bars
     * Used when processing warmup periods or special cases
     */
    void shift_prices();

    /**
     * Get current day close price for a symbol
     * @return Price or error if symbol not found
     */
    Result<double> get_current_price(const std::string& symbol) const;

    /**
     * Get previous day close price for a symbol
     * Used for execution pricing in BOD model (no lookahead)
     * @return Price or error if symbol not found
     */
    Result<double> get_previous_day_price(const std::string& symbol) const;

    /**
     * Get two days ago close price for a symbol
     * Used for T-2 PnL reference
     * @return Price or error if symbol not found
     */
    Result<double> get_two_days_ago_price(const std::string& symbol) const;

    /**
     * Get all current prices
     */
    const std::unordered_map<std::string, double>& get_all_current_prices() const {
        return current_prices_;
    }

    /**
     * Get all previous day prices
     */
    const std::unordered_map<std::string, double>& get_all_previous_day_prices() const {
        return previous_day_prices_;
    }

    /**
     * Get all two days ago prices
     */
    const std::unordered_map<std::string, double>& get_all_two_days_ago_prices() const {
        return two_days_ago_prices_;
    }

    /**
     * Check if previous day prices are available
     * On the first bar of a backtest, previous prices won't exist
     */
    bool has_previous_prices() const {
        return has_previous_prices_;
    }

    /**
     * Get price history for a symbol
     * @return Vector of historical prices (oldest first)
     */
    const std::vector<double>* get_price_history(const std::string& symbol) const;

    /**
     * Get price history length for a symbol
     */
    size_t get_price_history_length(const std::string& symbol) const;

    /**
     * Reset all state for clean multi-run support
     * Must be called between backtest runs
     */
    void reset();

    // PriceManagerBase interface implementation

    /**
     * Get price for a symbol at a timestamp
     * For backtest, this returns the current cached price
     * (timestamp is ignored since we process bars sequentially)
     */
    Result<double> get_price(
        const std::string& symbol,
        const Timestamp& timestamp) const override;

    /**
     * Get prices for multiple symbols
     */
    Result<std::unordered_map<std::string, double>> get_prices(
        const std::vector<std::string>& symbols,
        const Timestamp& timestamp) const override;
};

} // namespace backtest
} // namespace trade_ngin
