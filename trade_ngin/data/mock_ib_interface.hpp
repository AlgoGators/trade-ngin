#pragma once

#include <string>
#include <iostream>
#include <iomanip>

class MockIBInterface {
public:
    MockIBInterface() {
        std::cout << "Initializing IB Paper Trading Interface..." << std::endl;
    }

    void placeOrder(const std::string& symbol, double quantity, double price, bool is_buy) {
        // Mock order execution - in reality this would connect to IB API
        std::cout << "IB Paper Trade: " << (is_buy ? "BUY " : "SELL ") 
                  << std::abs(quantity) << " " << symbol 
                  << " @ $" << std::fixed << std::setprecision(2) << price << std::endl;
    }

    double getLastPrice(const std::string& symbol) {
        // Mock price retrieval - in reality this would get real-time price from IB
        return 0.0;  // Not implemented for now
    }

    double getPosition(const std::string& symbol) {
        // Mock position retrieval - in reality this would get current position from IB
        return 0.0;  // Not implemented for now
    }
}; 