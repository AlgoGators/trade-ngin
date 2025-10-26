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
protected:
    // Price validation ranges
    static constexpr double MIN_VALID_PRICE = 0.0001;
    static constexpr double MAX_VALID_PRICE = 1000000.0;

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

    /**
     * Common validation and utility methods
     */

    // Validate if a price is within reasonable bounds
    bool is_valid_price(double price) const {
        return price > MIN_VALID_PRICE && price < MAX_VALID_PRICE;
    }

    // Interpolate price between two points (for future use)
    double interpolate_price(double prev_price, double next_price, double ratio) const {
        return prev_price + (next_price - prev_price) * ratio;
    }

protected:
    // Helper method to validate a collection of prices
    Result<void> validate_prices(const std::unordered_map<std::string, double>& prices) const {
        for (const auto& [symbol, price] : prices) {
            if (!is_valid_price(price)) {
                return make_error<void>(ErrorCode::INVALID_DATA,
                    "Invalid price for " + symbol + ": " + std::to_string(price));
            }
        }
        return Result<void>();  // Default constructor for success
    }
};

} // namespace trade_ngin