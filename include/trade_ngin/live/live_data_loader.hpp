// include/trade_ngin/live/live_data_loader.hpp
// Data loading component for live trading - encapsulates all SELECT queries.
//
// Timezone contract (Phase 5 §5c):
//   All `Timestamp` parameters and all `YYYY-MM-DD` keys produced by this
//   module are UTC. Provider date columns (e.g. `equities_data.corporate_action.date`)
//   are interpreted as calendar dates with no timezone shift -- they are
//   text/date values whose semantics are determined by the ingest pipeline,
//   not by this loader. If a strategy needs market-local semantics, convert
//   at the strategy boundary, not here.
//
//   Date-string keys MUST be produced via `trade_ngin::core::format_utc_date`;
//   direct `std::gmtime` / `std::put_time` use is forbidden (non-thread-safe
//   and locale-dependent).
//
// Decoding contract (Phase 5 §1.17a + §5d):
//   Numeric columns from `convert_generic_to_arrow` are stored as utf8.
//   Implementations use `DataConversionUtils::safe_get_*` which dispatches
//   on the actual Arrow type, falls back to `std::stod`/`std::stoll` for
//   string storage (logging WARN on parse failures), and returns a typed
//   `Result` on null/type-mismatch -- never a silent 0.0.

#pragma once

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/error.hpp"  // Contains Result<T>
#include "trade_ngin/live/carried_day.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/live/live_historical_metrics.hpp"
#include "trade_ngin/live/live_statistics_columns.hpp"

namespace trade_ngin {

// Forward declarations
struct Position;

/**
 * @brief Structure representing a row from trading.live_results table
 */
struct LiveResultsRow {
    double daily_pnl = 0.0;
    double total_pnl = 0.0;
    double daily_realized_pnl = 0.0;
    double daily_unrealized_pnl = 0.0;
    double daily_return = 0.0;
    double total_cumulative_return = 0.0;  // Total return since inception (non-annualized)
    double total_annualized_return = 0.0;  // Annualized return
    double current_portfolio_value = 0.0;
    double gross_leverage = 0.0;
    double equity_to_margin_ratio = 0.0;
    double gross_notional = 0.0;
    double margin_posted = 0.0;
    double cash_available = 0.0;
    double daily_transaction_costs = 0.0;
    double daily_roll_costs = 0.0;  // T-ROLLX (017): the ROLL subset of daily_transaction_costs
    double total_roll_costs = 0.0;  // T-ROLLX (017): cumulative
    Timestamp date;
    std::string strategy_id;

    // Additional metrics
    double sharpe_ratio = 0.0;
    double sortino_ratio = 0.0;
    double max_drawdown = 0.0;
    double volatility = 0.0;
    double win_rate = 0.0;
    double avg_win = 0.0;
    double avg_loss = 0.0;
    double profit_factor = 0.0;
    double best_day = 0.0;
    double worst_day = 0.0;
    double downside_deviation = 0.0;
    double gross_profit = 0.0;
    double gross_loss = 0.0;
    int active_positions = 0;
    // Removed total_trades - will be implemented properly later with closing trades logic
    int winning_days = 0;
    int losing_days = 0;
    int total_days = 0;
};

/**
 * @brief The report's statistics and return lines from a day's stored row (T-8D-2 R58, R61): the
 *        metric names the daily report reads, each from its stored column. The statistics are the
 *        settled series' (they run through the day before the row's date); the cumulative return
 *        is the row's own level. Flat days are total less winning less losing.
 */
inline std::map<std::string, double> statistics_report_metrics(const LiveResultsRow& row) {
    return {
        {"Total Cumulative Return", row.total_cumulative_return},
        {"Total Annualized Return", row.total_annualized_return},
        {"Sharpe Ratio", row.sharpe_ratio},
        {"Sortino Ratio", row.sortino_ratio},
        {"Max Drawdown", row.max_drawdown},
        {"Volatility", row.volatility},
        {"Win Rate", row.win_rate},
        {"Average Win", row.avg_win},
        {"Average Loss", row.avg_loss},
        {"Profit Factor", row.profit_factor},
        {"Best Day", row.best_day},
        {"Worst Day", row.worst_day},
        {"Downside Deviation", row.downside_deviation},
        {"Gross Profit", row.gross_profit},
        {"Gross Loss", row.gross_loss},
        {"Winning Days", static_cast<double>(row.winning_days)},
        {"Losing Days", static_cast<double>(row.losing_days)},
        {"Flat Days",
         static_cast<double>(std::max(0, row.total_days - row.winning_days - row.losing_days))},
        {"Total Days", static_cast<double>(row.total_days)},
    };
}

/**
 * @brief Structure for previous day's data
 */
struct PreviousDayData {
    double portfolio_value = 0.0;
    double total_pnl = 0.0;
    double daily_pnl = 0.0;
    double daily_transaction_costs = 0.0;
    Timestamp date;
    bool exists = false;  // false if no previous day found
};

/**
 * @brief LiveDataLoader - Encapsulates all data retrieval operations for live trading
 *
 * This class replaces raw SQL SELECT queries with type-safe methods.
 * All methods return Result<T> for proper error handling.
 * Designed to be reusable across different live trading strategies.
 */
class LiveDataLoader {
private:
    std::shared_ptr<PostgresDatabase> db_;
    std::string schema_;  // "trading" or "backtest"
    // QT plan E3: the book (portfolio_type) every live_results, equity_curve, positions and
    // executions read is scoped to. "system" unless set_book() says otherwise; on the backtest
    // schema no book column exists and no filter is applied.
    std::string book_{"system"};
    std::string book_filter() const;

    // Helper method to check database connection
    Result<void> validate_connection() const;

    // The predicate "<date_expr> is on or after the book's start": the earliest live_start_date of
    // <schema>.strategy_trading_days_metadata for the key, no lower bound when the key has no row.
    std::string on_or_after_book_start(const std::string& date_expr,
                                       const std::string& strategy_id,
                                       const std::string& portfolio_id) const;

    // The scalar subquery that predicate compares with: the key's earliest live_start_date.
    std::string book_start_subquery(const std::string& strategy_id,
                                    const std::string& portfolio_id) const;

public:
    /// The book (system, qt_proposal or qt) the reads below are scoped to (QT plan E3).
    void set_book(const std::string& book) { book_ = book; }
    const std::string& get_book() const { return book_; }
    /**
     * @brief Construct a new LiveDataLoader
     * @param db Shared database connection
     * @param schema Database schema to use (default: "trading")
     */
    LiveDataLoader(std::shared_ptr<PostgresDatabase> db, const std::string& schema = "trading");

    ~LiveDataLoader() = default;

    // ========== Live Results Methods ==========

    /**
     * @brief Load complete live results row for a date
     * @param strategy_id Strategy identifier
     * @param date Target date
     * @return LiveResultsRow or error
     */
    Result<LiveResultsRow> load_live_results(const std::string& strategy_id,
                                             const std::string& portfolio_id,
                                             const Timestamp& date);

    /**
     * @brief Load previous day's complete data
     * @param strategy_id Strategy identifier
     * @param date Current date (will find previous trading day)
     * @return PreviousDayData or error
     */
    Result<PreviousDayData> load_previous_day_data(const std::string& strategy_id,
                                                   const std::string& portfolio_id,
                                                   const Timestamp& date);

    /**
     * @brief Get count of live results rows for strategy
     * @param strategy_id Strategy identifier
     * @return Row count or error
     */
    Result<int> get_live_results_count(const std::string& strategy_id,
                                       const std::string& portfolio_id = "BASE_PORTFOLIO");

    // ========== Historical Series Methods (since inception, as-of date) ==========

    /// One stored day of a book's P&L history, for the sizing capital (LOOP_SPEC section 3.1).
    struct PnlHistoryRow {
        std::string date;         ///< YYYY-MM-DD
        double daily_pnl{0.0};    ///< the row's stored net P&L
        int active_positions{0};  ///< the positions the day's stored book held
        bool settled_at_set{false};  ///< the row carries a settled_at stamp (migration 035)
    };

    /**
     * @brief The book's stored daily net P&L in date order, for the sizing capital: every
     *        live_results row of the key dated on or after the book's start (the earliest
     *        live_start_date of trading.strategy_trading_days_metadata for the key; no lower bound
     *        when the key has no such row) and strictly before `before_date`.
     *
     * Each row says whether its settled_at is set, so the read needs migration 035 applied.
     * No row is an empty list, not an error; a failed query is a DATABASE_ERROR.
     */
    Result<std::vector<PnlHistoryRow>> load_sizing_pnl_history(const std::string& strategy_id,
                                                               const std::string& portfolio_id,
                                                               const Timestamp& before_date);

    /**
     * @brief The book's start as "YYYY-MM-DD": the earliest live_start_date of
     *        <schema>.strategy_trading_days_metadata for the key, the anchor the sizing history
     *        above and the statistics levels below are bounded by. Empty when the key has no row.
     */
    Result<std::string> load_book_start(const std::string& strategy_id,
                                        const std::string& portfolio_id);

    /**
     * @brief total_dividend_income as stored on the key's <schema>.live_results row of a date:
     *        the previous value a run carries when it cannot recount the dividends. No value
     *        when the date has no row or the cell is NULL.
     */
    Result<std::optional<double>> load_stored_dividend_income(const std::string& strategy_id,
                                                              const std::string& portfolio_id,
                                                              const Timestamp& date);

    /**
     * @brief The book's stored per-symbol P&L cells for the worst day's symbol (migration 036):
     *        per date and symbol, SUM(daily_realized_pnl) and SUM(daily_unrealized_pnl) of
     *        <schema>.positions over the book's sleeves, from the book's start through
     *        `through_date`, ascending. A futures row books its daily move as realised and
     *        carries 0 unrealised; an equity row's unrealised cell is the open position's level.
     */
    Result<std::vector<SymbolDayPnl>> load_symbol_pnl_history(const std::string& strategy_id,
                                                              const std::string& portfolio_id,
                                                              const Timestamp& through_date);

    /**
     * @brief The sizing capital each stored row was sized on (migration 036): (date,
     *        risk_detail.sizing_capital) of every live_results row of the key that carries one,
     *        from the book's start through `through_date`, ascending.
     */
    Result<std::vector<DatedCapital>> load_sizing_capital_history(const std::string& strategy_id,
                                                                  const std::string& portfolio_id,
                                                                  const Timestamp& through_date);

    /**
     * @brief The book's stored executions for its fill counts (migration 036): every row of
     *        <schema>.executions of the key whose stored date is from the book's start through
     *        `through_date`. Each row's fill time is its stored DATE (midnight UTC), the day the
     *        sleeves' rows are netted on; the price, the quantity, the cost, the netting
     *        adjustment, the execution type and the instrument id are the stored cells.
     */
    Result<std::vector<ExecutionReport>> load_book_executions(const std::string& strategy_id,
                                                              const std::string& portfolio_id,
                                                              const Timestamp& through_date);

    /**
     * @brief The book's stored levels for its statistics: (date, current_portfolio_value) of
     *        every live_results row of the key from the book's start through `through_date`,
     *        ascending. A row with no value is left out.
     */
    Result<std::vector<DatedLevel>> load_statistics_levels(const std::string& strategy_id,
                                                           const std::string& portfolio_id,
                                                           const Timestamp& through_date);

    /**
     * @brief The dates of a futures book's statistics grid from its start through
     *        `through_date`, ascending: the Sunday-to-Friday dates on which at least
     *        kStatisticsGridMinSymbols DISTINCT symbols of `symbols` printed a bar in
     *        futures_data.ohlcv_1d (T-8D R3).
     *
     * Computed from the bars alone. T-8D-2 R81: this count is the grid the STATISTICS read and
     * the predicate the sessions a year were measured with; it is not the trading session test.
     * Trading decides per symbol with the session classifier, with a quarter of the universe only
     * as its book-level fallback, so the two can disagree on a date by design (a Saturday print
     * may trade and is never a statistics row: its P&L lands in Sunday's grid return). Neither
     * the classifier nor the runner's closed-market guard is read here.
     *
     * `symbols` is the live book's universe on the dates asked for: a live book's start is in
     * 2025 or later, after every listing date, so the universe is its 36 listed contracts and no
     * predecessor contract (ES, NQ, YM, RTY) stands in for a micro on any of these dates.
     */
    Result<std::vector<std::string>> load_futures_statistics_grid(
        const std::vector<std::string>& symbols, const std::string& strategy_id,
        const std::string& portfolio_id, const Timestamp& through_date);

    /**
     * @brief Load total trade count (number of executions) since inception up to a date.
     *
     * Counts rows in trading.executions for the given strategy and portfolio where
     * DATE(execution_time) <= as_of_date.
     */
    Result<int> load_total_trades_count(const std::string& strategy_id,
                                        const std::string& portfolio_id,
                                        const Timestamp& as_of_date);

    // ========== Position Methods ==========

    /**
     * @brief Load positions for a specific date
     * @param strategy_id Strategy identifier
     * @param date Target date
     * @return Vector of positions or error
     */
    Result<std::vector<Position>> load_positions(const std::string& strategy_id,
                                                 const std::string& portfolio_id,
                                                 const Timestamp& date);

    /**
     * @brief Load positions for CSV export (with specific fields)
     * @param strategy_id Strategy identifier
     * @param date Target date
     * @return Positions suitable for CSV export
     */
    Result<std::vector<Position>> load_positions_for_export(const std::string& strategy_id,
                                                            const std::string& portfolio_id,
                                                            const Timestamp& date);

    // ========== Commission Methods ==========

    /**
     * @brief Load commissions grouped by symbol for a date
     * @param date Target date
     * @return Map of symbol to total commission or error
     *
     * Sums the executions table's commissions_fees per symbol for the portfolio
     * (BASE_PORTFOLIO when empty) on the date's UTC day; no rows is an empty map.
     */
    Result<std::unordered_map<std::string, double>> load_commissions_by_symbol(
        const std::string& portfolio_id, const Timestamp& date);

    // ========== Carried-day Methods ==========

    /**
     * @brief A sleeve's last computed forecasts: its <schema>.signals rows of the latest stored
     *        run date strictly before `date`'s UTC calendar date.
     *
     * T-7a C3 (HD 2026-09-18): on a day with no session the positions file carries the previous
     * session's forecasts instead of the unfed strategies' empty ones. Rows are matched on
     * portfolio_id, strategy_id AND strategy_name, so each sleeve of a multi-sleeve book reads
     * its own. A sleeve with no stored signals returns an empty result (session_date empty).
     */
    Result<CarriedForecasts> load_last_signals_before(const std::string& strategy_id,
                                                      const std::string& strategy_name,
                                                      const std::string& portfolio_id,
                                                      const Timestamp& date);

    // ========== Utility Methods ==========

    /**
     * @brief Get the schema being used
     * @return Current schema name
     */
    const std::string& get_schema() const {
        return schema_;
    }

    /**
     * @brief Check if database connection is valid
     * @return true if connected, false otherwise
     */
    bool is_connected() const;
};

}  // namespace trade_ngin