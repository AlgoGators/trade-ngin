#include "trade_ngin/live/execution_manager.hpp"
#include "trade_ngin/apps/book_execution_phase.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>
#include <algorithm>
#include "trade_ngin/core/time_utils.hpp"
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace trade_ngin {

Result<std::vector<ExecutionReport>> ExecutionManager::generate_daily_executions(
    const std::unordered_map<std::string, Position>& current_positions,
    const std::unordered_map<std::string, Position>& previous_positions,
    const std::unordered_map<std::string, double>& market_prices,
    const Timestamp& timestamp, const std::string& portfolio_type,
    DailyExecutionObservation* observation) {
    if (observation) *observation = {};

    if (portfolio_type != "system" && portfolio_type != "qt") {
        if (observation) {
            observation->state = DailyExecutionState::rejected_stream;
            observation->error_code = ErrorCode::INVALID_ARGUMENT;
        }
        return make_error<std::vector<ExecutionReport>>(ErrorCode::INVALID_ARGUMENT,"unsupported_execution_stream");
    }
    try {

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
            DailyExecutionAttempt* attempt = nullptr;
            if (observation) {
                auto& entry = observation->attempts.emplace_back();
                entry.symbol = symbol;
                entry.sequence = daily_executions.size();
                entry.branch = DailyPositionBranch::current_position;
                entry.price_source = price_it != market_prices.end()
                    ? ExecutionPriceSource::market_prices
                    : ExecutionPriceSource::current_average_price;
                entry.selected_price = market_price;
                attempt = &entry;
            }
            ExecutionReport exec = generate_execution(
                symbol, trade_size, market_price, timestamp, daily_executions.size(),portfolio_type,
                attempt ? &attempt->execution : nullptr);
            if (attempt) attempt->returned = true;
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
            DailyExecutionAttempt* attempt = nullptr;
            if (observation) {
                auto& entry = observation->attempts.emplace_back();
                entry.symbol = symbol;
                entry.sequence = daily_executions.size();
                entry.branch = DailyPositionBranch::removed_position;
                entry.price_source = price_it != market_prices.end()
                    ? ExecutionPriceSource::market_prices
                    : ExecutionPriceSource::previous_average_price;
                entry.selected_price = market_price;
                attempt = &entry;
            }
            ExecutionReport exec = generate_execution(
                symbol, trade_size, market_price, timestamp, daily_executions.size(),portfolio_type,
                attempt ? &attempt->execution : nullptr);
            if (attempt) attempt->returned = true;
            daily_executions.push_back(exec);

            INFO("Generated execution for closed position: " + symbol + " " +
                 (exec.side == Side::BUY ? "BUY" : "SELL") + " " +
                 std::to_string(exec.filled_quantity) + " at " +
                 std::to_string(exec.fill_price));
        }
    }

    INFO("Generated " + std::to_string(daily_executions.size()) + " execution reports");
    Result<std::vector<ExecutionReport>> result(daily_executions);
    if (observation) observation->state = DailyExecutionState::returned;
    return result;
    } catch (const std::invalid_argument& e) {
        if (observation) {
            observation->state = DailyExecutionState::invalid_argument;
            observation->error_code = ErrorCode::INVALID_ARGUMENT;
        }
        return make_error<std::vector<ExecutionReport>>(ErrorCode::INVALID_ARGUMENT,e.what());
    }
}

ExecutionReport ExecutionManager::generate_execution(
    const std::string& symbol,
    double quantity_change,
    double market_price,
    const Timestamp& timestamp,
    size_t exec_sequence, const std::string& portfolio_type,
    ExecutionCallObservation* observation) {
    if (observation) *observation = {};

    if (portfolio_type != "system" && portfolio_type != "qt") {
        if (observation) observation->state = ExecutionCallState::rejected_stream;
        throw std::invalid_argument("unsupported_execution_stream");
    }

    ExecutionReport exec;

    // Determine side
    Side side = quantity_change > 0 ? Side::BUY : Side::SELL;

    // Generate IDs
    std::string date_str = generate_date_string(timestamp);
    exec.order_id = (portfolio_type == "qt" ? "QT_DAILY_" : "DAILY_") + symbol + "_" + date_str;
    exec.exec_id = generate_exec_id(symbol, timestamp, exec_sequence);
    if (portfolio_type == "qt" && (exec.order_id.size()>50 || exec.exec_id.size()>50)) {
        if (observation) observation->state = ExecutionCallState::rejected_id;
        throw std::invalid_argument("execution_id_exceeds_schema_bound");
    }

    // Set basic fields
    exec.symbol = symbol;
    exec.side = side;
    exec.filled_quantity = std::abs(quantity_change);
    exec.fill_time = timestamp;

    double abs_quantity = exec.filled_quantity.as_double();

    // TransactionCostManager is the single source of truth.
    // Keep fill_price as pure reference price (no embedded slippage).
    exec.fill_price = market_price;

    if (observation) observation->state = ExecutionCallState::cost_call_reached;
    auto charge = charge_model_book_execution(*cost_manager_,
        ModelTrackedExecutionCharge{symbol, abs_quantity, market_price, AssetType::FUTURE},
        observation ? &observation->cost : nullptr);
    const auto& cost_result = charge.raw;

    exec.commissions_fees = charge.commissions_fees;
    exec.implicit_price_impact = charge.implicit_price_impact;
    exec.slippage_market_impact = charge.slippage_market_impact;
    exec.total_transaction_costs = charge.total_transaction_costs;

    DEBUG("Transaction cost model for " + symbol + ": commissions=$" +
          std::to_string(cost_result.commissions_fees) +
          ", implicit_impact=" + std::to_string(cost_result.implicit_price_impact) +
          ", slippage=$" + std::to_string(cost_result.slippage_market_impact) +
          ", total=$" + std::to_string(cost_result.total_transaction_costs));

    exec.is_partial = false;

    if (observation) observation->state = ExecutionCallState::returned;
    return exec;
}

void ExecutionManager::update_market_data(const std::string& symbol, double volume, double close_price,
                                          ExecutionMarketDataObservation* observation) {
    if (observation) *observation = {};
    if (cost_manager_) {
        if (observation) observation->cost_model_reached = true;
        // Get previous close price (default to current if first observation)
        double prev_close = close_price;
        auto it = prev_close_prices_.find(symbol);
        if (it != prev_close_prices_.end()) {
            prev_close = it->second;
        }
        if (observation) {
            observation->previous_close_source = it != prev_close_prices_.end()
                ? PreviousCloseSource::stored_previous_close
                : PreviousCloseSource::initial_current_close;
            observation->previous_close_forwarded = prev_close;
        }

        // Update the cost manager with all required data
        cost_manager_->update_market_data(symbol, volume, close_price, prev_close,
                                          observation ? &observation->market_data : nullptr);

        // Store current close as previous for next update
        prev_close_prices_[symbol] = close_price;

        DEBUG("Updated market data for " + symbol + ": volume=" + std::to_string(volume) +
              ", close=" + std::to_string(close_price) + ", prev_close=" + std::to_string(prev_close));
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

Result<std::vector<ExecutionReport>> ExecutionManager::generate_daily_executions(
    const std::unordered_map<std::string, Position>& current_positions,
    const std::unordered_map<std::string, Position>& previous_positions,
    const std::unordered_map<std::string, double>& market_prices,
    const Timestamp& timestamp,
    PricingPolicy pricing,
    std::vector<std::string>* unpriced_out,
    DailyExecutionObservation* observation) {
    if(observation)*observation={};

    INFO("Generating execution reports for position changes...");
    std::vector<ExecutionReport> daily_executions;
    std::vector<std::string> unpriced_symbols;

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

            // Market price (Day T-1 close for Day T execution). What happens when the
            // map has no usable entry depends on what average_price means to THIS
            // caller -- see PricingPolicy. Futures assigns it the latest mark, so the
            // fallback yields a real price; equity mean reversion maintains it as a
            // cost basis, which is 0.00 for a position opened today.
            auto price_it = market_prices.find(symbol);
            double market_price = 0.0;
            if (price_it != market_prices.end() && price_it->second > 0.0) {
                market_price = price_it->second;
            } else if (pricing == PricingPolicy::MARK_FALLBACK) {
                market_price = current_position.average_price.as_double();
                WARN("No market price for " + symbol + ", using average price");
            } else {
                ERROR("No usable market price for " + symbol +
                      " - skipping execution for a position change of " +
                      std::to_string(trade_size) +
                      ". This symbol will NOT be traded today.");
                unpriced_symbols.push_back(symbol);
                continue;
            }

            // Generate execution
            DailyExecutionAttempt* attempt=nullptr;
            if(observation){observation->attempts.emplace_back();attempt=&observation->attempts.back();
                attempt->symbol=symbol;attempt->sequence=daily_executions.size();
                attempt->branch=DailyPositionBranch::current_position;
                attempt->price_source=price_it!=market_prices.end() && price_it->second>0.0?
                    ExecutionPriceSource::market_prices:ExecutionPriceSource::current_average_price;
                attempt->selected_price=market_price;}
            ExecutionReport exec = generate_equity_execution(
                symbol, trade_size, market_price, timestamp, daily_executions.size(),attempt?&attempt->execution:nullptr);
            if(attempt)attempt->returned=true;
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

            // Same rule as the position-change path above; see PricingPolicy. Under
            // STRICT, a close-out priced at the position's own cost basis would book
            // the exit at what it cost rather than what it is worth, silently
            // reporting zero realized PnL.
            auto price_it = market_prices.find(symbol);
            double market_price = 0.0;
            if (price_it != market_prices.end() && price_it->second > 0.0) {
                market_price = price_it->second;
            } else if (pricing == PricingPolicy::MARK_FALLBACK) {
                market_price = prev_position.average_price.as_double();
                WARN("No market price for closed position " + symbol + ", using average price");
            } else {
                ERROR("No usable market price to close " + symbol + " (quantity " +
                      std::to_string(prev_qty) +
                      ") - skipping execution. The caller must reconcile this symbol.");
                unpriced_symbols.push_back(symbol);
                continue;
            }

            // Generate execution for closing (opposite side of position)
            double trade_size = -prev_qty; // Negative because we're closing
            DailyExecutionAttempt* attempt=nullptr;
            if(observation){observation->attempts.emplace_back();attempt=&observation->attempts.back();
                attempt->symbol=symbol;attempt->sequence=daily_executions.size();
                attempt->branch=DailyPositionBranch::removed_position;
                attempt->price_source=price_it!=market_prices.end() && price_it->second>0.0?
                    ExecutionPriceSource::market_prices:ExecutionPriceSource::previous_average_price;
                attempt->selected_price=market_price;}
            ExecutionReport exec = generate_equity_execution(
                symbol, trade_size, market_price, timestamp, daily_executions.size(),attempt?&attempt->execution:nullptr);
            if(attempt)attempt->returned=true;
            daily_executions.push_back(exec);

            INFO("Generated execution for closed position: " + symbol + " " +
                 (exec.side == Side::BUY ? "BUY" : "SELL") + " " +
                 std::to_string(exec.filled_quantity) + " at " +
                 std::to_string(exec.fill_price));
        }
    }

    INFO("Generated " + std::to_string(daily_executions.size()) + " execution reports");

    if (!unpriced_symbols.empty()) {
        std::sort(unpriced_symbols.begin(), unpriced_symbols.end());
        unpriced_symbols.erase(std::unique(unpriced_symbols.begin(), unpriced_symbols.end()),
                               unpriced_symbols.end());
        std::string joined;
        for (const auto& s : unpriced_symbols) {
            if (!joined.empty()) joined += ", ";
            joined += s;
        }
        ERROR("Skipped " + std::to_string(unpriced_symbols.size()) +
              " symbol(s) with no usable market price: " + joined +
              ". Their intended position changes did NOT execute.");
    }
    if (unpriced_out) *unpriced_out = unpriced_symbols;

    if(observation)observation->state=DailyExecutionState::returned;
    return Result<std::vector<ExecutionReport>>(daily_executions);
}

ExecutionReport ExecutionManager::generate_equity_execution(
    const std::string& symbol,
    double quantity_change,
    double market_price,
    const Timestamp& timestamp,
    size_t exec_sequence,
    ExecutionCallObservation* observation) {
    if(observation)*observation={};

    ExecutionReport exec;

    // Determine side
    Side side = quantity_change > 0 ? Side::BUY : Side::SELL;

    // Generate IDs
    std::string date_str = core::format_utc_date(timestamp);
    date_str.erase(std::remove(date_str.begin(), date_str.end(), '-'), date_str.end());
    exec.order_id = "DAILY_" + symbol + "_" + date_str;
    exec.exec_id = generate_exec_id(symbol, timestamp, exec_sequence);

    // Set basic fields
    exec.symbol = symbol;
    exec.side = side;
    exec.filled_quantity = std::abs(quantity_change);
    exec.fill_time = timestamp;

    // TransactionCostManager is the single source of truth.
    // Keep fill_price as pure reference price (no embedded slippage).
    exec.fill_price = market_price;

    // E2-F29: pass the SIGNED quantity. TransactionCostManager takes |qty| for every cost
    // term except the SEC/TAF regulatory fees, which are sell-side only and are gated on
    // `quantity < 0`. Passing |quantity_change| made that gate unreachable, so a config with
    // apply_regulatory_fees charged a sell exactly what it charged a buy.
    if(observation)observation->state=ExecutionCallState::cost_call_reached;
    auto charge = charge_model_book_execution(*cost_manager_,
        ModelTrackedExecutionCharge{symbol, quantity_change, market_price, AssetType::EQUITY},observation?&observation->cost:nullptr);
    const auto& cost_result = charge.raw;

    exec.commissions_fees = charge.commissions_fees;
    exec.implicit_price_impact = charge.implicit_price_impact;
    exec.slippage_market_impact = charge.slippage_market_impact;
    exec.total_transaction_costs = charge.total_transaction_costs;

    DEBUG("Transaction cost model for " + symbol + ": commissions=$" +
          std::to_string(cost_result.commissions_fees) +
          ", implicit_impact=" + std::to_string(cost_result.implicit_price_impact) +
          ", slippage=$" + std::to_string(cost_result.slippage_market_impact) +
          ", total=$" + std::to_string(cost_result.total_transaction_costs));

    exec.is_partial = false;

    if(observation)observation->state=ExecutionCallState::returned;
    return exec;
}

} // namespace trade_ngin
