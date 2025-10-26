#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include <vector>
#include <unordered_map>

namespace trade_ngin {

/**
 * Base class for PnL management - extracts PnL calculation logic from live_trend.cpp
 * Goal: Modularize PnL calculations to reduce monolithic code
 */
class PnLManagerBase {
protected:
    double initial_capital_;

public:
    /**
     * PnL snapshot structure
     */
    struct PnLSnapshot {
        double daily_pnl = 0.0;
        double total_pnl = 0.0;
        double realized_pnl = 0.0;
        double unrealized_pnl = 0.0;
        double portfolio_value = 0.0;
        Timestamp timestamp;
    };

    /**
     * Position PnL data
     */
    struct PositionPnL {
        std::string symbol;
        double quantity = 0.0;
        double entry_price = 0.0;
        double current_price = 0.0;
        double previous_close = 0.0;
        double point_value = 0.0;
        double daily_pnl = 0.0;
        double total_pnl = 0.0;
    };

    explicit PnLManagerBase(double initial_capital = 500000.0)
        : initial_capital_(initial_capital) {}

    virtual ~PnLManagerBase() = default;

    /**
     * Core PnL calculations - same for both live and backtest
     */

    // Calculate position PnL from entry
    virtual double calculate_position_pnl(
        double quantity,
        double entry_price,
        double current_price,
        double point_value) const {
        return quantity * (current_price - entry_price) * point_value;
    }

    // Calculate daily PnL for a position
    virtual double calculate_daily_pnl(
        double quantity,
        double previous_close,
        double current_close,
        double point_value) const {
        return quantity * (current_close - previous_close) * point_value;
    }

    // Calculate net PnL after costs
    virtual double calculate_net_pnl(
        double gross_pnl,
        double commissions,
        double slippage = 0.0) const {
        return gross_pnl - commissions - slippage;
    }

    // Calculate portfolio value
    virtual double calculate_portfolio_value(
        double previous_value,
        double daily_pnl) const {
        return previous_value + daily_pnl;
    }

    // Get initial capital
    double get_initial_capital() const {
        return initial_capital_;
    }

    // Set initial capital
    void set_initial_capital(double capital) {
        initial_capital_ = capital;
    }
};

} // namespace trade_ngin