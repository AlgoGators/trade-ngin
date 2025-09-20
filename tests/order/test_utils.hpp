//===== test_utils.hpp =====
#pragma once

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include "../core/test_base.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/order/order_manager.hpp"

namespace trade_ngin {
namespace testing {

class MarketDataCapture {
public:
    void on_event(const MarketDataEvent& event);
    std::vector<MarketDataEvent> get_events() const;
    void clear();
    size_t count() const;

private:
    mutable std::mutex mutex_;
    std::vector<MarketDataEvent> events_;
};

// Test helper functions
Order create_test_order();
ExecutionReport create_test_execution(const std::string& order_id, double filled_qty = 100,
                                      bool is_partial = false);
OrderManagerConfig create_test_config();

// Custom matchers
MATCHER_P(HasOrderId, order_id, "Order has ID") {
    return arg.order_id == order_id;
}

MATCHER_P(HasOrderStatus, status, "Order has status") {
    return arg.status == status;
}

MATCHER_P2(HasFilledQuantity, quantity, tolerance, "Order filled quantity matches") {
    return std::abs(arg.filled_quantity - quantity) <= tolerance;
}

}  // namespace testing
}  // namespace trade_ngin
