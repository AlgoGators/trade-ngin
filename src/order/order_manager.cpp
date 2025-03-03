//===== order_manager.cpp =====
#include "trade_ngin/order/order_manager.hpp"
#include <atomic>

namespace trade_ngin {

OrderManager::~OrderManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!pending_orders_.empty()) {
        auto order_id = pending_orders_.front();
        // Clear any resources
        pending_orders_ = std::queue<std::string>();
        order_book_.clear();
    }
}

Result<void> OrderManager::initialize() {
    if (instance_id_.empty()) {
        instance_id_ = generate_instance_id();
    }

    ComponentInfo info{
        ComponentType::ORDER_MANAGER,
        ComponentState::INITIALIZED,
        instance_id_,
        "",
        std::chrono::system_clock::now(),
        {}
    };

    auto register_result = StateManager::instance().register_component(info);
    if (register_result.is_error()) {
        return register_result;
    }

    auto state_result = StateManager::instance().update_state(
        instance_id_,
        ComponentState::RUNNING
    );

    return state_result;
}

Result<std::string> OrderManager::submit_order(
    const Order& order,
    const std::string& strategy_id) {
    
    std::lock_guard<std::mutex> lock(mutex_);

    std::string error_msg;
    if (!validate_order(order, error_msg)) {
        return make_error<std::string>(
            ErrorCode::INVALID_ORDER,
            error_msg,
            "OrderManager"
        );
    }

    if (pending_orders_.size() >= config_.max_pending_orders) {
        return make_error<std::string>(
            ErrorCode::ORDER_REJECTED,
            "Maximum pending orders reached",
            "OrderManager"
        );
    }

    std::string order_id = generate_order_id();
    OrderBookEntry entry{
        order_id,
        order,
        OrderStatus::NEW,
        0.0,
        0.0,
        "",
        "",
        std::chrono::system_clock::now(),
        strategy_id
    };

    order_book_[order_id] = entry;
    pending_orders_.push(order_id);

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

    return Result<std::string>(order_id);
}

Result<void> OrderManager::cancel_order(const std::string& order_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = order_book_.find(order_id);
    if (it == order_book_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ORDER,
            "Order not found",
            "OrderManager"
        );
    }

    if (it->second.status == OrderStatus::FILLED ||
        it->second.status == OrderStatus::CANCELLED) {
        return make_error<void>(
            ErrorCode::INVALID_ORDER,
            "Order already " + 
            std::string((it->second.status == OrderStatus::FILLED ? "filled" : "cancelled")),
            "OrderManager"
        );
    }

    it->second.status = OrderStatus::CANCELLED;
    it->second.last_update = std::chrono::system_clock::now();
    return Result<void>();
}

Result<OrderBookEntry> OrderManager::get_order_status(
    const std::string& order_id) const {
    
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = order_book_.find(order_id);
    if (it == order_book_.end()) {
        return make_error<OrderBookEntry>(
            ErrorCode::INVALID_ORDER,
            "Order not found",
            "OrderManager"
        );
    }

    return Result<OrderBookEntry>(it->second);
}

Result<std::vector<OrderBookEntry>> OrderManager::get_strategy_orders(
    const std::string& strategy_id) const {
    
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OrderBookEntry> orders;

    for (const auto& [_, entry] : order_book_) {
        if (entry.strategy_id == strategy_id) {
            orders.push_back(entry);
        }
    }

    return Result<std::vector<OrderBookEntry>>(orders);
}

Result<std::vector<OrderBookEntry>> OrderManager::get_active_orders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OrderBookEntry> active_orders;

    for (const auto& [_, entry] : order_book_) {
        if (entry.status != OrderStatus::FILLED &&
            entry.status != OrderStatus::CANCELLED &&
            entry.status != OrderStatus::REJECTED) {
            active_orders.push_back(entry);
        }
    }

    return Result<std::vector<OrderBookEntry>>(active_orders);
}

bool OrderManager::validate_order(const Order& order, std::string& error_msg) const {
    if (order.symbol.empty()) {
        error_msg = "Symbol cannot be empty";
        return false;
    }

    if (order.quantity <= 0) {
        error_msg = "Quantity must be positive";
        return false;
    }

    if (order.type == OrderType::LIMIT && order.price <= 0) {
        error_msg = "Limit price must be positive";
        return false;
    }

    if (order.quantity > config_.max_order_size) {
        error_msg = "Order size exceeds maximum";
        return false;
    }

    double notional = order.quantity * order.price;
    if (notional > config_.max_notional_value) {
        error_msg = "Notional value exceeds maximum";
        return false;
    }

    return true;
}

std::string OrderManager::generate_order_id() const {
    static std::atomic<uint64_t> counter{0};
    return std::to_string(++counter);
}

Result<void> OrderManager::process_execution(const ExecutionReport& report) {
    auto it = order_book_.find(report.order_id);
    if (it == order_book_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ORDER,
            "Order not found",
            "OrderManager"
        );
    }

    auto& entry = it->second;
    double old_qty = entry.filled_quantity;
    double new_qty = old_qty + report.filled_quantity;

    entry.average_fill_price = (entry.average_fill_price * old_qty + 
                              report.fill_price * report.filled_quantity) / new_qty;
    entry.filled_quantity = new_qty;
    entry.status = std::abs(new_qty >= (entry.order.quantity - 1e-6)) ? 
                  OrderStatus::FILLED : OrderStatus::PARTIALLY_FILLED;
    entry.last_update = report.fill_time;

    return Result<void>();
}

Result<void> OrderManager::send_to_broker(const std::string& order_id) {
    auto& entry = order_book_[order_id];
    entry.status = OrderStatus::ACCEPTED; // Always mark as ACCEPTED

    if (config_.simulate_fills) {
        // Simulate an immediate fill (original logic)
        ExecutionReport report{
            order_id,
            "SIM_" + order_id,
            entry.order.symbol,
            entry.order.side,
            entry.order.quantity,
            entry.order.price,
            std::chrono::system_clock::now(),
            0.0,
            false
        };
        return process_execution(report);
    }

    return Result<void>();
}

} // namespace trade_ngin