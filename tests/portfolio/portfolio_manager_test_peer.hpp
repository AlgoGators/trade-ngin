#pragma once

#include <mutex>
#include "trade_ngin/portfolio/portfolio_manager.hpp"

namespace trade_ngin {
// Test-only access; the production class definition and layout are identical in every target.
class PortfolioManagerTestPeer {
public:
    // Acquires the manager's internal mutex_ for the caller, RAII-released on
    // destruction (or earlier via explicit unlock()). Mirrors the direct-mutex
    // access MarketDataBusTestPeer already uses for the bus's own mutex.
    static std::unique_lock<std::mutex> lock_manager_mutex(PortfolioManager& pm) {
        return std::unique_lock<std::mutex>(pm.mutex_);
    }

    static void set_position_update_contention_hook(PortfolioManager& pm,
                                                     void (*hook)() noexcept) noexcept {
        pm.position_update_contention_hook_.store(hook, std::memory_order_release);
    }
};
}  // namespace trade_ngin
