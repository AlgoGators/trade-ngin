#pragma once

#include "trade_ngin/live/price_manager_base.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/core/types.hpp"
#include <memory>
#include <optional>  // std::optional t1_date (E2-F14)

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
     * @param bars Vector of bars to process
     * @param reference_date The date to use as "today" for T-1 calculation (defaults to system time if not provided)
     * @param t1_date OPTIONAL explicit Day T-1 trading date. See below -- E2-F14.
     *
     * @par Why t1_date exists
     * Omitted (the default), T-1 is `reference_date - 24h` and only the LAST bar is
     * considered. That is correct for FUTURES, whose runners resolve the position book
     * they are finalizing with the SAME arithmetic --
     * `live_portfolio.cpp:1047` and `live_portfolio_conservative.cpp:1061` both do
     * `previous_date = now - std::chrono::hours(24)`. Book and prices therefore always
     * name the same calendar day by construction, each day is finalized exactly once, and
     * when that day has no bar (a Saturday, or an ag symbol with no Sunday session) zero
     * genuinely is the right answer. Both futures runners pass no t1_date and MUST keep
     * getting byte-identical behaviour.
     *
     * EQUITIES do not have that property. The equity runner resolves its book with
     * `holiday_checker.find_previous_trading_day()` (live_equity_mean_reversion.cpp:643),
     * which is calendar-aware and skips weekends and holidays. On a Sunday or Monday run
     * that returns FRIDAY, while `reference_date - 24h` asks for Saturday or Sunday and
     * `equities_data.ohlcv_1d` has no weekend rows at all. The result is that an
     * already-finalized Friday is re-finalized against an EMPTY price map: every position
     * takes the `t1_close_prices.end()` branch in LivePnLManager::finalize_previous_day
     * and is written back with 0/0. `live_results` survives because the
     * `yesterday_total_pnl != 0.0` gate blocks its UPDATE, but `store_positions` runs
     * ~480 lines earlier, outside that gate, and its DELETE-then-INSERT is keyed on the
     * T-1 date -- so Friday's finalized rows are replaced with zeros.
     *
     * Measured 2026-09-01 on a weekend-inclusive replay: Sat 08-01 finalized Friday at
     * $864.759444, then Sun 08-02 and Mon 08-03 each re-finalized the same Friday and
     * zeroed its three position rows. L5 (rows sum to aggregate) residual was exactly
     * -864.7594 on 07-31 and -14.6574 on 07-24, and 0.0000 on every other finalized day.
     * A Monday holiday makes it fire on four consecutive runs.
     *
     * @par Two fixes that look right and are NOT
     * - Adding a T-1 FALLBACK here (use the most recent bar at or before the expected
     *   date) would DOUBLE-COUNT FUTURES. For an ag future on a Monday run with no Sunday
     *   bar, T-1 would fall back to Friday and T-2 to Thursday, booking (Fri-Thu) onto the
     *   Sunday row -- a move the Saturday run already booked onto Friday's row.
     * - Making this function calendar-aware would also break futures: the futures calendar
     *   is not the equity calendar. Futures trade Sunday evening; equities do not.
     *
     * The only safe shape is for the CALLER to pass the same resolved date it used for the
     * book, so the two lookups cannot disagree. When t1_date is supplied the bar is
     * SEARCHED for by date rather than assumed to be `back()` -- on a live run the vendor
     * may already have posted today's bar, and a back()-only test would then reject a
     * perfectly good Friday -- and T-2 becomes the bar immediately preceding it.
     */
    Result<void> update_from_bars(
        const std::vector<Bar>& bars,
        const Timestamp& reference_date = std::chrono::system_clock::now(),
        std::optional<Timestamp> t1_date = std::nullopt);

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