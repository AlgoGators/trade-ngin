#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <chrono>
#include <memory>
#include <functional>
#include <unordered_map>
#include <mutex>
#include "api_handler.hpp"
#include "contract.hpp"

// Forward declarations
class Contract;
class ApiHandler;

// Mock configuration of mocking into IB
static const char* LOCALHOST = "127.0.0.1";
static const int PORT = 7497;
static const int CLIENT_ID = 0;

// Enhanced order priority system
enum class AdaptiveOrderPriority {
    URGENT,   // Immediate execution needed
    NORMAL,   // Standard execution
    SLOW,     // Cost-sensitive execution
    PASSIVE   // Liquidity providing
};

// Convert enum to string
inline std::string to_string(AdaptiveOrderPriority priority) {
    switch (priority) {
        case AdaptiveOrderPriority::URGENT: return "URGENT";
        case AdaptiveOrderPriority::NORMAL: return "NORMAL";
        case AdaptiveOrderPriority::SLOW: return "SLOW";
        case AdaptiveOrderPriority::PASSIVE: return "PASSIVE";
        default: return "UNKNOWN";
    }
}

// Order side
enum class OrderSide {
    BUY,
    SELL
};

// Order type
enum class OrderType {
    MARKET,
    LIMIT,
    STOP,
    STOP_LIMIT
};

// Order status
enum class OrderStatus {
    NEW,
    PENDING,
    FILLED,
    PARTIALLY_FILLED,
    CANCELLED,
    REJECTED
};

// Order class
class Order {
public:
    Order(const Contract& contract, OrderSide side, double quantity, 
          OrderType type = OrderType::MARKET, 
          AdaptiveOrderPriority priority = AdaptiveOrderPriority::NORMAL)
        : contract_(contract), side_(side), quantity_(quantity),
          type_(type), priority_(priority), status_(OrderStatus::NEW) {}

    // Getters
    const Contract& getContract() const { return contract_; }
    OrderSide getSide() const { return side_; }
    double getQuantity() const { return quantity_; }
    OrderType getType() const { return type_; }
    AdaptiveOrderPriority getPriority() const { return priority_; }
    OrderStatus getStatus() const { return status_; }

    // Setters
    void setStatus(OrderStatus status) { status_ = status; }
    void setFilledQuantity(double qty) { filled_quantity_ = qty; }

private:
    Contract contract_;
    OrderSide side_;
    double quantity_;
    OrderType type_;
    AdaptiveOrderPriority priority_;
    OrderStatus status_;
    double filled_quantity_ = 0.0;
    double limit_price_ = 0.0;
    double stop_price_ = 0.0;
};

// Order Manager class
class OrderManager {
public:
    OrderManager(const std::string& host = LOCALHOST, 
                int port = PORT, 
                int client_id = CLIENT_ID)
        : api_handler_(host, port, client_id) {}

    // Order submission
    void submitOrder(std::shared_ptr<Order> order) {
        std::lock_guard<std::mutex> lock(mutex_);
        order_queue_.push(order);
        processOrders();
    }

    // Cancel all orders
    void cancelAllOrders() {
        std::lock_guard<std::mutex> lock(mutex_);
        api_handler_.cancel_outstanding_orders();
        while (!order_queue_.empty()) {
            auto order = order_queue_.front();
            order->setStatus(OrderStatus::CANCELLED);
            order_queue_.pop();
        }
    }

    // Process queued orders
    void processOrders() {
        while (!order_queue_.empty()) {
            auto order = order_queue_.front();
            order_queue_.pop();

            try {
                executeOrder(order);
            } catch (const std::exception& e) {
                std::cerr << "Order execution failed: " << e.what() << "\n";
                order->setStatus(OrderStatus::REJECTED);
            }
        }
    }

private:
    void executeOrder(std::shared_ptr<Order> order) {
        // Implementation of order execution logic
        // This would interface with your actual trading API
        std::cout << "Executing order: " 
                  << (order->getSide() == OrderSide::BUY ? "BUY" : "SELL")
                  << " " << order->getQuantity() 
                  << " " << order->getContract().symbol()
                  << " [" << to_string(order->getPriority()) << "]\n";
        
        // Simulate order execution
        order->setStatus(OrderStatus::FILLED);
        order->setFilledQuantity(order->getQuantity());
    }

    ApiHandler api_handler_;
    std::queue<std::shared_ptr<Order>> order_queue_;
    std::mutex mutex_;
};
