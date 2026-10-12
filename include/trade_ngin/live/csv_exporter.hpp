#pragma once

#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/carried_day.hpp"

namespace trade_ngin {

// Forward declarations
class IDatabase;
class TrendFollowingStrategy;
// Base interface for trend following strategies (both standard and slow)
class BaseStrategy;
typedef BaseStrategy
    ITrendFollowingStrategy;  // Use base class for compatibility with both strategy types
struct Position;

// Type alias for per-strategy positions map
using StrategyPositionsMap = std::unordered_map<std::string, std::unordered_map<std::string, Position>>;

// Type alias for per-strategy strategies map
using StrategyInstancesMap = std::unordered_map<std::string, ITrendFollowingStrategy*>;

/**
 * @brief CSVExporter handles exporting trading positions to CSV files
 *
 * This class encapsulates all CSV export functionality, including:
 * - Today's positions (forward-looking, no PnL)
 * - Yesterday's finalized positions (with PnL)
 *
 * Phase 4 Refactoring Component
 */
class CSVExporter {
public:
    /**
     * @brief Construct a new CSVExporter
     * @param output_directory Directory where CSV files will be written (default: current
     * directory)
     */
    explicit CSVExporter(const std::string& output_directory = ".");

    /**
     * @brief Export current day positions to CSV with per-strategy breakdown
     *
     * Creates a file named: YYYY-MM-DD_positions.csv
     * Contains: strategy, symbol, quantity, market_price, notional, percentages, forecast,
     * sizing_vol, EMAs. The first line is the portfolio value as this run wrote it (today's
     * costs booked), with the gross and net notional; two comment lines above the column names
     * say what sizing_vol is and what pct_of_portfolio_value divides by. Every row prints six
     * decimals, but market_price, which prints the shortest text that reads back as the same
     * number (a price below 0.1 keeps every digit).
     *
     * @param date Current trading date
     * @param strategy_positions Per-strategy positions map
     * @param market_prices Market prices for positions (Day T-1 close)
     * @param portfolio_value Current portfolio value
     * @param gross_notional Gross notional value
     * @param net_notional Net notional value
     * @param strategy_instances Map of strategy name to strategy instance for forecasts and EMAs
     * @return Result containing filename on success, or error
     */
    Result<std::string> export_current_positions(
        const std::chrono::system_clock::time_point& date,
        const StrategyPositionsMap& strategy_positions,
        const std::unordered_map<std::string, double>& market_prices,
        double portfolio_value,
        double gross_notional,
        double net_notional,
        const StrategyInstancesMap& strategy_instances);

    /**
     * @brief Export the day's positions file on a day with NO session: every value carried.
     *
     * T-7a C3 (HD 2026-09-18). Same file name as export_current_positions
     * (YYYY-MM-DD_positions.csv) and the same columns, plus mark_date and forecast_run_date. A
     * second header line states that no session occurred and that every value is carried, not
     * computed; it describes the carried forecast as carried_forecast_label does ("computed by
     * the run of <D> from bars through <D-1>": a stored forecast carries its run's date).
     * One row per sleeve and symbol the sleeve holds or last forecast: quantity from the held
     * book, market_price the symbol's last mark (a held symbol with no loaded bar keeps its stored
     * row's price, mark_date "stored row"), forecast the sleeve's last computed forecast (empty if
     * the sleeve has none stored for that symbol). sizing_vol and the EMAs are not stored anywhere
     * and are left empty rather than written as zeros.
     *
     * @param date Run date (for the file name)
     * @param strategy_positions The held book per sleeve
     * @param last_marks Symbol -> its last loaded close and that bar's date
     * @param last_forecasts Sleeve -> its stored signals of the latest run date before today
     * @param no_session_reason Why there was no session (written into the note)
     * @return Result containing filename on success, or error
     */
    Result<std::string> export_carried_positions(
        const std::chrono::system_clock::time_point& date,
        const StrategyPositionsMap& strategy_positions,
        const std::unordered_map<std::string, CarriedMark>& last_marks,
        const std::unordered_map<std::string, CarriedForecasts>& last_forecasts,
        double portfolio_value, double gross_notional, double net_notional,
        const std::string& no_session_reason);

    /**
     * @brief Export yesterday's finalized positions to CSV with per-strategy breakdown
     *
     * Creates a file named: YYYY-MM-DD_positions_asof_YYYY-MM-DD.csv
     * Contains: strategy, symbol, quantity, prior_mark, mark, realized_pnl. prior_mark is the
     * close two days before the run date (the T-2 close) and mark the close of the day before
     * it (the T-1 close, the day the file settles); neither is an entry or an exit price. A
     * symbol with no close on that day has an empty cell. The file starts with its column
     * names (no comment line); quantity and realized_pnl print six decimals on every row, the
     * two marks the shortest text that reads back as the same number.
     *
     * @param date Current trading date (for filename)
     * @param yesterday_date Yesterday's date
     * @param strategy_positions Per-strategy positions map
     * @param prior_marks Day T-2 close prices
     * @param marks Day T-1 close prices
     * @return Result containing filename on success, or error
     */
    Result<std::string> export_finalized_positions(
        const std::chrono::system_clock::time_point& date,
        const std::chrono::system_clock::time_point& yesterday_date,
        const StrategyPositionsMap& strategy_positions,
        const std::unordered_map<std::string, double>& prior_marks,
        const std::unordered_map<std::string, double>& marks);

    /**
     * @brief Set the output directory for CSV files
     * @param directory Path to output directory
     */
    void set_output_directory(const std::string& directory);

    /// The positions file's one-line explanation of its sizing_vol column.
    static std::string sizing_vol_note();

    /// The positions file's one-line statement of what pct_of_portfolio_value divides by.
    static std::string pct_of_portfolio_value_note();

private:
    std::string output_directory_;

    /**
     * @brief Format date for CSV filename (YYYY-MM-DD format)
     * @param date Date to format
     * @return Formatted date string
     */
    std::string format_date_for_filename(const std::chrono::system_clock::time_point& date) const;

    /**
     * @brief Format date for display (YYYY-MM-DD format)
     * @param date Date to format
     * @return Formatted date string
     */
    std::string format_date_for_display(const std::chrono::system_clock::time_point& date) const;

    /**
     * @brief Calculate notional value for a position
     * @param symbol Symbol name
     * @param quantity Position quantity
     * @param price Market price
     * @return Notional value (quantity * price * multiplier)
     */
    double calculate_notional(const std::string& symbol, double quantity, double price) const;

    /**
     * @brief Write portfolio-level header comment to CSV file
     * @param file Output file stream
     * @param portfolio_value Portfolio value
     * @param gross_notional Gross notional
     * @param net_notional Net notional
     * @param date Trading date
     */
    void write_portfolio_header(std::ofstream& file, double portfolio_value, double gross_notional,
                                double net_notional, const std::string& date) const;

    /**
     * @brief Get clean symbol for instrument registry lookup
     * @param symbol Full symbol with version suffix
     * @return Clean symbol without version
     */
    std::string get_clean_symbol(const std::string& symbol) const;

    /**
     * @brief Format strategy ID to display name
     * @param strategy_id Strategy identifier (e.g., "TREND_FOLLOWING_FAST")
     * @return Display name (e.g., "Trend Following Fast")
     */
    std::string format_strategy_display_name(const std::string& strategy_id) const;
};

}  // namespace trade_ngin