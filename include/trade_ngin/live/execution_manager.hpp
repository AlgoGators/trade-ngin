#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include <vector>
#include <unordered_map>
#include <string>
#include <chrono>

namespace trade_ngin {

/**
 * ExecutionManager - Handles execution generation for live trading
 *
 * This class encapsulates the logic for:
 * - Generating execution reports from position changes
 * - Calculating commissions and transaction costs
 * - Applying slippage models
 *
 * Extracted from live_trend.cpp lines 717-833 as part of Phase 3 refactoring
 */
class ExecutionManager {
private:
    double commission_rate_;     // Commission rate per contract (e.g., 2.25)
    double slippage_bps_;       // Slippage in basis points (e.g., 1.0 for 1 bp)
    double market_impact_bps_;  // Market impact in basis points (e.g., 5.0 for 5 bps)
    double fixed_cost_per_trade_; // Fixed cost per trade (e.g., 1.0)

public:
    /**
     * Constructor with default values matching live_trend.cpp
     */
    explicit ExecutionManager(
        double commission_rate = 2.25,
        double slippage_bps = 1.0,
        double market_impact_bps = 5.0,
        double fixed_cost = 1.0)
        : commission_rate_(commission_rate),
          slippage_bps_(slippage_bps),
          market_impact_bps_(market_impact_bps),
          fixed_cost_per_trade_(fixed_cost) {}

    /**
     * Configuration structure for execution parameters
     */
    struct ExecutionConfig {
        double commission_rate = 2.25;
        double slippage_bps = 1.0;
        double market_impact_bps = 5.0;
        double fixed_cost = 1.0;
    };

    /**
     * Alternative constructor with config
     */
    explicit ExecutionManager(const ExecutionConfig& config)
        : commission_rate_(config.commission_rate),
          slippage_bps_(config.slippage_bps),
          market_impact_bps_(config.market_impact_bps),
          fixed_cost_per_trade_(config.fixed_cost) {}

    /**
     * Generate execution reports for daily position changes
     *
     * @param current_positions Current day's positions
     * @param previous_positions Previous day's positions
     * @param market_prices Market prices (typically T-1 close prices)
     * @param timestamp Execution timestamp
     * @return Vector of execution reports
     */
    Result<std::vector<ExecutionReport>> generate_daily_executions(
        const std::unordered_map<std::string, Position>& current_positions,
        const std::unordered_map<std::string, Position>& previous_positions,
        const std::unordered_map<std::string, double>& market_prices,
        const Timestamp& timestamp);

    /**
     * Generate a single execution report
     *
     * @param symbol Symbol being traded
     * @param quantity_change Change in position quantity (positive for buy, negative for sell)
     * @param market_price Market price for the symbol
     * @param timestamp Execution timestamp
     * @param exec_sequence Sequence number for unique exec_id
     * @return Single execution report
     */
    ExecutionReport generate_execution(
        const std::string& symbol,
        double quantity_change,
        double market_price,
        const Timestamp& timestamp,
        size_t exec_sequence);

    /**
     * Calculate transaction cost for a trade
     *
     * @param quantity Absolute quantity traded
     * @param price Execution price
     * @return Total transaction cost including commission, market impact, and fixed costs
     */
    double calculate_transaction_cost(double quantity, double price) const;

    /**
     * Apply slippage to market price
     *
     * @param market_price Base market price
     * @param side Trade side (BUY or SELL)
     * @return Price after slippage
     */
    double apply_slippage(double market_price, Side side) const;

    /**
     * Generate date string for order IDs
     *
     * @param timestamp Timestamp to convert
     * @return Date string in YYYYMMDD format
     */
    static std::string generate_date_string(const Timestamp& timestamp);

    /**
     * Generate unique execution ID
     *
     * @param symbol Trading symbol
     * @param timestamp Execution timestamp
     * @param sequence Sequence number
     * @return Unique execution ID
     */
    static std::string generate_exec_id(
        const std::string& symbol,
        const Timestamp& timestamp,
        size_t sequence);

    // Getters for configuration
    double get_commission_rate() const { return commission_rate_; }
    double get_slippage_bps() const { return slippage_bps_; }
    double get_market_impact_bps() const { return market_impact_bps_; }
    double get_fixed_cost() const { return fixed_cost_per_trade_; }

    // Setters for configuration
    void set_commission_rate(double rate) { commission_rate_ = rate; }
    void set_slippage_bps(double bps) { slippage_bps_ = bps; }
    void set_market_impact_bps(double bps) { market_impact_bps_ = bps; }
    void set_fixed_cost(double cost) { fixed_cost_per_trade_ = cost; }
};

} // namespace trade_ngin