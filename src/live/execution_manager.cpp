#include "trade_ngin/live/execution_manager.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>
#include <sstream>
#include <iomanip>

namespace trade_ngin {

Result<std::vector<ExecutionReport>> ExecutionManager::generate_daily_executions(
    const std::unordered_map<std::string, Position>& current_positions,
    const std::unordered_map<std::string, Position>& previous_positions,
    const std::unordered_map<std::string, double>& market_prices,
    const Timestamp& timestamp) {

    INFO("Generating execution reports for position changes...");
    std::vector<ExecutionReport> daily_executions;

    // Handle existing positions that changed
    for (const auto& [symbol, current_position] : current_positions) {
        double current_qty = current_position.quantity.as_double();
        double prev_qty = 0.0;

        // Get previous quantity
        auto prev_it = previous_positions.find(symbol);
        if (prev_it != previous_positions.end()) {
            prev_qty = prev_it->second.quantity.as_double();
        }

        DEBUG("Checking " + symbol + " - Current: " + std::to_string(current_qty) +
              ", Previous: " + std::to_string(prev_qty) +
              ", Diff: " + std::to_string(std::abs(current_qty - prev_qty)));

        // Check if position changed
        if (std::abs(current_qty - prev_qty) > 1e-6) {
            double trade_size = current_qty - prev_qty;

            // Get market price (Day T-1 close price for Day T execution)
            double market_price = current_position.average_price.as_double();
            auto price_it = market_prices.find(symbol);
            if (price_it != market_prices.end()) {
                market_price = price_it->second;
            } else {
                WARN("No market price for " + symbol + ", using average price");
            }

            // Generate execution
            ExecutionReport exec = generate_execution(
                symbol, trade_size, market_price, timestamp, daily_executions.size());
            daily_executions.push_back(exec);

            INFO("Generated execution: " + symbol + " " +
                 (exec.side == Side::BUY ? "BUY" : "SELL") + " " +
                 std::to_string(exec.filled_quantity) + " at " +
                 std::to_string(exec.fill_price));
        }
    }

    // Handle completely closed positions
    for (const auto& [symbol, prev_position] : previous_positions) {
        if (current_positions.find(symbol) == current_positions.end() &&
            prev_position.quantity.as_double() != 0.0) {
            // This position was completely closed
            double prev_qty = prev_position.quantity.as_double();

            // Get market price (Day T-1 close price for closing on Day T)
            double market_price = prev_position.average_price.as_double(); // Default fallback
            auto price_it = market_prices.find(symbol);
            if (price_it != market_prices.end()) {
                market_price = price_it->second;
            } else {
                WARN("No market price for closed position " + symbol + ", using average price");
            }

            // Generate execution for closing (opposite side of position)
            double trade_size = -prev_qty; // Negative because we're closing
            ExecutionReport exec = generate_execution(
                symbol, trade_size, market_price, timestamp, daily_executions.size());
            daily_executions.push_back(exec);

            INFO("Generated execution for closed position: " + symbol + " " +
                 (exec.side == Side::BUY ? "BUY" : "SELL") + " " +
                 std::to_string(exec.filled_quantity) + " at " +
                 std::to_string(exec.fill_price));
        }
    }

    INFO("Generated " + std::to_string(daily_executions.size()) + " execution reports");
    return Result<std::vector<ExecutionReport>>(daily_executions);
}

ExecutionReport ExecutionManager::generate_execution(
    const std::string& symbol,
    double quantity_change,
    double market_price,
    const Timestamp& timestamp,
    size_t exec_sequence) {

    ExecutionReport exec;

    // Determine side
    Side side = quantity_change > 0 ? Side::BUY : Side::SELL;

    // Generate IDs
    std::string date_str = generate_date_string(timestamp);
    exec.order_id = "DAILY_" + symbol + "_" + date_str;
    exec.exec_id = generate_exec_id(symbol, timestamp, exec_sequence);

    // Set basic fields
    exec.symbol = symbol;
    exec.side = side;
    exec.filled_quantity = std::abs(quantity_change);
    exec.fill_time = timestamp;

    double abs_quantity = exec.filled_quantity.as_double();

    if (use_new_cost_model_ && cost_manager_) {
        // New model: use TransactionCostManager (same as backtest)
        // Keep fill_price as pure reference price (no embedded slippage)
        exec.fill_price = market_price;

        // Calculate costs via TransactionCostManager
        auto cost_result = cost_manager_->calculate_costs(
            symbol, abs_quantity, market_price);

        // Populate all cost fields from result
        exec.commissions_fees = Decimal(cost_result.commissions_fees);
        exec.implicit_price_impact = Decimal(cost_result.implicit_price_impact);
        exec.slippage_market_impact = Decimal(cost_result.slippage_market_impact);
        exec.total_transaction_costs = Decimal(cost_result.total_transaction_costs);

        DEBUG("New cost model for " + symbol + ": commissions=$" +
              std::to_string(cost_result.commissions_fees) +
              ", implicit_impact=" + std::to_string(cost_result.implicit_price_impact) +
              ", slippage=$" + std::to_string(cost_result.slippage_market_impact) +
              ", total=$" + std::to_string(cost_result.total_transaction_costs));
    } else {
        // Legacy model: apply slippage to price and use simple bps calculation
        exec.fill_price = apply_slippage(market_price, side);

        // Commissions/fees: commission_rate * quantity (explicit fees)
        double commissions = std::abs(abs_quantity) * commission_rate_;

        // Market impact: basis points * quantity * price (implicit cost in dollars)
        double market_impact = std::abs(abs_quantity) * market_price * (market_impact_bps_ / 10000.0);

        // Fixed cost per trade
        double fixed_cost = fixed_cost_per_trade_;

        // Total explicit fees = commissions + fixed cost
        double explicit_fees = commissions + fixed_cost;

        // Implicit costs = market impact (slippage is already applied to fill_price)
        double implicit_costs = market_impact;

        // Populate cost breakdown fields
        exec.commissions_fees = Decimal(explicit_fees);
        exec.implicit_price_impact = Decimal(0.0);  // Not used in legacy mode
        exec.slippage_market_impact = Decimal(implicit_costs);
        exec.total_transaction_costs = Decimal(explicit_fees + implicit_costs);
    }

    exec.is_partial = false;

    return exec;
}

void ExecutionManager::update_market_data(const std::string& symbol, double volume, double close_price) {
    if (cost_manager_) {
        // Get previous close price (default to current if first observation)
        double prev_close = close_price;
        auto it = prev_close_prices_.find(symbol);
        if (it != prev_close_prices_.end()) {
            prev_close = it->second;
        }

        // Update the cost manager with all required data
        cost_manager_->update_market_data(symbol, volume, close_price, prev_close);

        // Store current close as previous for next update
        prev_close_prices_[symbol] = close_price;

        DEBUG("Updated market data for " + symbol + ": volume=" + std::to_string(volume) +
              ", close=" + std::to_string(close_price) + ", prev_close=" + std::to_string(prev_close));
    }
}

double ExecutionManager::calculate_transaction_cost(double quantity, double price) const {
    // Legacy method - kept for backward compatibility
    // Base commission: commission_rate * quantity
    double commission_cost = std::abs(quantity) * commission_rate_;

    // Market impact: basis points * quantity * price
    double market_impact = std::abs(quantity) * price * (market_impact_bps_ / 10000.0);

    // Fixed cost per trade
    double fixed_cost = fixed_cost_per_trade_;

    return commission_cost + market_impact + fixed_cost;
}

double ExecutionManager::apply_slippage(double market_price, Side side) const {
    // Convert basis points to decimal (1 bp = 0.0001)
    double slip_factor = slippage_bps_ / 10000.0;

    // Apply slippage: buy at higher price, sell at lower price
    if (side == Side::BUY) {
        return market_price * (1.0 + slip_factor);
    } else {
        return market_price * (1.0 - slip_factor);
    }
}

std::string ExecutionManager::generate_date_string(const Timestamp& timestamp) {
    // Convert timestamp to time_t
    std::time_t time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm* tm = std::localtime(&time);

    // Create date string in YYYYMMDD format
    std::stringstream date_ss;
    date_ss << std::setfill('0')
            << std::setw(4) << (tm->tm_year + 1900)
            << std::setw(2) << (tm->tm_mon + 1)
            << std::setw(2) << tm->tm_mday;
    return date_ss.str();
}

std::string ExecutionManager::generate_exec_id(
    const std::string& symbol,
    const Timestamp& timestamp,
    size_t sequence) {

    // Get timestamp in milliseconds for uniqueness
    auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch()).count();

    // Create unique execution ID
    return "EXEC_" + symbol + "_" + std::to_string(timestamp_ms) + "_" + std::to_string(sequence);
}

} // namespace trade_ngin
