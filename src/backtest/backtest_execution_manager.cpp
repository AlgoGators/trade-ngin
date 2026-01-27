#include "trade_ngin/backtest/backtest_execution_manager.hpp"
#include <cmath>

namespace trade_ngin {
namespace backtest {

namespace {
// Initialize transaction cost manager config from execution config
transaction_cost::TransactionCostManager::Config make_tc_config(
    const BacktestExecutionConfig& config) {
    transaction_cost::TransactionCostManager::Config tc_config;
    tc_config.explicit_fee_per_contract = config.explicit_fee_per_contract;
    return tc_config;
}
}  // namespace

BacktestExecutionManager::BacktestExecutionManager(const BacktestExecutionConfig& config)
    : config_(config),
      transaction_cost_manager_(make_tc_config(config)),
      execution_counter_(0) {}

std::vector<ExecutionReport> BacktestExecutionManager::generate_executions(
    const std::map<std::string, Position>& current_positions,
    const std::map<std::string, Position>& new_positions,
    const std::unordered_map<std::string, double>& execution_prices,
    const Timestamp& timestamp) {

    std::vector<ExecutionReport> executions;

    // Process each new position
    for (const auto& [symbol, new_pos] : new_positions) {
        // Get current quantity (0 if not present)
        double current_qty = 0.0;
        auto current_it = current_positions.find(symbol);
        if (current_it != current_positions.end()) {
            current_qty = static_cast<double>(current_it->second.quantity);
        }

        double new_qty = static_cast<double>(new_pos.quantity);
        double quantity_change = new_qty - current_qty;

        // Skip if no meaningful change
        if (std::abs(quantity_change) < 1e-4) {
            continue;
        }

        // Get execution price
        auto price_it = execution_prices.find(symbol);
        if (price_it == execution_prices.end() || price_it->second <= 0.0) {
            continue;  // Skip if no valid price
        }
        double execution_price = price_it->second;

        // Generate the execution
        auto exec = generate_execution(symbol, quantity_change, execution_price, timestamp);
        executions.push_back(exec);
    }

    return executions;
}

ExecutionReport BacktestExecutionManager::generate_execution(
    const std::string& symbol,
    double quantity_change,
    double execution_price,
    const Timestamp& timestamp) {

    Side side = quantity_change > 0 ? Side::BUY : Side::SELL;
    double abs_quantity = std::abs(quantity_change);

    // Create execution report
    ExecutionReport exec;
    exec.order_id = generate_order_id();
    exec.exec_id = generate_exec_id();
    exec.symbol = symbol;
    exec.side = side;
    exec.filled_quantity = Quantity(abs_quantity);
    exec.fill_time = timestamp;
    exec.is_partial = false;

    // TransactionCostManager is the single source of truth.
    // Fill price stays as pure reference price (no embedded slippage).
    exec.fill_price = Price(execution_price);

    auto cost_result = transaction_cost_manager_.calculate_costs(
        symbol, abs_quantity, execution_price);

    exec.commissions_fees = Decimal(cost_result.commissions_fees);
    exec.implicit_price_impact = Decimal(cost_result.implicit_price_impact);
    exec.slippage_market_impact = Decimal(cost_result.slippage_market_impact);
    exec.total_transaction_costs = Decimal(cost_result.total_transaction_costs);

    return exec;
}

void BacktestExecutionManager::reset() {
    execution_counter_ = 0;
    transaction_cost_manager_.clear_all_data();
}

void BacktestExecutionManager::update_market_data(
    const std::string& symbol,
    double volume,
    double close_price,
    double prev_close_price) {
    transaction_cost_manager_.update_market_data(symbol, volume, close_price, prev_close_price);
}

double BacktestExecutionManager::get_adv(const std::string& symbol) const {
    return transaction_cost_manager_.get_adv(symbol);
}

std::string BacktestExecutionManager::generate_order_id() {
    return "BT-" + std::to_string(execution_counter_);
}

std::string BacktestExecutionManager::generate_exec_id() {
    return "EX-" + std::to_string(execution_counter_++);
}

} // namespace backtest
} // namespace trade_ngin
