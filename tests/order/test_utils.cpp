//===== test_utils.cpp =====
#include "test_utils.hpp"

namespace trade_ngin {
namespace testing {

void MarketDataCapture::on_event(const MarketDataEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(event);
}

std::vector<MarketDataEvent> MarketDataCapture::get_events() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
}

void MarketDataCapture::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.clear();
}

size_t MarketDataCapture::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.size();
}

Order create_test_order() {
    Order order;
    order.symbol = "AAPL";
    order.side = Side::BUY;
    order.type = OrderType::LIMIT;
    order.quantity = 100;
    order.price = 150.0;
    order.time_in_force = TimeInForce::DAY;
    order.timestamp = std::chrono::system_clock::now();
    order.strategy_id = "TEST_STRATEGY";
    return order;
}

ExecutionReport create_test_execution(
    const std::string& order_id,
    double filled_qty,
    bool is_partial) {
    
    ExecutionReport exec;
    exec.order_id = order_id;
    exec.exec_id = "EXEC_" + order_id;
    exec.symbol = "AAPL";
    exec.side = Side::BUY;
    exec.filled_quantity = filled_qty;
    exec.fill_price = 150.0;
    exec.fill_time = std::chrono::system_clock::now();
    exec.commission = 1.0;
    exec.is_partial = is_partial;
    return exec;
}

OrderManagerConfig create_test_config() {
    OrderManagerConfig config;
    config.max_orders_per_second = 100;
    config.max_pending_orders = 1000;
    config.max_order_size = 100000.0;
    config.max_notional_value = 1000000.0;
    config.simulate_fills = true;
    config.retry_attempts = 3;
    config.retry_delay_ms = 100.0;
    config.component_type = ComponentType::ORDER_MANAGER;
    return config;
}

} // namespace testing
} // namespace trade_ngin