// include/trade_ngin/data/market_data_bus.hpp
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

/**
 * @brief Type of market data event
 */
enum class MarketDataEventType {
    TRADE,
    QUOTE,
    BAR,
    POSITION_UPDATE,
    SIGNAL_UPDATE,
    RISK_UPDATE,
    ORDER_UPDATE
};

/**
 * @brief Market data event structure
 */
struct MarketDataEvent {
    MarketDataEventType type;
    std::string symbol;
    Timestamp timestamp;
    std::unordered_map<std::string, double> numeric_fields;
    std::unordered_map<std::string, std::string> string_fields;
};

/**
 * @brief Callback type for market data events
 */
using MarketDataCallback = std::function<void(const MarketDataEvent&)>;

/**
 * @brief Subscriber info structure
 */
struct SubscriberInfo {
    std::string id;
    std::vector<MarketDataEventType> event_types;
    std::vector<std::string> symbols;
    MarketDataCallback callback;
};

/**
 * @brief Market data event bus for distributing data to components
 */
class MarketDataBus {
public:
    /**
     * @brief Subscribe to market data events
     * @param subscriber_info Subscriber configuration
     * @return Result indicating success or failure
     */
    Result<void> subscribe(const SubscriberInfo& subscriber_info);

    /**
     * @brief Unsubscribe from market data events
     * @param subscriber_id Subscriber identifier
     * @return Result indicating success or failure
     */
    Result<void> unsubscribe(const std::string& subscriber_id);

    /**
     * @brief Publish market data event
     * @param event Event to publish
     */
    void publish(const MarketDataEvent& event);

    /**
     * @brief Get singleton instance
     */
    static MarketDataBus& instance() {
        static MarketDataBus instance;
        return instance;
    }

private:
    MarketDataBus() = default;

    struct Subscription {
        std::vector<MarketDataEventType> event_types;
        std::vector<std::string> symbols;
        MarketDataCallback callback;
        bool active{true};
    };

    std::unordered_map<std::string, Subscription> subscriptions_;
    mutable std::mutex mutex_;

    bool should_notify(const Subscription& sub, const MarketDataEvent& event) const;
};

}  // namespace trade_ngin
