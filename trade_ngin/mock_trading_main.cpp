#include "system/mock_trading_platform.hpp"
#include "data/portfolio_manager.hpp"
#include <iostream>
#include <memory>
#include <csignal>

std::atomic<bool> running{true};

void signalHandler(int signum) {
    std::cout << "\nInterrupt signal received. Shutting down..." << std::endl;
    running = false;
}

int main() {
    try {
        // Set up signal handling
        signal(SIGINT, signalHandler);

        // Create portfolio manager with initial capital
        auto portfolio_manager = std::make_shared<PortfolioManager>(500000.0);

        // Open some test positions
        portfolio_manager->openPosition("MES", 1, 4800.0, "LONG");
        portfolio_manager->openPosition("MNQ", 1, 17000.0, "SHORT");

        // Create and start mock trading platform
        MockTradingPlatform platform(portfolio_manager);
        platform.start();

        std::cout << "Mock trading platform started. Press Ctrl+C to stop." << std::endl;

        // Keep running until interrupted
        while (running) {
            // Print current portfolio state every 5 seconds
            std::cout << "\nPortfolio State:" << std::endl;
            std::cout << "Total Capital: $" << portfolio_manager->getTotalCapital() << std::endl;
            std::cout << "Available Capital: $" << portfolio_manager->getAvailableCapital() << std::endl;
            std::cout << "Unrealized P&L: $" << portfolio_manager->getUnrealizedPnL() << std::endl;
            std::cout << "Realized P&L: $" << portfolio_manager->getRealizedPnL() << std::endl;

            std::cout << "\nPositions:" << std::endl;
            for (const auto& [symbol, pos] : portfolio_manager->getPositions()) {
                std::cout << symbol << ": " 
                         << pos.quantity << " @ $" << pos.entry_price 
                         << " (" << pos.side << ")" << std::endl;
            }

            std::this_thread::sleep_for(std::chrono::seconds(5));
        }

        // Clean shutdown
        platform.stop();
        std::cout << "Mock trading platform stopped." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
} 