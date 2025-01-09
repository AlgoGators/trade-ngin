#include "trading_system.hpp"
#include <iostream>

int main() {
    try {
        // Initialize the trading system
        TradingSystem system(1000000.0, "your_databento_api_key");
        
        // Initialize instruments and strategies
        system.initialize();
        
        // Add a trend following strategy
        auto trend_strategy = std::make_shared<TrendFollowingStrategy>(
            500000.0, 50.0, 0.2
        );
        system.getPortfolio().addStrategy(trend_strategy, 0.7);
        
        // Run the system
        system.run();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
