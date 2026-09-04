#pragma once

#include "trade_ngin/live/pnl_manager_base.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include <memory>
#include <functional>

namespace trade_ngin {

/**
 * Live implementation of PnL Manager
 * Handles Day T-1 finalization and position PnL calculations
 * This extracts ~200+ lines of PnL logic from live_trend.cpp
 */
class LivePnLManager : public PnLManagerBase {
private:
    // PnL tracking
    std::unordered_map<std::string, double> position_daily_pnl_;
    std::unordered_map<std::string, double> position_realized_pnl_;
    std::unordered_map<std::string, double> position_unrealized_pnl_;

    double cumulative_daily_pnl_ = 0.0;
    double cumulative_total_pnl_ = 0.0;

    // Reference to instrument registry for point values
    InstrumentRegistry& registry_;

public:
    /**
     * Constructor
     * @param initial_capital Starting capital
     * @param registry Reference to instrument registry
     */
    explicit LivePnLManager(double initial_capital, InstrumentRegistry& registry)
        : PnLManagerBase(initial_capital), registry_(registry) {}

    /**
     * @brief Unrealized P&L of one position against its own cost basis.
     *
     * The single rule for "what is this position worth versus what it cost", shared by
     * the live_results aggregate this manager produces and the per-row
     * trading.positions.unrealized_pnl the equity runner persists. It is a named
     * function because those two used to disagree: the aggregate was computed from
     * Position::average_price while the persisted row read
     * MeanReversionInstrumentData::entry_price, a field with no writer anywhere in the
     * tree. It was therefore always 0.0, so every persisted row said 0 while the
     * aggregate said otherwise -- a guaranteed log-versus-DB mismatch on any day with
     * open positions.
     *
     * Returns 0.0 when there is no basis to measure against: a zero quantity, or an
     * average_price not yet set (on_execution is its sole writer, so a fill that has not
     * been processed yet leaves it at 0).
     *
     * It deliberately does NOT screen mark_price. That is the caller's business and the
     * two callers differ: this manager skips a position whose price is absent, while the
     * equity runner defaults a missing close to 0.0 and must check it itself. Folding an
     * `mark_price > 0` guard in here would have quietly changed what the futures path
     * reports for a present-but-zero mark, which is outside this fix's scope.
     */
    static double unrealized_from_cost_basis(double quantity, double average_price,
                                             double mark_price, double point_value = 1.0) {
        if (quantity == 0.0 || average_price <= 0.0) return 0.0;
        return quantity * (mark_price - average_price) * point_value;
    }

    /**
     * Finalization result structure for Day T-1
     */
    struct FinalizationResult {
        double finalized_daily_pnl = 0.0;
        double finalized_portfolio_value = 0.0;
        std::unordered_map<std::string, double> position_realized_pnl;
        std::vector<Position> finalized_positions;
        bool success = false;
    };

    /**
     * Finalize previous day (T-1) positions
     * This is unique to live trading and handles the settlement process
     * Replaces the finalization logic in live_trend.cpp (lines ~600-700)
     */
    Result<FinalizationResult> finalize_previous_day(
        const std::vector<Position>& previous_positions,
        const std::unordered_map<std::string, double>& t1_close_prices,
        const std::unordered_map<std::string, double>& t2_close_prices,
        double previous_portfolio_value,
        double commissions = 0.0);

    /**
     * Calculate PnL for current day positions
     * Replaces the position PnL calculation logic in live_trend.cpp
     */
    Result<void> calculate_position_pnls(
        const std::vector<Position>& positions,
        const std::unordered_map<std::string, double>& current_prices,
        const std::unordered_map<std::string, double>& previous_prices);

    /**
     * Update position PnL
     */
    Result<void> update_position_pnl(
        const std::string& symbol,
        double daily_pnl,
        double realized_pnl = 0.0);

    /**
     * Get current PnL snapshot
     */
    Result<PnLSnapshot> get_current_snapshot() const;

    /**
     * Get daily PnL for a specific symbol
     */
    double get_position_daily_pnl(const std::string& symbol) const;

    /**
     * Get realized PnL for a specific symbol
     */
    double get_position_realized_pnl(const std::string& symbol) const;

    /**
     * Get total daily PnL across all positions
     */
    double get_total_daily_pnl() const {
        return cumulative_daily_pnl_;
    }

    /**
     * Get total cumulative PnL
     */
    double get_total_pnl() const {
        return cumulative_total_pnl_;
    }

    /**
     * Clear all PnL tracking for new day
     */
    void reset_daily_tracking() {
        position_daily_pnl_.clear();
        position_realized_pnl_.clear();
        position_unrealized_pnl_.clear();
        cumulative_daily_pnl_ = 0.0;
    }

    /**
     * Set cumulative total PnL (for initialization)
     */
    void set_total_pnl(double total_pnl) {
        cumulative_total_pnl_ = total_pnl;
    }

    /**
     * Helper to get point value for a symbol
     * Uses InstrumentRegistry with accurate fallback values
     */
    double get_point_value(const std::string& symbol) const;

private:
    /**
     * Get fallback multiplier for known symbols
     * These are calculated as: minimum_price_fluctuation / tick_size
     */
    double get_fallback_multiplier(const std::string& symbol) const;
};

} // namespace trade_ngin