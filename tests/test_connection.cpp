#include <iostream>
#include <thread>
#include <chrono>
#include "../trade_ngin/system/ibkr_interface.hpp"
#include <spdlog/spdlog.h>

void onMarketData(const std::string& symbol, double price, double volume) {
    std::cout << "Market Data - Symbol: " << symbol 
              << " Price: " << price 
              << " Volume: " << volume << std::endl;
}

int main() {
    spdlog::set_level(spdlog::level::debug);
    
    // Create interface instance
    IBKRInterface ibkr;
    
    // Connect to IB Gateway
    std::cout << "Connecting to IB Gateway..." << std::endl;
    if (!ibkr.connect()) {
        std::cerr << "Failed to connect!" << std::endl;
        return 1;
    }
    std::cout << "Connected successfully!" << std::endl;
    
    // Set up market data callback
    ibkr.setMarketDataCallback(onMarketData);
    
    // Create a test futures contract (ES - E-mini S&P 500)
    Contract es;
    es.symbol = "ES";
    es.secType = SecType::FUT;
    es.exchange = "CME";
    es.currency = "USD";
    es.lastTradeDateOrContractMonth = "202403"; // March 2024 contract
    
    // Request market data
    std::cout << "Requesting market data for ES futures..." << std::endl;
    if (!ibkr.requestMarketData(es)) {
        std::cerr << "Failed to request market data!" << std::endl;
        return 1;
    }
    
    // Keep program running to receive data
    std::cout << "Receiving data for 30 seconds..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(30));
    
    // Disconnect
    ibkr.disconnect();
    std::cout << "Test complete." << std::endl;
    
    return 0;
}
