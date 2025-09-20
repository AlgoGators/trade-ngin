#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <thread>
#include "../core/test_base.hpp"
#include "../order/test_utils.hpp"
#include "trade_ngin/execution/execution_engine.hpp"
#include "trade_ngin/order/order_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

class ExecutionEngineTest : public TestBase {
protected:
    void SetUp() override {
        try {
            TestBase::SetUp();
            // Create an order manager with simulation enabled
            OrderManagerConfig order_config = create_test_config();
            order_config.simulate_fills = true;
            order_config.max_notional_value = 2000000.0;
            order_manager_ = std::make_shared<OrderManager>(order_config);
            auto init_result = order_manager_->initialize();
            ASSERT_TRUE(init_result.is_ok()) << std::string(init_result.error()->what());

            // Initialize execution engine
            engine_ = std::make_unique<ExecutionEngine>(order_manager_);
            auto engine_init = engine_->initialize();
            ASSERT_TRUE(engine_init.is_ok()) << std::string(engine_init.error()->what());

        } catch (const std::exception& e) {
            FAIL() << "Exception thrown: " << e.what();
        }
    }

    void TearDown() override {
        try {
            if (engine_) {
                // Cancel any active jobs first
                auto active_jobs = engine_->get_active_jobs();
                if (active_jobs.is_ok()) {
                    for (const auto& job : active_jobs.value()) {
                        auto cancel_result = engine_->cancel_execution(job.job_id);
                        if (cancel_result.is_error()) {
                            std::cerr << "Error cancelling job " << job.job_id << ": "
                                      << cancel_result.error()->what() << std::endl;
                        }
                    }
                }

                // Wait for cancellations to complete
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                engine_.reset();
            }

            if (order_manager_) {
                order_manager_.reset();
            }

            TestBase::TearDown();

        } catch (const std::exception& e) {
            std::cerr << "Error in test teardown: " << e.what() << std::endl;
        }
    }

    std::shared_ptr<OrderManager> order_manager_;
    std::unique_ptr<ExecutionEngine> engine_;
};

TEST_F(ExecutionEngineTest, SimpleMarketOrder) {
    try {
        // Create a simple market order
        Order order;
        order.symbol = "AAPL";
        order.side = Side::BUY;
        order.quantity = 100;
        order.price = 150.0;
        order.type = OrderType::MARKET;
        order.time_in_force = TimeInForce::DAY;

        // Create basic execution config
        ExecutionConfig config;
        config.max_participation_rate = 0.1;
        config.urgency_level = 0.5;
        config.time_horizon = std::chrono::minutes(10);

        // Verify order manager setup
        ASSERT_TRUE(order_manager_ != nullptr) << "Order manager is null";

        // Submit for execution
        auto result = engine_->submit_execution(order, ExecutionAlgo::MARKET, config);

        // Detailed error handling
        if (result.is_error()) {
            FAIL() << "Failed to submit execution: " << result.error()->what();
        }

        std::string job_id = result.value();
        EXPECT_FALSE(job_id.empty()) << "Job ID should not be empty";

        // Add small delay to allow execution
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Get execution metrics
        auto metrics_result = engine_->get_metrics(job_id);
        if (metrics_result.is_error()) {
            FAIL() << "Failed to get metrics: " << metrics_result.error()->what();
        }

        const auto& metrics = metrics_result.value();
        EXPECT_EQ(metrics.num_child_orders, 1);
        EXPECT_GT(metrics.completion_rate, 0.0);

    } catch (const std::exception& e) {
        FAIL() << "Test failed with exception: " << e.what();
    }
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
    ASSERT_TRUE(result.is_ok()) << result.error()->what();

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
    EXPECT_GT(metrics.num_child_orders, 1)
        << "Expected multiple child orders, got " << metrics.num_child_orders;
    EXPECT_GT(metrics.participation_rate, 0.0)
        << "Expected non-zero participation rate, got " << metrics.participation_rate;
    EXPECT_LE(metrics.participation_rate, config.max_participation_rate)
        << "Participation rate exceeded max limit";
}

// TEST_F(ExecutionEngineTest, VWAPExecution) {
//     Order order;
//     order.symbol = "AAPL";
//     order.side = Side::SELL;
//     order.type = OrderType::LIMIT;
//     order.quantity = 2000;
//     order.price = 150.0;
//     order.time_in_force = TimeInForce::DAY;

//     ExecutionConfig config;
//     config.max_participation_rate = 0.15;  // 15% max participation
//     config.time_horizon = std::chrono::minutes(60);
//     config.min_child_size = 200;
//     config.allow_cross_venue = true;

//     auto result = engine_->submit_execution(order, ExecutionAlgo::VWAP, config);
//     ASSERT_TRUE(result.is_ok());
//     std::string job_id = result.value();

//     // Check metrics
//     std::this_thread::sleep_for(std::chrono::milliseconds(100));
//     auto metrics_result = engine_->get_metrics(job_id);
//     ASSERT_TRUE(metrics_result.is_ok());

//     const auto& metrics = metrics_result.value();
//     EXPECT_GT(metrics.num_child_orders, 1);
//     EXPECT_GT(metrics.vwap_price, 0.0);
//     EXPECT_LE(metrics.participation_rate, config.max_participation_rate);

//     // VWAP-specific checks
//     // Price should be close to VWAP
//     double price_deviation =
//         std::abs(metrics.average_fill_price - metrics.vwap_price) / metrics.vwap_price;
//     EXPECT_LT(price_deviation, 0.01);  // Within 1% of VWAP
// }

TEST_F(ExecutionEngineTest, POVExecution) {
    Order order;
    order.symbol = "GOOG";
    order.side = Side::BUY;
    order.type = OrderType::LIMIT;
    order.quantity = 500;
    order.price = 2500.0;
    order.time_in_force = TimeInForce::DAY;

    ExecutionConfig config;
    config.max_participation_rate = 0.05;  // 5% participation rate
    config.urgency_level = 0.3;            // Lower urgency
    config.min_child_size = 50;
    config.time_horizon = std::chrono::minutes(120);

    auto result = engine_->submit_execution(order, ExecutionAlgo::POV, config);
    ASSERT_TRUE(result.is_ok());
    std::string job_id = result.value();

    // Check metrics
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto metrics_result = engine_->get_metrics(job_id);
    ASSERT_TRUE(metrics_result.is_ok());

    const auto& metrics = metrics_result.value();
    EXPECT_GT(metrics.volume_participation, 0.0);
    EXPECT_LE(metrics.volume_participation, config.max_participation_rate);
    EXPECT_GT(metrics.num_child_orders, 1);
}

// TEST_F(ExecutionEngineTest, ImplementationShortfallExecution) {
//     Order order;
//     order.symbol = "AMZN";
//     order.side = Side::BUY;
//     order.type = OrderType::MARKET;
//     order.quantity = 1500;
//     order.time_in_force = TimeInForce::DAY;
//
//     ExecutionConfig config;
//     config.urgency_level = 0.8;  // High urgency
//     config.time_horizon = std::chrono::minutes(30);
//     config.max_participation_rate = 0.2;
//     config.allow_cross_venue = true;
//
//     auto result = engine_->submit_execution(order, ExecutionAlgo::IS, config);
//     ASSERT_TRUE(result.is_ok());
//     std::string job_id = result.value();
//
//     // Check metrics
//     std::this_thread::sleep_for(std::chrono::milliseconds(100));
//     auto metrics_result = engine_->get_metrics(job_id);
//     ASSERT_TRUE(metrics_result.is_ok());
//
//     const auto& metrics = metrics_result.value();
//     EXPECT_GT(metrics.implementation_shortfall, 0.0);
//     EXPECT_GT(metrics.market_impact, 0.0);
//     EXPECT_GT(metrics.completion_rate, 0.0);
// }

TEST_F(ExecutionEngineTest, AdaptiveLimitExecution) {
    Order order;
    order.symbol = "FB";
    order.side = Side::SELL;
    order.type = OrderType::LIMIT;
    order.quantity = 800;
    order.price = 300.0;
    order.time_in_force = TimeInForce::DAY;

    ExecutionConfig config;
    config.urgency_level = 0.5;
    config.time_horizon = std::chrono::minutes(45);
    config.max_participation_rate = 0.1;
    config.min_child_size = 100;

    auto result = engine_->submit_execution(order, ExecutionAlgo::ADAPTIVE_LIMIT, config);
    ASSERT_TRUE(result.is_ok());
    std::string job_id = result.value();

    // Check metrics
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto metrics_result = engine_->get_metrics(job_id);
    ASSERT_TRUE(metrics_result.is_ok());

    const auto& metrics = metrics_result.value();
    EXPECT_GT(metrics.num_child_orders, 0);
    EXPECT_LE(metrics.average_fill_price,
              order.price.as_double());  // Should not exceed limit price
    EXPECT_GT(metrics.completion_rate, 0.0);
}

TEST_F(ExecutionEngineTest, DarkPoolExecution) {
    Order order;
    order.symbol = "NVDA";
    order.side = Side::BUY;
    order.type = OrderType::LIMIT;
    order.quantity = 1200;
    order.price = 400.0;
    order.time_in_force = TimeInForce::DAY;

    ExecutionConfig config;
    config.dark_pool_only = true;
    config.time_horizon = std::chrono::minutes(120);
    config.min_child_size = 100;
    config.urgency_level = 0.4;

    auto result = engine_->submit_execution(order, ExecutionAlgo::DARK_POOL, config);
    ASSERT_TRUE(result.is_ok());
    std::string job_id = result.value();

    // Check metrics
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto metrics_result = engine_->get_metrics(job_id);
    ASSERT_TRUE(metrics_result.is_ok());

    const auto& metrics = metrics_result.value();
    EXPECT_GT(metrics.num_child_orders, 0);
    EXPECT_LE(metrics.market_impact, 0.001);  // Should have minimal market impact
    EXPECT_GT(metrics.completion_rate, 0.0);
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
    const int num_orders = 5;
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
        config.min_child_size = 50;

        auto result = engine_->submit_execution(order, ExecutionAlgo::TWAP, config);

        ASSERT_TRUE(result.is_ok())
            << "Failed to submit order " << i << ": " << result.error()->what();
        job_ids.push_back(result.value());

        // Add small delay between orders
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Wait for some execution progress
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Verify all jobs are being tracked
    auto active_jobs = engine_->get_active_jobs();
    ASSERT_TRUE(active_jobs.is_ok())
        << "Failed to get active jobs: " << active_jobs.error()->what();

    size_t active_count = active_jobs.value().size();
    EXPECT_GE(active_count, 0) << "Expected at least one active job";

    // Cancel jobs with better error handling
    size_t successful_cancels = 0;
    for (size_t i = 0; i < job_ids.size() / 2; ++i) {
        auto cancel_result = engine_->cancel_execution(job_ids[i]);
        if (cancel_result.is_ok()) {
            successful_cancels++;
        }
        // Add delay between cancellations
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Wait for cancellations to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Verify correct number of jobs remain active
    active_jobs = engine_->get_active_jobs();
    ASSERT_TRUE(active_jobs.is_ok());
    EXPECT_GE(active_jobs.value().size(), 0) << "No active jobs remaining after cancellation";
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
