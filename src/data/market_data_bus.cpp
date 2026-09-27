// src/data/market_data_bus.cpp
#include "trade_ngin/data/market_data_bus.hpp"
#include <algorithm>
#include <type_traits>
#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

struct MarketDataBus::RegistrationIdentity {};

MarketDataBus::ScopedSubscription::~ScopedSubscription() noexcept { reset(); }
MarketDataBus::ScopedSubscription::ScopedSubscription(ScopedSubscription&& other) noexcept
    : bus_(other.bus_), id_(std::move(other.id_)), identity_(std::move(other.identity_)) {
    other.bus_ = nullptr;
}
MarketDataBus::ScopedSubscription& MarketDataBus::ScopedSubscription::operator=(ScopedSubscription&& other) noexcept {
    if (this != &other) {
        reset();
        bus_ = other.bus_;
        id_ = std::move(other.id_);
        identity_ = std::move(other.identity_);
        other.bus_ = nullptr;
    }
    return *this;
}
void MarketDataBus::ScopedSubscription::reset() noexcept {
    if (!bus_) return;
    // Dispatch holds this same nonrecursive mutex through callback return. External
    // reset therefore waits for an in-flight invocation before deactivating it.
    // Callback-initiated reset/destruction and reentrant bus calls are unsupported.
    std::unique_lock<std::mutex> lock(bus_->mutex_, std::defer_lock);
    if (auto hook = bus_->scoped_reset_contention_hook_.load(std::memory_order_acquire)) {
        if (!lock.try_lock()) {
            hook();  // Private, inert unless installed by the synchronized test peer.
            lock.lock();
        }
    } else {
        lock.lock();
    }
    auto it = bus_->subscriptions_.find(id_);
    if (it != bus_->subscriptions_.end() && it->second.identity == identity_)
        it->second.active = false;
    lock.unlock();
    id_.clear();
    identity_.reset();
    bus_ = nullptr;
}

Result<void> MarketDataBus::subscribe_scoped(const SubscriberInfo& info,
                                             ScopedSubscription& output) {
    if (!output.empty())
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Output subscription must be empty", "MarketDataBus");
    if (info.id.empty())
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Subscriber ID cannot be empty", "MarketDataBus");
    if (info.event_types.empty())
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Must subscribe to at least one event type", "MarketDataBus");
    if (!info.callback)
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Callback function cannot be null", "MarketDataBus");

    // All user callback copies and allocation happen before the map can change.
    auto identity = std::make_shared<RegistrationIdentity>();
    std::string key = info.id;
    std::string owner_key = info.id;
    Subscription candidate{info.event_types, info.symbols, info.callback, true, identity};
    static_assert(std::is_nothrow_move_constructible_v<Subscription> &&
                  std::is_nothrow_move_assignable_v<Subscription>);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = subscriptions_.find(key);
        if (it == subscriptions_.end()) {
            subscriptions_.emplace(std::move(key), std::move(candidate));
        } else {
            using std::swap;
            swap(it->second, candidate);
        }
        // Moves into the output cannot throw. It is armed before a publisher
        // can acquire the mutex and call the newly installed raw target.
        output.id_ = std::move(owner_key);
        output.identity_ = std::move(identity);
        output.bus_ = this;
    }
    return Result<void>();
}

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
                     true, nullptr};

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
    if (!publish_enabled_.load(std::memory_order_acquire)) {
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
