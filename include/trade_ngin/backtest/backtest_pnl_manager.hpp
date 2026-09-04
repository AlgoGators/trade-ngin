#pragma once

#include <cmath>

#include "trade_ngin/live/pnl_manager_base.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/strategy/types.hpp"  // PnLAccountingMethod (E2-F2)
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
    // Bring base class overloads into scope to avoid hiding them
    using PnLManagerBase::calculate_daily_pnl;
    using PnLManagerBase::calculate_position_pnl;

    /**
     * Constructor
     * @param initial_capital Starting capital
     * @param registry Reference to instrument registry for point value lookups
     */
    explicit BacktestPnLManager(double initial_capital, InstrumentRegistry& registry)
        : PnLManagerBase(initial_capital)
        , current_portfolio_value_(initial_capital)
        , registry_(registry) {
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
        double total_transaction_costs = 0.0;
        double net_daily_pnl = 0.0;
        double new_portfolio_value = 0.0;
        std::unordered_map<std::string, PositionPnLResult> position_results;
        std::string date_str;
        bool success = false;
    };
    
    /**
     * @brief The unrealized P&L to record on a backtest position row, per the
     *        strategy's own settlement model. E2-F2.
     *
     * REALIZED_ONLY (futures) returns 0.0 -- an identity of the model, not a
     * convention. Under daily settlement the position is never carried at an
     * unsettled price, and on a futures row `average_price` IS the prior
     * settlement close: TrendFollowingStrategy resets it to current_price after
     * settling (trend_following.cpp:550-556) and get_target_positions() stamps
     * price_history.back() (:610-624), while the coordinator feeds on_data the
     * T-1 bars (backtest_coordinator.cpp:547). So
     * `quantity * (mark - average_price) * point_value` reduces ALGEBRAICALLY to
     * the realized formula, and writing it into unrealized records the same
     * settled move in two columns.
     *
     * That is exactly what this codebase did until E2-F2: the coordinator gated
     * realized on the accounting method but left unrealized ungated, and its
     * inline expression also omitted point_value -- so a futures row carried the
     * settled move DIVIDED by the contract multiplier. Measured at 2,769 of 3,064
     * rows on TREND_FOLLOWING (gross magnitude $693,172.27, net -$28,315.21), and
     * it read as an exact 1/point_value ratio against realized on every symbol:
     * MYM 2.0 (pv 0.5), MES 0.2 (pv 5), ZN 0.001 (pv 1000), 6A 0.00001 (pv 1e5).
     * `main` had `unrealized_pnl = Decimal(0.0)` unconditionally; the regression
     * entered with 76b4ea5d.
     *
     * This is the backtest counterpart of LivePnLManager::UnrealizedPolicy
     * (SETTLED / MARK_TO_MARKET), which fixed the identical defect on the live
     * side in 5b589ac2 (E2-F3). Live and backtest MUST agree; before this they
     * did not.
     *
     * DO NOT "restore" an ungated unrealized here, and do not drop point_value
     * from the MIXED branch. If futures ever legitimately needs a non-zero
     * unrealized, the settlement model has changed and `average_price` must stop
     * being the prior close first.
     *
     * @param method    The strategy's PnL accounting method.
     * @param quantity  Signed position quantity.
     * @param average_price Cost basis for MIXED/UNREALIZED_ONLY. Ignored under
     *                  REALIZED_ONLY. <= 0 means "no basis known" -> 0.0.
     * @param mark_price Mark to measure against (the day's close).
     * @param point_value Contract multiplier; 1.0 for equities, applied
     *                  explicitly rather than assumed.
     */
    static double unrealized_for_accounting(PnLAccountingMethod method,
                                            double quantity,
                                            double average_price,
                                            double mark_price,
                                            double point_value) {
        if (method == PnLAccountingMethod::REALIZED_ONLY) return 0.0;
        if (quantity == 0.0 || average_price <= 0.0) return 0.0;
        return quantity * (mark_price - average_price) * point_value;
    }

    /**
     * @brief What `realized_pnl` a stored backtest row carries, by accounting method.
     *
     * Phase 4 audit T4.6 / §1.14, and the pin C-5 §9-A2 found missing. The coordinator
     * branches here and the two answers are different quantities, not two spellings of one:
     *
     *   REALIZED_ONLY (futures)  -- the day's settled mark-to-market IS the day's realized.
     *                               There is no separate cost basis to close against.
     *   MIXED / UNREALIZED_ONLY  -- realized is booked by on_execution when a position
     *                               actually closes, and the row carries that bar's FLOW.
     *                               The settled move belongs in unrealized, not here.
     *
     * Stamping the MTM figure on an equity row was the defect: it made every held day look
     * like a realizing day and made the column uncorrelated with the fills.
     *
     * @param method    the strategy's accounting method.
     * @param daily_mtm the bar's settled mark-to-market move, dollarised.
     * @param flow      the realized increment from this bar's fills (realized_row_for_bar).
     */
    static double realized_for_row(PnLAccountingMethod method, double daily_mtm, double flow) {
        return method == PnLAccountingMethod::REALIZED_ONLY ? daily_mtm : flow;
    }

    /**
     * @brief The stored row's per-bar realized FLOW, and whether the row survives.
     *
     * E2-F54. `backtest.final_positions.realized_pnl` is a FLOW -- what this position
     * realized on THIS bar's date -- exactly as `trading.positions.daily_realized_pnl` is
     * live (E2-F19, docs/AVERAGE_PRICE_LIFECYCLE.md). The strategy's own record is a
     * running total over the whole backtest, so the row is the increment since the
     * previous bar.
     *
     * Two ordering constraints are why this is a named function rather than three lines
     * inside the bar loop:
     *
     *   1. `cumulative` MUST be read from the strategy's fill-maintained holdings
     *      (BaseStrategy::get_positions(), written by on_execution), NOT from the target
     *      snapshot the loop iterates. The target copy is taken before on_execution runs,
     *      so a sale on bar D would otherwise land on D+1.
     *
     *   2. This MUST be called before any flat-row skip. A full close is the bar with the
     *      largest realized and a quantity of zero; skipping it first strands the exit's
     *      P&L until the symbol is re-entered, or loses it entirely.
     *
     * @param quantity            Signed position quantity AFTER this bar's fills.
     * @param cumulative_from_fills The strategy's running realized total for this symbol.
     * @param last_cumulative     In/out: the value at the previous bar. Advanced here.
     *                            Cleared by BacktestCoordinator::reset_portfolio_state();
     *                            an uncleared ledger opens the next portfolio's first bar
     *                            with the previous book's total and reports a difference.
     * @param tol                 Dead-row tolerance, matching LiveDailyCycle::is_dead_row.
     */
    struct RealizedRow {
        double flow = 0.0;   ///< what realized_pnl on this row must carry
        bool keep = false;   ///< false => dead row, do not write or persist it
    };

    static RealizedRow realized_row_for_bar(double quantity,
                                            double cumulative_from_fills,
                                            double& last_cumulative,
                                            double tol = 1e-8) {
        RealizedRow row;
        row.flow = cumulative_from_fills - last_cumulative;
        last_cumulative = cumulative_from_fills;
        row.keep = !is_dead_row(quantity, row.flow, tol);
        return row;
    }

    /**
     * @brief The live dead-row rule, restated for the backtest.
     *
     * LiveDailyCycle::is_dead_row: a row dies only when it has NEITHER quantity NOR
     * realized. A position closed to zero that realized something keeps its row for that
     * date, so the exit's P&L has somewhere to live and the flows sum to the cumulative.
     */
    static bool is_dead_row(double quantity, double realized, double tol = 1e-8) {
        return std::abs(quantity) <= tol && std::abs(realized) <= tol;
    }

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

