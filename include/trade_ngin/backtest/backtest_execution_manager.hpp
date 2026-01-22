#pragma once

#include <memory>
#include <map>
#include <vector>
#include <optional>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/backtest/slippage_models.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

namespace trade_ngin {
namespace backtest {

/**
 * @brief Configuration for execution generation
 */
struct BacktestExecutionConfig {
    // Legacy config (kept for compatibility, but not used when use_new_cost_model=true)
    double commission_rate = 0.0005;   // Per-share commission rate
    double slippage_bps = 1.0;         // Slippage in basis points (for basic model)
    double market_impact_bps = 5.0;    // Market impact in basis points
    double fixed_cost_per_trade = 1.0; // Fixed cost per trade

    // New cost model settings
    bool use_new_cost_model = true;    // Use TransactionCostManager

    // Explicit fee per contract (broker + exchange + clearing + regulatory)
    double explicit_fee_per_contract = 1.75;
};

/**
 * @brief Manages execution generation for backtesting
 *
 * This class extracts execution logic from BacktestEngine including:
 * - Position change detection and execution generation
 * - Slippage application (via model or basic)
 * - Transaction cost calculation
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
     * @brief Constructor with slippage model
     */
    BacktestExecutionManager(const BacktestExecutionConfig& config,
                             std::unique_ptr<SlippageModel> slippage_model);

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
     * @param current_bars Current day's bars (for slippage model context)
     * @param timestamp Timestamp for executions
     * @return Vector of generated executions
     */
    std::vector<ExecutionReport> generate_executions(
        const std::map<std::string, Position>& current_positions,
        const std::map<std::string, Position>& new_positions,
        const std::unordered_map<std::string, double>& execution_prices,
        const std::vector<Bar>& current_bars,
        const Timestamp& timestamp);

    /**
     * @brief Generate a single execution
     *
     * @param symbol Symbol to trade
     * @param quantity_change Signed quantity change (+ for buy, - for sell)
     * @param execution_price Base price before slippage
     * @param symbol_bar Optional bar for slippage model context
     * @param timestamp Timestamp for the execution
     * @return Generated ExecutionReport
     */
    ExecutionReport generate_execution(
        const std::string& symbol,
        double quantity_change,
        double execution_price,
        const std::optional<Bar>& symbol_bar,
        const Timestamp& timestamp);

    /**
     * @brief Calculate transaction costs for an execution
     * @param execution The execution to calculate costs for
     * @return Total transaction cost
     */
    double calculate_transaction_costs(const ExecutionReport& execution) const;

    /**
     * @brief Calculate commission only
     * @param quantity Trade quantity
     * @return Commission cost
     */
    double calculate_commission(double quantity) const;

    /**
     * @brief Apply slippage to a price
     * @param price Original price
     * @param quantity Trade quantity (for volume-based slippage)
     * @param side Trade side
     * @param symbol_bar Optional bar for context
     * @return Price with slippage applied
     */
    double apply_slippage(
        double price,
        double quantity,
        Side side,
        const std::optional<Bar>& symbol_bar = std::nullopt) const;

    /**
     * @brief Set the slippage model
     */
    void set_slippage_model(std::unique_ptr<SlippageModel> model);

    /**
     * @brief Check if using advanced slippage model
     */
    bool has_slippage_model() const { return slippage_model_ != nullptr; }

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

    /**
     * @brief Check if using new transaction cost model
     */
    bool is_using_new_cost_model() const { return config_.use_new_cost_model; }

private:
    BacktestExecutionConfig config_;
    std::unique_ptr<SlippageModel> slippage_model_;
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
