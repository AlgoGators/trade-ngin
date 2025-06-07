//===== order_manager.hpp =====
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include <memory>
#include <unordered_map>
#include <queue>
#include <mutex>
#include <iostream>

namespace trade_ngin {

enum class OrderStatus {
    NEW,
    PENDING,
    ACCEPTED,
    REJECTED,
    CANCELLED,
    FILLED,
    PARTIALLY_FILLED,
    EXPIRED
};

struct OrderBookEntry {
    std::string order_id;
    Order order;
    OrderStatus status;
    double filled_quantity{0.0};
    double average_fill_price{0.0};
    std::string broker_order_id;
    std::string error_message;
    Timestamp last_update;
    std::string strategy_id;
};

struct OrderManagerConfig {
    size_t max_orders_per_second{100};
    size_t max_pending_orders{1000};
    double max_order_size{100000.0};
    double max_notional_value{1000000.0};
    std::string broker_config_path;
    bool simulate_fills{false};
    int retry_attempts{3};
    double retry_delay_ms{100.0};
    ComponentType component_type{ComponentType::ORDER_MANAGER};
};

class OrderManager {
public:
    explicit OrderManager(OrderManagerConfig config, std::string component_id = "ORDER_MANAGER")
        : config_(std::move(config))
        , component_id_(std::move(component_id)) {}
    ~OrderManager();

    OrderManager(const OrderManager&) = delete;
    OrderManager& operator=(const OrderManager&) = delete;

    Result<void> initialize();
    Result<std::string> submit_order(const Order& order, const std::string& strategy_id);
    Result<void> cancel_order(const std::string& order_id);
    Result<OrderBookEntry> get_order_status(const std::string& order_id) const;
    Result<std::vector<OrderBookEntry>> get_strategy_orders(const std::string& strategy_id) const;
    Result<std::vector<OrderBookEntry>> get_active_orders() const;
    OrderManagerConfig get_config() const { return config_; }
    Result<void> process_execution(const ExecutionReport& report);

private:
    bool validate_order(const Order& order, std::string& error_msg) const;
    std::string generate_order_id() const;
    Result<void> send_to_broker(const std::string& order_id);

    OrderManagerConfig config_;
    std::unordered_map<std::string, OrderBookEntry> order_book_;
    std::queue<std::string> pending_orders_;
    mutable std::mutex mutex_;
    std::string component_id_;

    std::string instance_id_;
    std::string generate_instance_id() const {
        static std::atomic<uint64_t> counter{0};
        return "ORDER_MANAGER_" + std::to_string(++counter);
    }
};
}  // namespace trade_ngin