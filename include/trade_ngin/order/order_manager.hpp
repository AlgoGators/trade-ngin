// include/trade_ngin/order/order_manager.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include <memory>
#include <unordered_map>
#include <queue>
#include <mutex>

namespace trade_ngin {

/**
 * @brief Order status enumeration
 */
enum class OrderStatus {
    NEW,        // Order created but not sent
    PENDING,    // Order sent to broker
    ACCEPTED,   // Order accepted by broker/exchange
    REJECTED,   // Order rejected by broker/exchange
    CANCELLED,  // Order cancelled
    FILLED,     // Order completely filled
    PARTIALLY_FILLED, // Order partially filled
    EXPIRED     // Order expired (e.g., day orders)
};

/**
 * @brief Order validation status
 */
struct OrderValidation {
    bool is_valid{false};
    std::string error_message;
};

/**
 * @brief Order book entry
 */
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

/**
 * @brief Configuration for the order manager
 */
struct OrderManagerConfig {
    size_t max_orders_per_second{100};      // Rate limit for orders
    size_t max_pending_orders{1000};        // Maximum number of pending orders
    double max_order_size{100000.0};        // Maximum single order size
    double max_notional_value{1000000.0};   // Maximum notional value per order
    std::string broker_config_path;         // Path to broker configuration
    bool simulate_fills{false};             // Whether to simulate fills (for testing)
    int retry_attempts{3};                  // Number of retry attempts for failed orders
    double retry_delay_ms{100.0};          // Delay between retries
};

/**
 * @brief Manager for order lifecycle and execution
 */
class OrderManager {
public:
    explicit OrderManager(OrderManagerConfig config);
    ~OrderManager();

    // Delete copy and move
    OrderManager(const OrderManager&) = delete;
    OrderManager& operator=(const OrderManager&) = delete;
    OrderManager(OrderManager&&) = delete;
    OrderManager& operator=(OrderManager&&) = delete;

    /**
     * @brief Initialize order manager
     * @return Result indicating success or failure
     */
    Result<void> initialize();

    /**
     * @brief Submit a new order
     * @param order Order to submit
     * @param strategy_id ID of strategy submitting order
     * @return Result containing order ID if successful
     */
    Result<std::string> submit_order(const Order& order, const std::string& strategy_id);

    /**
     * @brief Cancel an existing order
     * @param order_id ID of order to cancel
     * @return Result indicating success or failure
     */
    Result<void> cancel_order(const std::string& order_id);

    /**
     * @brief Get order status
     * @param order_id Order ID to query
     * @return Result containing order book entry
     */
    Result<OrderBookEntry> get_order_status(const std::string& order_id) const;

    /**
     * @brief Get all orders for a strategy
     * @param strategy_id Strategy ID to query
     * @return Result containing vector of order book entries
     */
    Result<std::vector<OrderBookEntry>> get_strategy_orders(
        const std::string& strategy_id) const;

    /**
     * @brief Get all active orders
     * @return Result containing vector of order book entries
     */
    Result<std::vector<OrderBookEntry>> get_active_orders() const;

    /**
     * @brief Get order status as string
     * @param status Order status enumeration
     * @return String representation of status
     */
    std::string order_status_to_string(OrderStatus status) const {
        switch (status) {
            case OrderStatus::NEW: return "new";
            case OrderStatus::PENDING: return "pending";
            case OrderStatus::ACCEPTED: return "accepted";
            case OrderStatus::PARTIALLY_FILLED: return "partially filled";
            case OrderStatus::FILLED: return "filled";
            case OrderStatus::CANCELLED: return "cancelled";
            case OrderStatus::REJECTED: return "rejected";
            case OrderStatus::EXPIRED: return "expired";
            default: return "unknown";
        }
    }


private:
    OrderManagerConfig config_;
    std::unordered_map<std::string, OrderBookEntry> order_book_;
    std::queue<std::string> pending_orders_;
    mutable std::mutex mutex_;

    /**
     * @brief Validate an order before submission
     * @param order Order to validate
     * @return Validation result
     */
    OrderValidation validate_order(const Order& order) const;

    /**
     * @brief Process execution report
     * @param report Execution report from broker
     * @return Result indicating success or failure
     */
    Result<void> process_execution(const ExecutionReport& report);

    /**
     * @brief Generate unique order ID
     * @return Unique order ID
     */
    std::string generate_order_id() const;

    /**
     * @brief Send order to broker
     * @param order_id Order ID to send
     * @return Result indicating success or failure
     */
    Result<void> send_to_broker(const std::string& order_id);

    /**
     * @brief Handle broker rejection
     * @param order_id Rejected order ID
     * @param reason Rejection reason
     * @return Result indicating success or failure
     */
    Result<void> handle_rejection(
        const std::string& order_id,
        const std::string& reason);

    /**
     * @brief Update order status
     * @param order_id Order ID to update
     * @param new_status New order status
     * @param message Optional status message
     * @return Result indicating success or failure
     */
    Result<void> update_order_status(
        const std::string& order_id,
        OrderStatus new_status,
        const std::string& message = "");
};

} // namespace trade_ngin