#pragma once

#include "trade_ngin/live/pnl_manager_base.hpp"
#include "trade_ngin/core/types.hpp"
#include <memory>

namespace trade_ngin {

// Forward declaration
// class InstrumentRegistry;  // TODO: Add when available

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

    double cumulative_daily_pnl_ = 0.0;
    double cumulative_total_pnl_ = 0.0;

    // Reference to instrument registry for point values
    // std::shared_ptr<InstrumentRegistry> instrument_registry_;  // TODO: Add when available

public:
    /**
     * Constructor
     */
    explicit LivePnLManager(double initial_capital = 500000.0)
        : PnLManagerBase(initial_capital) {}

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
     */
    double get_point_value(const std::string& symbol) const;
};

} // namespace trade_ngin