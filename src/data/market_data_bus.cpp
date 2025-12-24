// src/data/market_data_bus.cpp
#include "trade_ngin/data/market_data_bus.hpp"
#include <algorithm>
#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

Result<void> MarketDataBus::subscribe(const SubscriberInfo& subscriber_info) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (subscriber_info.id.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Subscriber ID cannot be empty",
                                "MarketDataBus");
    }

    if (subscriber_info.event_types.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Must subscribe to at least one event type", "MarketDataBus");
    }

    if (!subscriber_info.callback) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Callback function cannot be null",
                                "MarketDataBus");
    }

    // Create or update subscription
    Subscription sub{subscriber_info.event_types, subscriber_info.symbols, subscriber_info.callback,
                     true};

    subscriptions_[subscriber_info.id] = std::move(sub);

    INFO("Added subscription for " + subscriber_info.id + " with " +
         std::to_string(subscriber_info.event_types.size()) + " event types and " +
         std::to_string(subscriber_info.symbols.size()) + " symbols");

    return Result<void>();
}

Result<void> MarketDataBus::unsubscribe(const std::string& subscriber_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = subscriptions_.find(subscriber_id);
    if (it == subscriptions_.end()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Subscriber ID not found: " + subscriber_id, "MarketDataBus");
    }

    it->second.active = false;
    INFO("Deactivated subscription for " + subscriber_id);

    return Result<void>();
}

void MarketDataBus::publish(const MarketDataEvent& event) {
    // Early return if publishing is disabled (e.g., during backtest data loading)
    if (!publish_enabled_) {
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(mutex_);

        // Notify each active subscriber if they should receive this event
        for (const auto& [id, sub] : subscriptions_) {
            if (sub.active && should_notify(sub, event)) {
                try {
                    sub.callback(event);
                } catch (const std::exception& e) {
                    ERROR("Error in subscriber callback for " + id + ": " + e.what());
                }
            }
        }

    } catch (const std::exception& e) {
        ERROR("Error publishing event: " + std::string(e.what()));
    }
}

bool MarketDataBus::should_notify(const Subscription& sub, const MarketDataEvent& event) const {
    // Check if subscriber is interested in this event type
    if (std::find(sub.event_types.begin(), sub.event_types.end(), event.type) ==
        sub.event_types.end()) {
        return false;
    }

    // If no symbols specified, subscriber wants all symbols
    if (sub.symbols.empty()) {
        return true;
    }

    // Check if subscriber is interested in this symbol
    return std::find(sub.symbols.begin(), sub.symbols.end(), event.symbol) != sub.symbols.end();
}

}  // namespace trade_ngin