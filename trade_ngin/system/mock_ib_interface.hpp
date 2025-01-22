#pragma once
#include <string>
#include <iostream>
#include <unordered_map>
#include <vector>

class MockIBInterface {
public:
    MockIBInterface() = default;

    void addSymbol(const std::string& symbol) {
        prices[symbol] = 0.0;
        available_symbols.push_back(symbol);
    }

    void setPrice(const std::string& symbol, double price) {
        prices[symbol] = price;
    }

    double getPrice(const std::string& symbol) const {
        auto it = prices.find(symbol);
        if (it == prices.end()) {
            throw std::runtime_error("Symbol not found: " + symbol);
        }
        return it->second;
    }

    std::vector<std::string> getAvailableSymbols() const {
        return available_symbols;
    }

    void placeOrder(const std::string& symbol, double quantity, double price, bool is_buy) {
        std::cout << "Mock IB Order: " << (is_buy ? "BUY " : "SELL ")
                  << quantity << " " << symbol << " @ $" << price << std::endl;
    }

private:
    std::unordered_map<std::string, double> prices;
    std::vector<std::string> available_symbols;
}; 