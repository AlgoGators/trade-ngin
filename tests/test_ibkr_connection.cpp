#include "trade_ngin/system/ibkr_interface.hpp"
#include <spdlog/spdlog.h>
#include <iostream>
#include <thread>
#include <chrono>

using namespace std::chrono_literals;

int main() {
    try {
        // Initialize logging
        spdlog::set_level(spdlog::level::debug);
        spdlog::info("Starting IBKR connection test...");

        // Create interface
        ibkr::IBKRInterface interface;

        // Connect to TWS
        if (!interface.connect()) {
            spdlog::error("Failed to connect to TWS");
            return 1;
        }

        spdlog::info("Successfully connected to TWS");

        // Create a test contract
        ibkr::Contract contract;
        contract.symbol = "AAPL";
        contract.secType = "STK";
        contract.exchange = "SMART";
        contract.currency = "USD";

        // Set up market data callback
        interface.setMarketDataCallback([](const ibkr::MarketDataUpdate& update) {
            spdlog::info("Received market data: Price={}, Size={}", update.price, update.size);
        });

        // Request market data
        if (!interface.requestMarketData(contract)) {
            spdlog::error("Failed to request market data");
            return 1;
        }

        spdlog::info("Requested market data for AAPL");

        // Wait for some data
        std::this_thread::sleep_for(10s);

        // Cleanup
        interface.disconnect();
        spdlog::info("Test completed successfully");
        return 0;

    } catch (const std::exception& e) {
        spdlog::error("Error: {}", e.what());
        return 1;
    }
}
