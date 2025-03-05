#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <unordered_map>
#include "contract.hpp"

namespace ibkr {

enum class TickType {
    BID,
    ASK,
    LAST,
    HIGH,
    LOW,
    VOLUME,
    UNKNOWN
};

struct MarketDataUpdate {
    Contract contract;
    TickType tickType;
    double price;
    int size;
    std::string timestamp;
};

class MarketDataHandler {
public:
    using DataCallback = std::function<void(const MarketDataUpdate&)>;
    
    virtual ~MarketDataHandler() = default;
    
    // Real-time market data
    virtual void subscribeMarketData(const Contract& contract,
                                   const std::vector<std::string>& genericTicks,
                                   bool snapshot = false) = 0;
    
    virtual void unsubscribeMarketData(const Contract& contract) = 0;
};

} // namespace ibkr
