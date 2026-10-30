#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include <unordered_map>
#include <vector>
#include <string>

namespace trade_ngin {

/**
 * Base class for price management - extracts price logic from live_trend.cpp
 * Goal: Reduce monolithic code by modularizing price retrieval and caching
 */
class PriceManagerBase {
public:
    /**
     * Price data structure with metadata
     */
    struct PriceData {
        double price;
        Timestamp timestamp;
        bool is_settlement;

        PriceData() : price(0.0), is_settlement(false) {}
        PriceData(double p, const Timestamp& t, bool s = false)
            : price(p), timestamp(t), is_settlement(s) {}
    };

    virtual ~PriceManagerBase() = default;

    /**
     * Core interface - must be implemented by all derived classes
     */

    // Get price for a single symbol at a specific timestamp
    virtual Result<double> get_price(
        const std::string& symbol,
        const Timestamp& timestamp) const = 0;

    // Get prices for multiple symbols at a specific timestamp
    virtual Result<std::unordered_map<std::string, double>> get_prices(
        const std::vector<std::string>& symbols,
        const Timestamp& timestamp) const = 0;
};

} // namespace trade_ngin