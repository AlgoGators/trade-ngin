#pragma once
#include <memory>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include "adaptive_order_priority.hpp"
#include "../data/portfolio_manager.hpp"
#include "../data/market_data.hpp"

class MockTradingPlatform {
public:
    MockTradingPlatform(std::shared_ptr<PortfolioManager> portfolio_manager)
        : portfolio_manager_(portfolio_manager),
          order_manager_(std::make_unique<OrderManager>()),
          is_running_(false) {}

    // Start the trading platform
    void start() {
        if (is_running_) return;
        is_running_ = true;
        trading_thread_ = std::thread(&MockTradingPlatform::tradingLoop, this);
    }

    // Stop the trading platform
    void stop() {
        is_running_ = false;
        if (trading_thread_.joinable()) {
            trading_thread_.join();
        }
    }

private:
    void tradingLoop() {
        while (is_running_) {
            try {
                // Get portfolio state
                auto positions = portfolio_manager_->getPositions();
                double total_capital = portfolio_manager_->getTotalCapital();
                double available_capital = portfolio_manager_->getAvailableCapital();

                // Process each position
                for (const auto& [symbol, position] : positions) {
                    if (position.status == "OPEN") {
                        // Create contract for the instrument
                        Contract contract(symbol);

                        // Check if we need to close or adjust position
                        if (position.quantity != 0) {
                            // Create and submit order
                            auto order = std::make_shared<Order>(
                                contract,
                                position.side == "LONG" ? OrderSide::BUY : OrderSide::SELL,
                                std::abs(position.quantity),
                                OrderType::MARKET,
                                AdaptiveOrderPriority::NORMAL
                            );

                            order_manager_->submitOrder(order);
                        }
                    }
                }

                // Sleep for a bit to avoid busy waiting
                std::this_thread::sleep_for(std::chrono::seconds(1));
            } catch (const std::exception& e) {
                std::cerr << "Error in trading loop: " << e.what() << std::endl;
            }
        }
    }

    std::shared_ptr<PortfolioManager> portfolio_manager_;
    std::unique_ptr<OrderManager> order_manager_;
    std::thread trading_thread_;
    std::atomic<bool> is_running_;
}; 