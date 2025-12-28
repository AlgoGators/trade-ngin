#pragma once

#include "trade_ngin/live/pnl_manager_base.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include <memory>
#include <unordered_map>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace trade_ngin {
namespace backtest {

/**
 * Backtest PnL Manager - Single Source of Truth for Backtest PnL Calculations
 * 
 * This class centralizes ALL PnL calculations for the backtest engine to ensure:
 * 1. Consistent date alignment (PnL for date T uses close[T] - close[T-1])
 * 2. Proper quantity application (quantity * price_change * point_value)
 * 3. Consistent point value multiplier usage (from InstrumentRegistry)
 * 
 * Formula: Daily PnL = quantity * (close_T - close_T-1) * point_value
 * 
 * Where:
 *   - close_T = closing price for the current bar date
 *   - close_T-1 = closing price for the previous bar date  
 *   - point_value = minimum price fluctuation value from InstrumentRegistry
 *   - quantity = number of contracts (can be negative for shorts)
 * 
 * DEBUG LOGGING: All PnL calculations are logged with prefix "[BACKTEST_PNL]" 
 * to allow easy verification. Look for these log tags:
 *   - [BACKTEST_PNL] CALC: Individual PnL calculations
 *   - [BACKTEST_PNL] POINT_VALUE: Point value lookups
 *   - [BACKTEST_PNL] DAILY_TOTAL: Daily total PnL
 *   - [BACKTEST_PNL] PORTFOLIO: Portfolio value updates
 *   - [BACKTEST_PNL] POSITION: Position PnL updates
 */
class BacktestPnLManager : public PnLManagerBase {
private:
    // Previous day's close prices for each symbol
    std::unordered_map<std::string, double> previous_close_prices_;
    
    // Current day's PnL for each position
    std::unordered_map<std::string, double> position_daily_pnl_;
    
    // Cumulative PnL for each position
    std::unordered_map<std::string, double> position_cumulative_pnl_;
    
    // Daily totals
    double daily_total_pnl_ = 0.0;
    double cumulative_total_pnl_ = 0.0;
    double current_portfolio_value_ = 0.0;
    
    // Current date for debugging
    std::string current_date_str_;
    
    // Reference to instrument registry for point values
    InstrumentRegistry& registry_;
    
    // Debug flag to control verbose logging
    bool debug_enabled_ = true;

public:
    /**
     * Constructor
     * @param initial_capital Starting capital
     * @param registry Reference to instrument registry for point value lookups
     */
    explicit BacktestPnLManager(double initial_capital, InstrumentRegistry& registry)
        : PnLManagerBase(initial_capital)
        , registry_(registry)
        , current_portfolio_value_(initial_capital) {
        log_debug("[BACKTEST_PNL] Initialized with capital=" + std::to_string(initial_capital));
    }
    
    /**
     * Result structure for a single position's PnL calculation
     */
    struct PositionPnLResult {
        std::string symbol;
        double quantity = 0.0;
        double previous_close = 0.0;
        double current_close = 0.0;
        double point_value = 1.0;
        double daily_pnl = 0.0;
        bool valid = false;
        std::string error_message;
    };
    
    /**
     * Result structure for daily PnL calculation
     */
    struct DailyPnLResult {
        double total_daily_pnl = 0.0;
        double total_commissions = 0.0;
        double net_daily_pnl = 0.0;
        double new_portfolio_value = 0.0;
        std::unordered_map<std::string, PositionPnLResult> position_results;
        std::string date_str;
        bool success = false;
    };
    
    /**
     * MAIN ENTRY POINT: Calculate daily PnL for all positions
     * 
     * This is THE method that should be called to calculate PnL for a given day.
     * It ensures consistent application of:
     * - Date alignment (uses T and T-1 closes correctly)
     * - Quantity scaling
     * - Point value multiplier
     * 
     * @param timestamp Current bar timestamp (Day T)
     * @param positions Current positions map
     * @param current_close_prices Close prices for Day T
     * @param commissions Total commissions for this day
     * @return DailyPnLResult with all calculation details
     */
    DailyPnLResult calculate_daily_pnl(
        const Timestamp& timestamp,
        const std::unordered_map<std::string, Position>& positions,
        const std::unordered_map<std::string, double>& current_close_prices,
        double commissions = 0.0);
    
    /**
     * Calculate PnL for a single position
     * 
     * Formula: daily_pnl = quantity * (current_close - previous_close) * point_value
     * 
     * @param symbol Position symbol
     * @param quantity Position quantity (can be negative for shorts)
     * @param previous_close Previous day's close price
     * @param current_close Current day's close price
     * @return PositionPnLResult with calculation details
     */
    PositionPnLResult calculate_position_pnl(
        const std::string& symbol,
        double quantity,
        double previous_close,
        double current_close);
    
    /**
     * Get point value multiplier for a symbol
     * 
     * Uses InstrumentRegistry first, then falls back to known values.
     * All lookups are logged for debugging.
     * 
     * @param symbol Full symbol (e.g., "MYM.v.0")
     * @return Point value multiplier (e.g., 0.5 for MYM)
     */
    double get_point_value(const std::string& symbol) const;
    
    /**
     * Update previous close prices for next day's calculation
     * Call this AFTER processing each day's PnL
     * 
     * @param close_prices Map of symbol to close price
     */
    void update_previous_closes(const std::unordered_map<std::string, double>& close_prices);
    
    /**
     * Set previous close price for a single symbol
     */
    void set_previous_close(const std::string& symbol, double close_price);
    
    /**
     * Get previous close price for a symbol
     */
    double get_previous_close(const std::string& symbol) const;
    
    /**
     * Check if we have a previous close for a symbol
     */
    bool has_previous_close(const std::string& symbol) const;
    
    /**
     * Reset all tracking for a new backtest run
     */
    void reset();
    
    /**
     * Reset daily tracking (called at start of each new day)
     */
    void reset_daily();
    
    /**
     * Get current portfolio value
     */
    double get_portfolio_value() const { return current_portfolio_value_; }
    
    /**
     * Set portfolio value (for initialization or adjustments)
     */
    void set_portfolio_value(double value) { current_portfolio_value_ = value; }
    
    /**
     * Get daily PnL for a specific position
     */
    double get_position_daily_pnl(const std::string& symbol) const;
    
    /**
     * Get cumulative PnL for a specific position
     */
    double get_position_cumulative_pnl(const std::string& symbol) const;
    
    /**
     * Get total daily PnL across all positions
     */
    double get_daily_total_pnl() const { return daily_total_pnl_; }
    
    /**
     * Get cumulative total PnL
     */
    double get_cumulative_total_pnl() const { return cumulative_total_pnl_; }
    
    /**
     * Enable/disable debug logging
     */
    void set_debug_enabled(bool enabled) { debug_enabled_ = enabled; }
    
    /**
     * Get current date string (for debugging)
     */
    std::string get_current_date() const { return current_date_str_; }

private:
    /**
     * Extract base symbol (remove .v./.c. suffix)
     */
    std::string extract_base_symbol(const std::string& symbol) const;
    
    /**
     * Get fallback multiplier for known symbols
     * These are calculated as: minimum_price_fluctuation / tick_size
     */
    double get_fallback_multiplier(const std::string& base_symbol) const;
    
    /**
     * Format timestamp as date string for logging
     */
    std::string format_date(const Timestamp& ts) const;
    
    /**
     * Log debug message if debug is enabled
     */
    void log_debug(const std::string& message) const;
    
    /**
     * Log info message (always logged)
     */
    void log_info(const std::string& message) const;
    
    /**
     * Log warning message (always logged)
     */
    void log_warn(const std::string& message) const;
};

} // namespace backtest
} // namespace trade_ngin

