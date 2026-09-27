// include/trade_ngin/data/market_data_bus.hpp
#pragma once

#include <functional>
#include <atomic>
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
    struct RegistrationIdentity;
public:
    class ScopedSubscription {
    public:
        // The bus must outlive its handles. Concurrent operations on one handle
        // and callback-initiated reset/destruction are unsupported.
        ScopedSubscription() noexcept = default;
        ~ScopedSubscription() noexcept;
        ScopedSubscription(ScopedSubscription&& other) noexcept;
        ScopedSubscription& operator=(ScopedSubscription&& other) noexcept;
        ScopedSubscription(const ScopedSubscription&) = delete;
        ScopedSubscription& operator=(const ScopedSubscription&) = delete;

        void reset() noexcept;
        bool empty() const noexcept { return bus_ == nullptr; }

    private:
        friend class MarketDataBus;
        MarketDataBus* bus_{nullptr};
        std::string id_;
        std::shared_ptr<const RegistrationIdentity> identity_;
    };

    Result<void> subscribe_scoped(const SubscriberInfo& subscriber_info,
                                  ScopedSubscription& empty_output);

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
     * @brief Enable or disable publishing
     * @param enabled True to enable, false to disable
     * @note Use this during backtest data loading to prevent duplicate processing
     */
    void set_publish_enabled(bool enabled) { publish_enabled_.store(enabled, std::memory_order_release); }

    /**
     * @brief Check if publishing is enabled
     * @return True if publishing is enabled
     */
    bool is_publish_enabled() const { return publish_enabled_.load(std::memory_order_acquire); }

    /**
     * @brief Get singleton instance
     */
    static MarketDataBus& instance() {
        static MarketDataBus instance;
        return instance;
    }

private:
    friend class MarketDataBusTestPeer;
    MarketDataBus() = default;
    std::atomic<bool> publish_enabled_{true};  // Can be disabled during backtest data loading

    struct Subscription {
        std::vector<MarketDataEventType> event_types;
        std::vector<std::string> symbols;
        MarketDataCallback callback;
        bool active{true};
        std::shared_ptr<const RegistrationIdentity> identity;
    };

    std::unordered_map<std::string, Subscription> subscriptions_;
    mutable std::mutex mutex_;
    std::atomic<void (*)() noexcept> scoped_reset_contention_hook_{nullptr};

    bool should_notify(const Subscription& sub, const MarketDataEvent& event) const;
};

}  // namespace trade_ngin
