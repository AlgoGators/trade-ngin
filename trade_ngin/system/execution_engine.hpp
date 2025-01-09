#pragma once
#include <queue>
#include <atomic>
#include "adaptive_order_priority.hpp"

class ExecutionEngine {
public:
    struct ExecutionMetrics {
        double slippage;
        double market_impact;
        double fill_rate;
        double participation_rate;
        std::chrono::microseconds latency;
    };

    ExecutionEngine(std::shared_ptr<OrderManager> order_manager);
    
    void submitOrder(std::shared_ptr<Order> order);
    void optimizeExecution(const Portfolio& portfolio);
    ExecutionMetrics getMetrics() const;

private:
    void executeViaAlgo(std::shared_ptr<Order> order, const std::string& algo);
    void executeViaSmartRouter(std::shared_ptr<Order> order);
    void executeViaDarkPool(std::shared_ptr<Order> order);

    std::shared_ptr<OrderManager> order_manager_;
    std::atomic<bool> running_{true};
    ExecutionMetrics metrics_;
}; 