#pragma once

#include "trade_ngin/data/market_data_bus.hpp"

namespace trade_ngin {
// Test-only access; the production class definition and layout are identical in every target.
class MarketDataBusTestPeer {
public:
    static bool is_active(MarketDataBus& bus, const std::string& id) {
        std::lock_guard<std::mutex> lock(bus.mutex_);
        const auto it = bus.subscriptions_.find(id);
        return it != bus.subscriptions_.end() && it->second.active;
    }

    static void set_contention_hook(MarketDataBus& bus, void (*hook)() noexcept) noexcept {
        bus.scoped_reset_contention_hook_.store(hook, std::memory_order_release);
    }
};
}  // namespace trade_ngin
