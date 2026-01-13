#pragma once

#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

// Forward declarations
class IDatabase;
class TrendFollowingStrategy;
class TrendFollowingSlowStrategy;
// Base interface for trend following strategies (both standard and slow)
class BaseStrategy;
typedef BaseStrategy
    ITrendFollowingStrategy;  // Use base class for compatibility with both strategy types
struct Position;

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
     * @brief Export current day positions to CSV (forward-looking, no PnL)
     *
     * Creates a file named: DD-MM-YYYY_positions.csv
     * Contains: symbol, quantity, market_price, notional, percentages, forecast, volatility, EMAs
     *
     * @param date Current trading date
     * @param positions Current positions map
     * @param market_prices Market prices for positions (Day T-1 close)
     * @param portfolio_value Current portfolio value
     * @param gross_notional Gross notional value
     * @param net_notional Net notional value
     * @param strategy Strategy instance for forecasts and EMAs
     * @param symbol_commissions Optional commission data by symbol
     * @return Result containing filename on success, or error
     */
    Result<std::string> export_current_positions(
        const std::chrono::system_clock::time_point& date,
        const std::unordered_map<std::string, Position>& positions,
        const std::unordered_map<std::string, double>& market_prices, double portfolio_value,
        double gross_notional, double net_notional, ITrendFollowingStrategy* strategy,
        const std::unordered_map<std::string, double>& symbol_commissions = {});

    /**
     * @brief Export yesterday's finalized positions to CSV (with PnL)
     *
     * Creates a file named: DD-MM-YYYY_positions_asof_DD-MM-YYYY.csv
     * Contains: symbol, quantity, entry_price, exit_price, realized_pnl
     *
     * @param date Current trading date (for filename)
     * @param yesterday_date Yesterday's date
     * @param db Database connection for querying finalized positions
     * @param fallback_positions Fallback positions if database query fails
     * @param entry_prices Day T-2 close prices (entry)
     * @param exit_prices Day T-1 close prices (exit)
     * @return Result containing filename on success, or error
     */
    Result<std::string> export_finalized_positions(
        const std::chrono::system_clock::time_point& date,
        const std::chrono::system_clock::time_point& yesterday_date, std::shared_ptr<IDatabase> db,
        const std::unordered_map<std::string, Position>& fallback_positions,
        const std::unordered_map<std::string, double>& entry_prices,  // T-2 prices
        const std::unordered_map<std::string, double>& exit_prices    // T-1 prices
    );

    /**
     * @brief Set the output directory for CSV files
     * @param directory Path to output directory
     */
    void set_output_directory(const std::string& directory);

private:
    std::string output_directory_;

    /**
     * @brief Format date for CSV filename (DD-MM-YYYY format)
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
};

}  // namespace trade_ngin