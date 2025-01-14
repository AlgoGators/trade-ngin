#pragma once

#include <vector>
#include <string>

struct MarketData {
    std::string timestamp;
    std::string symbol;
    double open;
    double high;
    double low;
    double close;
    double volume;
};

// Function to get market data for a given symbol
std::vector<MarketData> getMarketData(const std::string& symbol); 