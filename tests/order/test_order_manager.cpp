//===== test_order_manager.cpp =====
#include <gtest/gtest.h>
#include <thread>
#include "test_utils.hpp"
#include "trade_ngin/order/order_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

class OrderManagerTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();

        test_id_ = "OrderManager_" + std::to_string(test_counter_++);
        config_ = create_test_config();
        order_manager_ = std::make_unique<OrderManager>(config_);
        ASSERT_TRUE(order_manager_->initialize().is_ok());
    }

    void TearDown() override {
        if (order_manager_) {
            order_manager_.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        TestBase::TearDown();
    }

    static int test_counter_;
    std::string test_id_;
    OrderManagerConfig config_;
    std::unique_ptr<OrderManager> order_manager_;
};

int OrderManagerTest::test_counter_ = 0;

TEST_F(OrderManagerTest, SubmitOrderSuccess) {
    config_.simulate_fills = false;
    order_manager_ = std::make_unique<OrderManager>(config_);
    ASSERT_TRUE(order_manager_->initialize().is_ok());

    Order order = create_test_order();
    auto result = order_manager_->submit_order(order, "TEST_STRATEGY");
    ASSERT_TRUE(result.is_ok());

    std::string order_id = result.value();
    EXPECT_FALSE(order_id.empty());

    auto status_result = order_manager_->get_order_status(order_id);
    ASSERT_TRUE(status_result.is_ok());
    EXPECT_EQ(status_result.value().status, OrderStatus::ACCEPTED);
}

TEST_F(OrderManagerTest, InvalidOrder) {
    Order order = create_test_order();
    order.quantity = -100;  // Invalid quantity

    auto result = order_manager_->submit_order(order, "TEST_STRATEGY");
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ORDER);
}

TEST_F(OrderManagerTest, OrderCancellation) {
    // Disable simulate_fills
    config_.simulate_fills = false;
    order_manager_ = std::make_unique<OrderManager>(config_);
    ASSERT_TRUE(order_manager_->initialize().is_ok());

    Order order = create_test_order();
    auto submit_result = order_manager_->submit_order(order, "TEST_STRATEGY");
    ASSERT_TRUE(submit_result.is_ok());

    auto cancel_result = order_manager_->cancel_order(submit_result.value());
    EXPECT_TRUE(cancel_result.is_ok());

    auto status_result = order_manager_->get_order_status(submit_result.value());
    ASSERT_TRUE(status_result.is_ok());
    EXPECT_EQ(status_result.value().status, OrderStatus::CANCELLED);
}

TEST_F(OrderManagerTest, PartialFills) {
    // Disable simulate_fills
    config_.simulate_fills = false;
    order_manager_ = std::make_unique<OrderManager>(config_);
    ASSERT_TRUE(order_manager_->initialize().is_ok());

    Order order = create_test_order();
    order.quantity = 200;

    auto submit_result = order_manager_->submit_order(order, "TEST_STRATEGY");
    ASSERT_TRUE(submit_result.is_ok());

    std::string order_id = submit_result.value();

    ExecutionReport exec1 = create_test_execution(order_id, 100, true);
    ASSERT_TRUE(order_manager_->process_execution(exec1).is_ok());

    auto status1 = order_manager_->get_order_status(order_id);
    ASSERT_TRUE(status1.is_ok());
    EXPECT_EQ(status1.value().status, OrderStatus::PARTIALLY_FILLED);
    EXPECT_NEAR(status1.value().filled_quantity, 100, 1e-6);  // Use tolerance

    ExecutionReport exec2 = create_test_execution(order_id, 100, false);
    ASSERT_TRUE(order_manager_->process_execution(exec2).is_ok());

    auto status2 = order_manager_->get_order_status(order_id);
    ASSERT_TRUE(status2.is_ok());
    EXPECT_EQ(status2.value().status, OrderStatus::FILLED);
    EXPECT_NEAR(status2.value().filled_quantity, 200, 1e-6);  // Use tolerance
}

TEST_F(OrderManagerTest, GetStrategyOrders) {
    std::string strategy1 = "STRATEGY_1";
    std::string strategy2 = "STRATEGY_2";

    for (const auto& strategy_id : {strategy1, strategy2}) {
        for (int i = 0; i < 2; ++i) {
            Order order = create_test_order();
            ASSERT_TRUE(order_manager_->submit_order(order, strategy_id).is_ok());
        }
    }

    auto result1 = order_manager_->get_strategy_orders(strategy1);
    ASSERT_TRUE(result1.is_ok());
    EXPECT_EQ(result1.value().size(), 2);

    auto result2 = order_manager_->get_strategy_orders(strategy2);
    ASSERT_TRUE(result2.is_ok());
    EXPECT_EQ(result2.value().size(), 2);
}

TEST_F(OrderManagerTest, GetActiveOrders) {
    // Disable simulate_fills
    config_.simulate_fills = false;
    order_manager_ = std::make_unique<OrderManager>(config_);
    ASSERT_TRUE(order_manager_->initialize().is_ok());

    std::vector<std::string> order_ids;

    // Submit 3 orders
    for (int i = 0; i < 3; ++i) {
        auto result = order_manager_->submit_order(create_test_order(), "TEST_STRATEGY");
        ASSERT_TRUE(result.is_ok());
        order_ids.push_back(result.value());
    }

    // Fill one order
    ExecutionReport exec = create_test_execution(order_ids[0]);
    ASSERT_TRUE(order_manager_->process_execution(exec).is_ok());

    // Cancel one order
    ASSERT_TRUE(order_manager_->cancel_order(order_ids[1]).is_ok());

    auto active_orders = order_manager_->get_active_orders();
    ASSERT_TRUE(active_orders.is_ok());
    EXPECT_EQ(active_orders.value().size(), 1);
    EXPECT_EQ(active_orders.value()[0].order_id, order_ids[2]);
}
