#pragma once

#include <memory>
#include <map>
#include <vector>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

namespace trade_ngin {
namespace backtest {

/**
 * @brief Configuration for execution generation
 */
struct BacktestExecutionConfig {
    // Explicit fee per contract (broker + exchange + clearing + regulatory)
    double explicit_fee_per_contract = 1.75;
};

/**
 * @brief Manages execution generation for backtesting
 *
 * This class extracts execution logic from BacktestEngine including:
 * - Position change detection and execution generation
 * - Transaction cost calculation via TransactionCostManager
 *
 * Key responsibilities:
 * - Generate ExecutionReport objects from position changes
 * - Apply slippage to execution prices
 * - Calculate commissions and transaction costs
 * - Track execution IDs
 */
class BacktestExecutionManager {
public:
    /**
     * @brief Constructor with config
     */
    explicit BacktestExecutionManager(const BacktestExecutionConfig& config);

    /**
    ~BacktestExecutionManager() = default;

    /**
     * @brief Generate executions from position changes
     *
     * Compares current and new positions to detect changes,
     * then generates ExecutionReport for each change.
     *
     * @param current_positions Current positions by symbol
     * @param new_positions Target positions by symbol
     * @param execution_prices Prices to use for executions (previous day close for BOD model)
     * @param timestamp Timestamp for executions
     * @return Vector of generated executions
     */
    std::vector<ExecutionReport> generate_executions(
        const std::map<std::string, Position>& current_positions,
        const std::map<std::string, Position>& new_positions,
        const std::unordered_map<std::string, double>& execution_prices,
        const Timestamp& timestamp);

    /**
     * @brief Generate a single execution
     *
     * @param symbol Symbol to trade
     * @param quantity_change Signed quantity change (+ for buy, - for sell)
     * @param execution_price Reference price (previous day close)
     * @param timestamp Timestamp for the execution
     * @return Generated ExecutionReport
     */
    ExecutionReport generate_execution(
        const std::string& symbol,
        double quantity_change,
        double execution_price,
        const Timestamp& timestamp);

    /**
     * @brief Reset execution counter and state
     */
    void reset();

    /**
     * @brief Get total executions generated
     */
    int get_execution_count() const { return execution_counter_; }

    /**
     * @brief Update market data for transaction cost calculation
     *
     * Call this daily to update rolling ADV and volatility for each symbol.
     *
     * @param symbol Instrument symbol
     * @param volume Today's trading volume
     * @param close_price Today's close price
     * @param prev_close_price Previous day's close price
     */
    void update_market_data(
        const std::string& symbol,
        double volume,
        double close_price,
        double prev_close_price);

    /**
     * @brief Get current ADV for a symbol
     */
    double get_adv(const std::string& symbol) const;

    /**
     * @brief Get the transaction cost manager (for external access if needed)
     */
    transaction_cost::TransactionCostManager& get_transaction_cost_manager() {
        return transaction_cost_manager_;
    }

private:
    BacktestExecutionConfig config_;
    transaction_cost::TransactionCostManager transaction_cost_manager_;
    int execution_counter_ = 0;

    /**
     * @brief Generate unique order ID
     */
    std::string generate_order_id();

    /**
     * @brief Generate unique execution ID
     */
    std::string generate_exec_id();
};

} // namespace backtest
} // namespace trade_ngin
