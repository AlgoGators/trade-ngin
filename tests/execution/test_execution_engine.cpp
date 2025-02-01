#include <gtest/gtest.h>
#include "trade_ngin/execution/execution_engine.hpp"
#include "trade_ngin/order/order_manager.hpp"
#include "../core/test_base.hpp"
#include "../order/test_utils.hpp"
#include <memory>
#include <chrono>

using namespace trade_ngin;
using namespace trade_ngin::testing;

class ExecutionEngineTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        
        // Create an order manager with simulation enabled
        OrderManagerConfig order_config = create_test_config();
        order_config.simulate_fills = true;
        order_manager_ = std::make_shared<OrderManager>(order_config);
        ASSERT_TRUE(order_manager_->initialize().is_ok());

        // Initialize execution engine
        engine_ = std::make_unique<ExecutionEngine>(order_manager_);
        ASSERT_TRUE(engine_->initialize().is_ok());
    }

    void TearDown() override {
        engine_.reset();
        order_manager_.reset();
        TestBase::TearDown();
    }

    std::shared_ptr<OrderManager> order_manager_;
    std::unique_ptr<ExecutionEngine> engine_;
};

TEST_F(ExecutionEngineTest, SimpleMarketOrder) {
    // Create a simple market order
    Order order;
    order.symbol = "AAPL";
    order.side = Side::BUY;
    order.type = OrderType::MARKET;
    order.quantity = 100;
    order.time_in_force = TimeInForce::DAY;

    // Create basic execution config
    ExecutionConfig config;
    config.max_participation_rate = 0.1;
    config.urgency_level = 0.5;
    config.time_horizon = std::chrono::minutes(10);

    // Submit for execution
    auto result = engine_->submit_execution(order, ExecutionAlgo::MARKET, config);
    ASSERT_TRUE(result.is_ok());
    
    std::string job_id = result.value();
    EXPECT_FALSE(job_id.empty());

    // Get execution metrics
    auto metrics_result = engine_->get_metrics(job_id);
    ASSERT_TRUE(metrics_result.is_ok());
    
    const auto& metrics = metrics_result.value();
    EXPECT_EQ(metrics.num_child_orders, 1);
    EXPECT_GT(metrics.completion_rate, 0.0);
}

TEST_F(ExecutionEngineTest, TWAPExecution) {
    Order order;
    order.symbol = "MSFT";
    order.side = Side::BUY;
    order.type = OrderType::LIMIT;
    order.quantity = 1000;
    order.price = 100.0;
    order.time_in_force = TimeInForce::DAY;

    ExecutionConfig config;
    config.max_participation_rate = 0.1;
    config.time_horizon = std::chrono::minutes(30);
    config.min_child_size = 100;

    auto result = engine_->submit_execution(order, ExecutionAlgo::TWAP, config);
    ASSERT_TRUE(result.is_ok());
    
    std::string job_id = result.value();

    // Verify job is active
    auto active_jobs = engine_->get_active_jobs();
    ASSERT_TRUE(active_jobs.is_ok());
    EXPECT_FALSE(active_jobs.value().empty());

    // Check metrics after some time
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    auto metrics_result = engine_->get_metrics(job_id);
    ASSERT_TRUE(metrics_result.is_ok());
    
    const auto& metrics = metrics_result.value();
    EXPECT_GT(metrics.num_child_orders, 1);  // Should split into multiple child orders
    EXPECT_GT(metrics.participation_rate, 0.0);
    EXPECT_LT(metrics.participation_rate, config.max_participation_rate);
}

TEST_F(ExecutionEngineTest, CancelExecution) {
    Order order;
    order.symbol = "GOOG";
    order.side = Side::SELL;
    order.type = OrderType::LIMIT;
    order.quantity = 500;
    order.price = 2500.0;
    
    ExecutionConfig config;
    config.time_horizon = std::chrono::minutes(60);

    auto submit_result = engine_->submit_execution(order, ExecutionAlgo::VWAP, config);
    ASSERT_TRUE(submit_result.is_ok());
    
    std::string job_id = submit_result.value();

    // Cancel the execution
    auto cancel_result = engine_->cancel_execution(job_id);
    ASSERT_TRUE(cancel_result.is_ok());

    // Verify job is no longer active
    auto active_jobs = engine_->get_active_jobs();
    ASSERT_TRUE(active_jobs.is_ok());
    
    bool job_found = false;
    for (const auto& job : active_jobs.value()) {
        if (job.job_id == job_id) {
            job_found = true;
            break;
        }
    }
    EXPECT_FALSE(job_found);
}

TEST_F(ExecutionEngineTest, ParticipationConstraints) {
    Order order;
    order.symbol = "AAPL";
    order.side = Side::BUY;
    order.type = OrderType::LIMIT;
    order.quantity = 10000;  // Large order
    order.price = 150.0;

    ExecutionConfig config;
    config.max_participation_rate = 0.05;  // 5% max participation
    config.min_child_size = 100;
    config.time_horizon = std::chrono::minutes(60);

    auto result = engine_->submit_execution(order, ExecutionAlgo::POV, config);
    ASSERT_TRUE(result.is_ok());
    
    std::string job_id = result.value();

    // Check execution metrics
    auto metrics_result = engine_->get_metrics(job_id);
    ASSERT_TRUE(metrics_result.is_ok());
    
    const auto& metrics = metrics_result.value();
    EXPECT_LE(metrics.participation_rate, config.max_participation_rate);
    EXPECT_GT(metrics.num_child_orders, 1);
}

TEST_F(ExecutionEngineTest, InvalidConfigurations) {
    Order order;
    order.symbol = "MSFT";
    order.side = Side::BUY;
    order.type = OrderType::LIMIT;
    order.quantity = 100;
    order.price = 200.0;

    // Test invalid participation rate
    ExecutionConfig invalid_config;
    invalid_config.max_participation_rate = 1.5;  // Over 100%
    
    auto result1 = engine_->submit_execution(order, ExecutionAlgo::VWAP, invalid_config);
    EXPECT_TRUE(result1.is_error());

    // Test invalid time horizon
    ExecutionConfig zero_time_config;
    zero_time_config.time_horizon = std::chrono::minutes(0);
    
    auto result2 = engine_->submit_execution(order, ExecutionAlgo::TWAP, zero_time_config);
    EXPECT_TRUE(result2.is_error());
}

TEST_F(ExecutionEngineTest, StressTest) {
    const int num_orders = 10;
    std::vector<std::string> job_ids;

    for (int i = 0; i < num_orders; ++i) {
        Order order;
        order.symbol = "AAPL";
        order.side = (i % 2 == 0) ? Side::BUY : Side::SELL;
        order.type = OrderType::LIMIT;
        order.quantity = 100 * (i + 1);
        order.price = 150.0;

        ExecutionConfig config;
        config.max_participation_rate = 0.1;
        config.time_horizon = std::chrono::minutes(30);

        auto result = engine_->submit_execution(
            order, 
            ExecutionAlgo::TWAP, 
            config
        );
        
        ASSERT_TRUE(result.is_ok());
        job_ids.push_back(result.value());
    }

    // Verify all jobs are being tracked
    auto active_jobs = engine_->get_active_jobs();
    ASSERT_TRUE(active_jobs.is_ok());
    EXPECT_GE(active_jobs.value().size(), num_orders);

    // Cancel half the jobs
    for (size_t i = 0; i < job_ids.size() / 2; ++i) {
        EXPECT_TRUE(engine_->cancel_execution(job_ids[i]).is_ok());
    }

    // Verify correct number of jobs remain active
    active_jobs = engine_->get_active_jobs();
    ASSERT_TRUE(active_jobs.is_ok());
    EXPECT_GE(active_jobs.value().size(), num_orders / 2);
}

TEST_F(ExecutionEngineTest, MetricsAccuracy) {
    Order order;
    order.symbol = "AAPL";
    order.side = Side::BUY;
    order.type = OrderType::LIMIT;
    order.quantity = 1000;
    order.price = 150.0;

    ExecutionConfig config;
    config.max_participation_rate = 0.1;
    config.urgency_level = 0.5;
    config.time_horizon = std::chrono::minutes(30);

    auto result = engine_->submit_execution(order, ExecutionAlgo::IS, config);
    ASSERT_TRUE(result.is_ok());
    
    std::string job_id = result.value();

    // Wait for some execution progress
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto metrics_result = engine_->get_metrics(job_id);
    ASSERT_TRUE(metrics_result.is_ok());
    
    const auto& metrics = metrics_result.value();

    // Verify metrics are within expected ranges
    EXPECT_GE(metrics.completion_rate, 0.0);
    EXPECT_LE(metrics.completion_rate, 1.0);
    
    EXPECT_GE(metrics.participation_rate, 0.0);
    EXPECT_LE(metrics.participation_rate, config.max_participation_rate);
    
    EXPECT_GE(metrics.implementation_shortfall, 0.0);
    EXPECT_GE(metrics.volume_participation, 0.0);
    
    EXPECT_GT(metrics.total_time.count(), 0);
    EXPECT_GT(metrics.num_child_orders, 0);
}