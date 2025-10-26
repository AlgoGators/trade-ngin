#pragma once

#include "trade_ngin/live/price_manager_base.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/core/types.hpp"
#include <memory>

namespace trade_ngin {

/**
 * Live implementation of PriceManager
 * Handles price retrieval from database and caching for live trading
 * This extracts ~100+ lines of price logic from live_trend.cpp
 */
class LivePriceManager : public PriceManagerBase {
private:
    std::shared_ptr<PostgresDatabase> db_;

    // Cache for different price types
    mutable std::unordered_map<std::string, double> latest_prices_;  // Most recent prices
    mutable std::unordered_map<std::string, double> settlement_prices_;  // Settlement/close prices
    mutable std::unordered_map<std::string, double> previous_day_prices_;  // T-1 close prices
    mutable std::unordered_map<std::string, double> two_days_ago_prices_;  // T-2 close prices

    std::string data_schema_ = "futures_data";

public:
    /**
     * Constructor
     */
    explicit LivePriceManager(std::shared_ptr<PostgresDatabase> db)
        : db_(std::move(db)) {}

    /**
     * Load close prices for a specific date
     * Replaces the manual SQL queries in live_trend.cpp
     */
    Result<std::unordered_map<std::string, double>> load_close_prices(
        const std::vector<std::string>& symbols,
        const Timestamp& date) const;

    /**
     * Load previous day (T-1) close prices
     * Used for execution prices and current market values
     */
    Result<void> load_previous_day_prices(
        const std::vector<std::string>& symbols,
        const Timestamp& current_date);

    /**
     * Load two days ago (T-2) close prices
     * Used for Day T-1 PnL finalization
     */
    Result<void> load_two_days_ago_prices(
        const std::vector<std::string>& symbols,
        const Timestamp& current_date);

    /**
     * Update prices from live market data (bars)
     * Used during live trading to update latest prices
     */
    Result<void> update_from_bars(const std::vector<Bar>& bars);

    /**
     * Get settlement/close price for a symbol on a date
     * Replaces the settlement price queries in live_trend.cpp
     */
    Result<double> get_settlement_price(
        const std::string& symbol,
        const Timestamp& date) const;

    /**
     * Get latest cached price for a symbol
     */
    Result<double> get_latest_price(const std::string& symbol) const;

    /**
     * Get previous day close price (T-1)
     */
    Result<double> get_previous_day_price(const std::string& symbol) const;

    /**
     * Get two days ago close price (T-2)
     */
    Result<double> get_two_days_ago_price(const std::string& symbol) const;

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
     * Clear all price caches
     */
    void clear_caches() {
        latest_prices_.clear();
        settlement_prices_.clear();
        previous_day_prices_.clear();
        two_days_ago_prices_.clear();
    }

    // Implementation of base interface
    Result<double> get_price(
        const std::string& symbol,
        const Timestamp& timestamp) const override;

    Result<std::unordered_map<std::string, double>> get_prices(
        const std::vector<std::string>& symbols,
        const Timestamp& timestamp) const override;
};

} // namespace trade_ngin