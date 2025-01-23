// src/order/order_manager.cpp

#include "trade_ngin/order/order_manager.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include <chrono>
#include <sstream>
#include <iomanip>

namespace trade_ngin {

OrderManager::OrderManager(OrderManagerConfig config)
    : config_(std::move(config)) {
    
    // Register with state manager
    ComponentInfo info{
        ComponentType::ORDER_MANAGER,
        ComponentState::INITIALIZED,
        "ORDER_MANAGER",
        "",
        std::chrono::system_clock::now(),
        {
            {"max_orders_per_second", static_cast<double>(config_.max_orders_per_second)},
            {"max_pending_orders", static_cast<double>(config_.max_pending_orders)}
        }
    };

    auto register_result = StateManager::instance().register_component(info);
    if (register_result.is_error()) {
        throw std::runtime_error(register_result.error()->what());
    }
}

OrderManager::~OrderManager() {
    // Cancel all pending orders on shutdown
    std::lock_guard<std::mutex> lock(mutex_);
    while (!pending_orders_.empty()) {
        auto order_id = pending_orders_.front();
        pending_orders_.pop();
        
        auto it = order_book_.find(order_id);
        if (it != order_book_.end() && 
            it->second.status != OrderStatus::FILLED &&
            it->second.status != OrderStatus::CANCELLED) {
            
            cancel_order(order_id);
        }
    }
}

Result<void> OrderManager::initialize() {
    try {
        // Setup market data subscription for execution reports
        MarketDataCallback callback = [this](const MarketDataEvent& event) {
            if (event.type == MarketDataEventType::ORDER_UPDATE) {
                ExecutionReport report;
                report.order_id = event.string_fields.at("order_id");
                report.exec_id = event.string_fields.at("exec_id");
                report.symbol = event.symbol;
                report.side = event.string_fields.at("side") == "BUY" ? Side::BUY : Side::SELL;
                report.filled_quantity = event.numeric_fields.at("filled_quantity");
                report.fill_price = event.numeric_fields.at("fill_price");
                report.fill_time = event.timestamp;
                report.commission = event.numeric_fields.at("commission");
                report.is_partial = event.numeric_fields.at("is_partial") > 0;

                auto result = process_execution(report);
                if (result.is_error()) {
                    ERROR("Error processing execution report: " + 
                          std::string(result.error()->what()));
                }
            }
        };

        SubscriberInfo sub_info{
            "ORDER_MANAGER",
            {MarketDataEventType::ORDER_UPDATE},
            {},  // Subscribe to all symbols
            callback
        };

        auto subscribe_result = MarketDataBus::instance().subscribe(sub_info);
        if (subscribe_result.is_error()) {
            return subscribe_result;
        }

        // Update state
        auto state_result = StateManager::instance().update_state(
            "ORDER_MANAGER",
            ComponentState::RUNNING
        );

        if (state_result.is_error()) {
            return state_result;
        }

        INFO("Order Manager initialized successfully");
        return Result<void>({});

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::NOT_INITIALIZED,
            std::string("Failed to initialize order manager: ") + e.what(),
            "OrderManager"
        );
    }
}

Result<std::string> OrderManager::submit_order(
    const Order& order,
    const std::string& strategy_id) {
    
    try {
        std::lock_guard<std::mutex> lock(mutex_);

        // Validate order
        auto validation = validate_order(order);
        if (!validation.is_valid) {
            return make_error<std::string>(
                ErrorCode::INVALID_ORDER,
                "Order validation failed: " + validation.error_message,
                "OrderManager"
            );
        }

        // Check pending order limit
        if (pending_orders_.size() >= config_.max_pending_orders) {
            return make_error<std::string>(
                ErrorCode::ORDER_REJECTED,
                "Maximum pending orders reached",
                "OrderManager"
            );
        }

        // Generate order ID and create book entry
        std::string order_id = generate_order_id();
        OrderBookEntry entry{
            order_id,
            order,
            OrderStatus::NEW,
            0.0,  // filled_quantity
            0.0,  // average_fill_price
            "",   // broker_order_id
            "",   // error_message
            std::chrono::system_clock::now(),
            strategy_id
        };

        // Add to order book and pending queue
        order_book_[order_id] = entry;
        pending_orders_.push(order_id);

        // Send to broker
        auto send_result = send_to_broker(order_id);
        if (send_result.is_error()) {
            order_book_[order_id].status = OrderStatus::REJECTED;
            order_book_[order_id].error_message = send_result.error()->what();
            pending_orders_.pop();

            return make_error<std::string>(
                send_result.error()->code(),
                send_result.error()->what(),
                "OrderManager"
            );
        }

        // Update status
        order_book_[order_id].status = OrderStatus::PENDING;
        INFO("Order submitted: " + order_id + " for " + strategy_id);

        return Result<std::string>(order_id);

    } catch (const std::exception& e) {
        return make_error<std::string>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error submitting order: ") + e.what(),
            "OrderManager"
        );
    }
}

Result<void> OrderManager::cancel_order(const std::string& order_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = order_book_.find(order_id);
    if (it == order_book_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ORDER,
            "Order not found: " + order_id,
            "OrderManager"
        );
    }

    if (it->second.status == OrderStatus::FILLED ||
        it->second.status == OrderStatus::CANCELLED) {
        return make_error<void>(
            ErrorCode::INVALID_ORDER,
            "Order already " + 
            order_status_to_string(it->second.status),
            "OrderManager"
        );
    }

    // Attempt to cancel with broker
    // TODO: Implement broker cancellation
    
    it->second.status = OrderStatus::CANCELLED;
    it->second.last_update = std::chrono::system_clock::now();

    INFO("Order cancelled: " + order_id);
    return Result<void>({});
}

Result<OrderBookEntry> OrderManager::get_order_status(
    const std::string& order_id) const {
    
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = order_book_.find(order_id);
    if (it == order_book_.end()) {
        return make_error<OrderBookEntry>(
            ErrorCode::INVALID_ORDER,
            "Order not found: " + order_id,
            "OrderManager"
        );
    }

    return Result<OrderBookEntry>(it->second);
}

Result<std::vector<OrderBookEntry>> OrderManager::get_strategy_orders(
    const std::string& strategy_id) const {
    
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OrderBookEntry> strategy_orders;

    for (const auto& [order_id, entry] : order_book_) {
        if (entry.strategy_id == strategy_id) {
            strategy_orders.push_back(entry);
        }
    }

    return Result<std::vector<OrderBookEntry>>(strategy_orders);
}

Result<std::vector<OrderBookEntry>> OrderManager::get_active_orders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OrderBookEntry> active_orders;

    for (const auto& [order_id, entry] : order_book_) {
        if (entry.status != OrderStatus::FILLED &&
            entry.status != OrderStatus::CANCELLED &&
            entry.status != OrderStatus::REJECTED) {
            active_orders.push_back(entry);
        }
    }

    return Result<std::vector<OrderBookEntry>>(active_orders);
}

OrderValidation OrderManager::validate_order(const Order& order) const {
    OrderValidation validation;

    // Basic validations
    if (order.symbol.empty()) {
        validation.error_message = "Symbol cannot be empty";
        return validation;
    }

    if (order.quantity <= 0) {
        validation.error_message = "Quantity must be positive";
        return validation;
    }

    if (order.type == OrderType::LIMIT && order.price <= 0) {
        validation.error_message = "Limit price must be positive";
        return validation;
    }

    // Size checks
    if (order.quantity > config_.max_order_size) {
        validation.error_message = "Order size exceeds maximum";
        return validation;
    }

    double notional = order.price * order.quantity;
    if (notional > config_.max_notional_value) {
        validation.error_message = "Notional value exceeds maximum";
        return validation;
    }

    // Time in force validation
    if (order.time_in_force == TimeInForce::GTD && 
        !order.good_till_date.has_value()) {
        validation.error_message = "GTD orders require good till date";
        return validation;
    }

    validation.is_valid = true;
    return validation;
}

Result<void> OrderManager::process_execution(const ExecutionReport& report) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = order_book_.find(report.order_id);
    if (it == order_book_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ORDER,
            "Order not found for execution: " + report.order_id,
            "OrderManager"
        );
    }

    auto& entry = it->second;

    // Update filled quantity and average price
    double old_qty = entry.filled_quantity;
    double new_qty = old_qty + report.filled_quantity;
    entry.average_fill_price = (entry.average_fill_price * old_qty + 
                              report.fill_price * report.filled_quantity) / new_qty;
    entry.filled_quantity = new_qty;

    // Update status
    if (std::abs(entry.filled_quantity - entry.order.quantity) < 1e-6) {
        entry.status = OrderStatus::FILLED;
    } else {
        entry.status = OrderStatus::PARTIALLY_FILLED;
    }

    entry.last_update = report.fill_time;

    // Publish fill event
    MarketDataEvent event;
    event.type = MarketDataEventType::ORDER_UPDATE;
    event.symbol = report.symbol;
    event.timestamp = report.fill_time;
    event.string_fields = {
        {"order_id", report.order_id},
        {"exec_id", report.exec_id},
        {"side", report.side == Side::BUY ? "BUY" : "SELL"}
    };
    event.numeric_fields = {
        {"filled_quantity", report.filled_quantity},
        {"fill_price", report.fill_price},
        {"commission", report.commission},
        {"is_partial", report.is_partial ? 1.0 : 0.0}
    };

    MarketDataBus::instance().publish(event);

    INFO("Processed execution for order " + report.order_id + 
         ": " + std::to_string(report.filled_quantity) + " @ " + 
         std::to_string(report.fill_price));

    return Result<void>({});
}

std::string OrderManager::generate_order_id() const {
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    
    std::stringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0') 
       << std::setw(16) << now_ms;
    return ss.str();
}

Result<void> OrderManager::send_to_broker(const std::string& order_id) {
    // TODO: Implement actual broker integration
    // For now, simulate acceptance
    if (config_.simulate_fills) {
        auto& entry = order_book_[order_id];
        entry.status = OrderStatus::ACCEPTED;
        entry.broker_order_id = "SIM_" + order_id;
        
        // Simulate an immediate fill
        ExecutionReport report;
        report.order_id = order_id;
        report.exec_id = "SIM_EXEC_" + order_id;
        report.symbol = entry.order.symbol;
        report.side = entry.order.side;
        report.filled_quantity = entry.order.quantity;
        report.fill_price = entry.order.price;
        report.fill_time = std::chrono::system_clock::now();
        report.commission = 0.0;
        report.is_partial = false;

        return process_execution(report);
    }

    return Result<void>({});
}

Result<void> OrderManager::handle_rejection(
    const std::string& order_id,
    const std::string& reason) {
    
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = order_book_.find(order_id);
    if (it == order_book_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ORDER,
            "Order not found: " + order_id,
            "OrderManager"
        );
    }

    it->second.status = OrderStatus::REJECTED;
    it->second.error_message = reason;
    it->second.last_update = std::chrono::system_clock::now();

    // Remove from pending queue if present
    std::queue<std::string> temp;
    while (!pending_orders_.empty()) {
        auto pending_id = pending_orders_.front();
        pending_orders_.pop();
        if (pending_id != order_id) {
            temp.push(pending_id);
        }
    }
    pending_orders_ = std::move(temp);

    ERROR("Order rejected: " + order_id + " - " + reason);
    return Result<void>({});
}

Result<void> OrderManager::update_order_status(
    const std::string& order_id,
    OrderStatus new_status,
    const std::string& message) {
    
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = order_book_.find(order_id);
    if (it == order_book_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ORDER,
            "Order not found: " + order_id,
            "OrderManager"
        );
    }

    // Validate state transition
    auto old_status = it->second.status;
    bool valid_transition = false;

    switch (old_status) {
        case OrderStatus::NEW:
            valid_transition = (new_status == OrderStatus::PENDING ||
                              new_status == OrderStatus::REJECTED);
            break;

        case OrderStatus::PENDING:
            valid_transition = (new_status == OrderStatus::ACCEPTED ||
                              new_status == OrderStatus::REJECTED ||
                              new_status == OrderStatus::CANCELLED);
            break;

        case OrderStatus::ACCEPTED:
            valid_transition = (new_status == OrderStatus::PARTIALLY_FILLED ||
                              new_status == OrderStatus::FILLED ||
                              new_status == OrderStatus::CANCELLED ||
                              new_status == OrderStatus::EXPIRED);
            break;

        case OrderStatus::PARTIALLY_FILLED:
            valid_transition = (new_status == OrderStatus::FILLED ||
                              new_status == OrderStatus::CANCELLED);
            break;

        case OrderStatus::FILLED:
        case OrderStatus::CANCELLED:
        case OrderStatus::REJECTED:
        case OrderStatus::EXPIRED:
            valid_transition = false;  // Terminal states
            break;
    }

    if (!valid_transition) {
        return make_error<void>(
            ErrorCode::INVALID_ORDER,
            "Invalid state transition from " + 
            std::to_string(static_cast<int>(old_status)) + " to " +
            std::to_string(static_cast<int>(new_status)),
            "OrderManager"
        );
    }

    // Update status
    it->second.status = new_status;
    it->second.last_update = std::chrono::system_clock::now();
    
    if (!message.empty()) {
        it->second.error_message = message;
    }

    // Log status change
    INFO("Order " + order_id + " status changed from " + 
         std::to_string(static_cast<int>(old_status)) + " to " +
         std::to_string(static_cast<int>(new_status)) +
         (message.empty() ? "" : " - " + message));

    return Result<void>({});
}

} // namespace trade_ngin