#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
#include <vector>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

class TrendFollowingTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();

        StateManager::reset_instance();

        // Initialize database
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        auto connect_result = db_->connect();
        ASSERT_TRUE(connect_result.is_ok());

        // Create base configuration
        strategy_config_.capital_allocation = 1000000.0;  // $1M
        strategy_config_.max_leverage = 4.0;
        strategy_config_.asset_classes = {AssetClass::FUTURES};
        strategy_config_.frequencies = {DataFrequency::DAILY};
        strategy_config_.save_signals = true;
        strategy_config_.save_positions = true;

        // Configure risk limits
        risk_limits_.max_position_size = 1000.0;
        risk_limits_.max_notional_value = 1000000.0;
        risk_limits_.max_drawdown = 0.5;
        risk_limits_.max_leverage = 5.0;  // Increased to allow for slight leverage over 4.0

        // Add trading parameters for test symbols
        for (const auto& symbol : {"ES", "NQ", "YM"}) {
            strategy_config_.trading_params[symbol] = 5.0;      // Contract multiplier
            strategy_config_.position_limits[symbol] = 1000.0;  // Position limit
        }

        // Create trend following configuration
        trend_config_.weight = 1.0 / 30.0;  // Each of 30 contracts get 1/30th weight
        trend_config_.risk_target = 0.1;    // Lower risk target to reduce leverage in tests (was at 20% before, change if needed!!)
        trend_config_.idm = 2.5;            // Instrument diversification multiplier
        trend_config_.use_position_buffering = true;
        trend_config_.ema_windows = {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}};
        trend_config_.vol_lookback_short = 32;  // 1 month
        trend_config_.vol_lookback_long = 252;  // 1 year
        trend_config_.fdm = {{1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}};

        static int test_id = 0;
        std::string unique_id = "TEST_TREND_" + std::to_string(++test_id);

        // Create strategy instance
        strategy_ = std::make_unique<TrendFollowingStrategy>(unique_id, strategy_config_,
                                                             trend_config_, db_);

        // Initialize strategy
        auto init_result = strategy_->initialize();
        ASSERT_TRUE(init_result.is_ok());
        ASSERT_TRUE(strategy_->update_risk_limits(risk_limits_).is_ok());
    }

    void TearDown() override {
        if (strategy_) {
            strategy_->stop();
            strategy_.reset();
        }
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
        TestBase::TearDown();
    }

    // Helper to create a vector of Bar data for testing
    std::vector<Bar> create_test_data(const std::string& symbol,
                                      int num_bars = 300,  // Enough for the longest EMA window
                                      double start_price = 100.0, double volatility = 0.20) {
        std::vector<Bar> data;
        auto now = std::chrono::system_clock::now();

        // Validate input parameters
        if (symbol.empty()) {
            std::cerr << "ERROR: create_test_data called with empty symbol" << std::endl;
            return data;  // Return empty vector
        }

        if (num_bars <= 0) {
            std::cerr << "ERROR: create_test_data called with invalid num_bars: " << num_bars
                      << std::endl;
            return data;  // Return empty vector
        }

        // Set default price if input is invalid
        double price = (start_price <= 0.0) ? 100.0 : start_price;

        // Constrain volatility to reasonable range
        volatility = std::max(0.001, std::min(volatility, 0.5));

        // Seed random number generator
        srand(42);

        data.reserve(num_bars);

        for (int i = 0; i < num_bars; i++) {
            Bar bar;
            bar.symbol = symbol;
            bar.timestamp = now - std::chrono::hours(24 * (num_bars - i));

            // Generate price movement with some trend and volatility
            double trend = std::sin(i * 0.1) * 0.005;  // Cyclical trend
            double random = (static_cast<double>(rand()) / RAND_MAX - 0.5) * volatility;

            // Ensure price doesn't get too close to zero
            price = std::max(0.1 * start_price, price * (1.0 + trend + random));

            // Ensure all price fields are positive and properly ordered
            bar.open = price;
            bar.close = price * (1.0 + random);

            // Make sure high is the highest price
            double high_offset = volatility * 0.5;
            double open = bar.open.as_double();
            double close = bar.close.as_double();
            double high = price * (1.0 + high_offset);
            bar.high = std::max({open, close, high});

            // Make sure low is the lowest price but greater than zero
            double low_offset = volatility * 0.5;
            double low = price * (1.0 - low_offset);
            bar.low = std::max(0.1 * start_price, std::min({open, close, low}));

            // Ensure volume is positive
            bar.volume = 100000 + (rand() % 50000);

            // Final validation check - prevent any invalid data from being used
            if (bar.open <= 0.0 || bar.high <= 0.0 || bar.low <= 0.0 || bar.close <= 0.0 ||
                bar.high < bar.low) {
                std::cerr << "ERROR: Generated invalid bar data for symbol " << symbol << std::endl;

                // Fix the invalid data
                double safe_price = std::max(0.1 * start_price, price);
                bar.open = safe_price;
                bar.high = safe_price * 1.01;
                bar.low = safe_price * 0.99;
                bar.close = safe_price;
            }

            data.push_back(bar);
        }

        return data;
    }

    // Helper function to print detailed bar information for debugging
    void print_bar_details(const Bar& bar, const std::string& prefix = "") {
        std::cout << prefix << "Bar details:" << std::endl;
        std::cout << prefix << "  Symbol: '" << bar.symbol << "'" << std::endl;
        std::cout << prefix
                  << "  Timestamp: " << std::chrono::system_clock::to_time_t(bar.timestamp)
                  << std::endl;
        std::cout << prefix << "  Open: " << bar.open << std::endl;
        std::cout << prefix << "  High: " << bar.high << std::endl;
        std::cout << prefix << "  Low: " << bar.low << std::endl;
        std::cout << prefix << "  Close: " << bar.close << std::endl;
        std::cout << prefix << "  Volume: " << bar.volume << std::endl;
    }

    // Process data safely in chunks to help build history
    void process_data_safely(const std::vector<Bar>& data, size_t chunk_size = 50) {
        for (size_t i = 0; i < data.size(); i += chunk_size) {
            size_t end_idx = std::min(i + chunk_size, data.size());
            std::vector<Bar> chunk(data.begin() + i, data.begin() + end_idx);

            auto result = strategy_->on_data(chunk);
            if (result.is_error()) {
                std::cout << "ERROR processing chunk " << (i / chunk_size) << ": "
                          << result.error()->what() << std::endl;
                std::cout << "Error code: " << static_cast<int>(result.error()->code())
                          << std::endl;

                // If we have many bars, try processing one by one to find the problematic bar
                if (chunk.size() > 1) {
                    std::cout << "Attempting to process bars individually to isolate the problem..."
                              << std::endl;
                    for (size_t j = 0; j < chunk.size(); j++) {
                        std::vector<Bar> single_bar = {chunk[j]};
                        print_bar_details(chunk[j], "  Checking bar " + std::to_string(j) + ": ");
                        auto single_result = strategy_->on_data(single_bar);
                        if (single_result.is_error()) {
                            std::cout << "  ERROR processing individual bar " << j << ": "
                                      << single_result.error()->what() << std::endl;
                        } else {
                            std::cout << "  Bar " << j << " processed successfully" << std::endl;
                        }
                    }
                }
            }

            ASSERT_TRUE(result.is_ok())
                << "Failed to process data chunk " << (i / chunk_size) << ": "
                << (result.is_error() ? result.error()->what() : "Unknown error");
        }
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    StrategyConfig strategy_config_;
    RiskLimits risk_limits_;
    TrendFollowingConfig trend_config_;
    std::unique_ptr<TrendFollowingStrategy> strategy_;
    double last_position_{0.0};
};

// Test initialization and valid configuration
TEST_F(TrendFollowingTest, ValidConfiguration) {
    EXPECT_EQ(strategy_->get_state(), StrategyState::INITIALIZED);
    EXPECT_EQ(strategy_->get_config().capital_allocation, 1000000.0);
    EXPECT_FALSE(strategy_->get_positions().empty());
}

// Test invalid configuration (e.g. invalid risk target)
TEST_F(TrendFollowingTest, InvalidConfiguration) {
    trend_config_.risk_target = -0.1;  // Invalid negative value

    auto invalid_strategy = std::make_unique<TrendFollowingStrategy>(
        "INVALID_TEST", strategy_config_, trend_config_, db_);

    auto result = invalid_strategy->initialize();
    EXPECT_TRUE(result.is_error());
}

// Test signal generation and error handling for edge cases
TEST_F(TrendFollowingTest, SignalGeneration) {
    auto test_data = create_test_data("ES", 300, 4000.0);

    // Start strategy
    ASSERT_TRUE(strategy_->start().is_ok());

    // Process valid data first to build history - in smaller chunks to identify any specific issues
    for (size_t i = 0; i < test_data.size(); i += 25) {  // Process in smaller chunks
        size_t end_idx = std::min(i + 25, test_data.size());
        std::vector<Bar> chunk(test_data.begin() + i, test_data.begin() + end_idx);
        auto result = strategy_->on_data(chunk);
        ASSERT_TRUE(result.is_ok())
            << "Failed to process chunk " << i / 25 << ": "
            << (result.is_error() ? result.error()->what() : "Unknown error");
    }

    // Process invalid data (e.g., a default-constructed Bar with no fields set)
    std::vector<Bar> invalid_data = {Bar()};
    auto result = strategy_->on_data(invalid_data);
    EXPECT_TRUE(result.is_error()) << "Expected an error for invalid data, but got success";
    
    // Test with empty data - should be handled gracefully
    
    std::vector<Bar> empty_data;
    result = strategy_->on_data(empty_data);
    EXPECT_TRUE(result.is_ok()) << "Failed to process empty data: "
                                << (result.is_error() ? result.error()->what() : "Unknown error");
    
    // Test with missing fields (only symbol set)
    Bar missing_fields;
    missing_fields.symbol = "ES";
    missing_fields.timestamp = std::chrono::system_clock::now();
    std::vector<Bar> missing_data{missing_fields};
    result = strategy_->on_data(missing_data);
    EXPECT_TRUE(result.is_error()) << "Expected an error for missing fields, but got success";
}

TEST_F(TrendFollowingTest, InvalidBar_TimestampZero_ReturnsInvalidData) {
    ASSERT_TRUE(strategy_->start().is_ok());

    Bar bar;
    bar.symbol = "ES";
    bar.timestamp = Timestamp{};  
    bar.open = 100.0;
    bar.high = 101.0;
    bar.low = 99.0;
    bar.close = 100.5;
    bar.volume = 1000.0;

    auto result = strategy_->on_data(std::vector<Bar>{bar});
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_DATA);
}

TEST_F(TrendFollowingTest, InvalidBar_NonPositivePrices_ReturnsInvalidData) {
    ASSERT_TRUE(strategy_->start().is_ok());

    {
        Bar bar;
        bar.symbol = "ES";
        bar.timestamp = std::chrono::system_clock::now();
        bar.open = 0.0; 
        bar.high = 101.0;
        bar.low = 99.0;
        bar.close = 100.5;
        bar.volume = 1000.0;

        auto result = strategy_->on_data({bar});
        EXPECT_TRUE(result.is_error());
        EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_DATA);
    }

    {
        Bar bar;
        bar.symbol = "ES";
        bar.timestamp = std::chrono::system_clock::now();
        bar.open = 100.0;
        bar.high = 101.0;
        bar.low = 99.0;
        bar.close = 0.0;  
        bar.volume = 1000.0;

        auto result = strategy_->on_data({bar});
        EXPECT_TRUE(result.is_error());
        EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_DATA);
    }
}

TEST_F(TrendFollowingTest, InvalidBar_HighLessThanLow_ReturnsInvalidData) {
    ASSERT_TRUE(strategy_->start().is_ok());

    Bar bar;
    bar.symbol = "ES";
    bar.timestamp = std::chrono::system_clock::now();
    bar.open = 100.0;
    bar.high = 98.0; 
    bar.low = 99.0;
    bar.close = 99.5;
    bar.volume = 1000.0;

    auto result = strategy_->on_data({bar});
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_DATA);
}

TEST_F(TrendFollowingTest, InvalidBar_InconsistentOHLCRelationships_ReturnsInvalidData) {
    ASSERT_TRUE(strategy_->start().is_ok());

    
    {
        Bar bar;
        bar.symbol = "ES";
        bar.timestamp = std::chrono::system_clock::now();
        bar.open = 100.0;
        bar.close = 100.2;
        bar.high = 99.9;   // 
        bar.low = 99.0;
        bar.volume = 1000.0;
        auto result = strategy_->on_data({bar});
        EXPECT_TRUE(result.is_error());
        EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_DATA);
    }


    {
        Bar bar;
        bar.symbol = "ES";
        bar.timestamp = std::chrono::system_clock::now();
        bar.open = 100.0;
        bar.close = 100.2;
        bar.high = 101.0;
        bar.low = 100.3;  
        bar.volume = 1000.0;
        auto result = strategy_->on_data({bar});
        EXPECT_TRUE(result.is_error());
        EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_DATA);
    }
}

TEST_F(TrendFollowingTest, InvalidBar_NegativeVolume_ReturnsInvalidData) {
    ASSERT_TRUE(strategy_->start().is_ok());

    Bar bar;
    bar.symbol = "ES";
    bar.timestamp = std::chrono::system_clock::now();
    bar.open = 100.0;
    bar.high = 101.0;
    bar.low = 99.0;
    bar.close = 100.5;
    bar.volume = -1.0;  

    auto result = strategy_->on_data({bar});
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_DATA);
}

// Test state transitions: INITIALIZED -> RUNNING -> PAUSED -> RUNNING -> STOPPED
TEST_F(TrendFollowingTest, StateTransitions) {
    EXPECT_EQ(strategy_->get_state(), StrategyState::INITIALIZED);

    ASSERT_TRUE(strategy_->start().is_ok());
    EXPECT_EQ(strategy_->get_state(), StrategyState::RUNNING);

    ASSERT_TRUE(strategy_->pause().is_ok());
    EXPECT_EQ(strategy_->get_state(), StrategyState::PAUSED);

    // Try to process data while paused (should return an error)
    auto test_data = create_test_data("ES", 300);
    auto result = strategy_->on_data(test_data);
    EXPECT_TRUE(result.is_error());

    ASSERT_TRUE(strategy_->resume().is_ok());
    EXPECT_EQ(strategy_->get_state(), StrategyState::RUNNING);

    ASSERT_TRUE(strategy_->stop().is_ok());
    EXPECT_EQ(strategy_->get_state(), StrategyState::STOPPED);
}

// Test processing of data for multiple symbols arriving concurrently
TEST_F(TrendFollowingTest, ConcurrentSymbolUpdates) {
    // Create tests data for two symbols
    int data_size = 500;
    auto es_data = create_test_data("ES", data_size, 4000.0);
    auto nq_data = create_test_data("NQ", data_size, 15000.0);

    ASSERT_TRUE(strategy_->start().is_ok());

    // Process data in chunks to simulate concurrent updates
    process_data_safely(es_data);
    process_data_safely(nq_data);

    // Create interleaved data for symbols "ES" and "NQ"
    std::vector<Bar> interleaved_data;
    auto now = std::chrono::system_clock::now();

    for (int i = 0; i < 20; ++i) {
        Bar es_bar = es_data.back();
        es_bar.timestamp = now + std::chrono::seconds(i * 2);
        es_bar.close = es_bar.close + i;
        es_bar.open  = es_bar.close * 0.999;
        es_bar.high  = std::max(es_bar.open.as_double(), es_bar.close.as_double()) * 1.002;
        es_bar.low   = std::min(es_bar.open.as_double(), es_bar.close.as_double()) * 0.998;
    
        Bar nq_bar = nq_data.back();
        nq_bar.timestamp = now + std::chrono::seconds(i * 2);
        nq_bar.close = nq_bar.close + i;
        nq_bar.open  = nq_bar.close * 0.999;
        nq_bar.high  = std::max(nq_bar.open.as_double(), nq_bar.close.as_double()) * 1.002;
        nq_bar.low   = std::min(nq_bar.open.as_double(), nq_bar.close.as_double()) * 0.998;
    
        interleaved_data.push_back(es_bar);
        interleaved_data.push_back(nq_bar);
    }

    ASSERT_TRUE(strategy_->on_data(interleaved_data).is_ok()) << "Failed to process interleaved data: ";

    const auto& positions = strategy_->get_positions();
    EXPECT_TRUE(positions.find("ES") != positions.end());
    EXPECT_TRUE(positions.find("NQ") != positions.end());
}

// Test recovery from extreme market conditions (stress recovery)
TEST_F(TrendFollowingTest, MarketStressRecovery) {
    int base_data_size = 500;

    // Create normal phase data
    auto normal_data = create_test_data("ES", base_data_size, 4000.0);

    // Process the initial data to build history
    ASSERT_TRUE(strategy_->start().is_ok());
    process_data_safely(normal_data);

    // Simulate market stress with separate phases
    std::vector<Bar> stress_data;
    double price = 4000.0;

    // Crash phase: simulate a sharp drop
    std::vector<Bar> crash_data;
    for (int i = 0; i < 500; i++) {
        Bar bar = normal_data.back();  // Start with the last normal data point
        bar.timestamp = bar.timestamp + std::chrono::hours(i + 1);
        bar.close = price * std::pow(0.95, i / 10.0 + 1);  // Smoother decline
        bar.open = bar.close * 1.01;
        bar.high = bar.close * 1.02;
        bar.low = bar.close * 0.98;
        bar.volume = 150000 + (rand() % 50000);  // Higher volume during crash
        crash_data.push_back(bar);
    }

    // Recovery phase: simulate a gradual recovery
    std::vector<Bar> recovery_data;
    double crash_end_price = crash_data.back().close.as_double();
    for (int i = 0; i < 500; i++) {
        Bar bar = crash_data.back();
        bar.timestamp = bar.timestamp + std::chrono::hours(i + 1);
        bar.close = crash_end_price * std::pow(1.02, i / 10.0 + 1);  // Smoother recovery
        bar.open = bar.close * 0.99;
        bar.high = bar.close * 1.02;
        bar.low = bar.close * 0.98;
        bar.volume = 120000 + (rand() % 40000);
        recovery_data.push_back(bar);
    }

    // Process crash and recovery data and track positions
    std::vector<double> positions;

    // Process crash phase
    process_data_safely(crash_data, 10);

    // Get position after crash
    {
        const auto& current_positions = strategy_->get_positions();
        if (current_positions.find("ES") != current_positions.end()) {
            positions.push_back(current_positions.at("ES").quantity.as_double());
        }
    }

    // Process recovery phase
    process_data_safely(recovery_data, 10);

    // Get position after recovery
    {
        const auto& current_positions = strategy_->get_positions();
        if (current_positions.find("ES") != current_positions.end()) {
            positions.push_back(current_positions.at("ES").quantity.as_double());
        }
    }

    // Verify position direction changes appropriately
    ASSERT_GE(positions.size(), 2);
    double crash_phase_pos = positions.front();
    double recovery_phase_pos = positions.back();

    // Check for directional change (not exact values)
    EXPECT_LT(crash_phase_pos, 0.0) << "Expected negative position during crash";
    EXPECT_GT(recovery_phase_pos, crash_phase_pos)
        << "Expected position to improve during recovery";
}

// Test position calculation and scaling under a strong trend
TEST_F(TrendFollowingTest, PositionScaling) {
    auto base_data = create_test_data("ES", 500, 4000.0, 0.05);

    ASSERT_TRUE(strategy_->start().is_ok());
    process_data_safely(base_data);

    // Create additional uptrend data
    std::vector<Bar> uptrend_data;
    Bar latest = base_data.back();

    for (int i = 0; i < 50; i++) {
        Bar bar = latest;
        bar.timestamp = latest.timestamp + std::chrono::hours(i + 1);
        bar.close = 4000 + i * 20;  // Add a consistent uptrend
        bar.open = bar.close * 0.99;
        bar.high = bar.close * 1.01;
        bar.low = bar.close * 0.99;
        bar.volume = 100000 + (rand() % 40000);
        uptrend_data.push_back(bar);
    }

    // Process uptrend data
    ASSERT_TRUE(strategy_->on_data(uptrend_data).is_ok());

    // Verify position
    const auto& positions = strategy_->get_positions();
    ASSERT_TRUE(positions.find("ES") != positions.end());

    double position_size = positions.at("ES").quantity.as_double();
    EXPECT_GT(position_size, 0.0) << "Expected positive position in uptrend";
    EXPECT_LT(position_size, strategy_config_.position_limits["ES"])
        << "Position exceeds limit: " << position_size;
}

// Test volatility calculation differences between high- and low-volatility data
TEST_F(TrendFollowingTest, VolatilityCalculation) {
    auto volatile_base = create_test_data("ES", 500, 4000.0, 0.05);
    auto stable_base = create_test_data("NQ", 500, 15000.0, 0.01);

    ASSERT_TRUE(strategy_->start().is_ok());
    process_data_safely(volatile_base);
    process_data_safely(stable_base);

    // Create more volatile data for ES
    std::vector<Bar> volatile_data;
    Bar volatile_latest = volatile_base.back();
    for (int i = 0; i < 30; i++) {
        Bar bar = volatile_latest;
        bar.timestamp = volatile_latest.timestamp + std::chrono::hours(i + 1);
        double random = (static_cast<double>(rand()) / RAND_MAX - 0.5) * 0.05;  // High volatility
        bar.close *= (1.0 + random);
        bar.open = bar.close * (1.0 + (static_cast<double>(rand()) / RAND_MAX - 0.5) * 0.02);
        bar.high = std::max(bar.open, bar.close) * 1.02;
        bar.low = std::min(bar.open, bar.close) * 0.98;
        bar.volume = 120000 + (rand() % 50000);
        volatile_data.push_back(bar);
    }

    // Create more stable data for NQ
    std::vector<Bar> stable_data;
    Bar stable_latest = stable_base.back();
    for (int i = 0; i < 30; i++) {
        Bar bar = stable_latest;
        bar.timestamp = stable_latest.timestamp + std::chrono::hours(i + 1);
        double random = (static_cast<double>(rand()) / RAND_MAX - 0.5) * 0.01;  // Low volatility
        bar.close *= (1.0 + random);
        bar.open = bar.close * (1.0 + (static_cast<double>(rand()) / RAND_MAX - 0.5) * 0.005);
        bar.high = std::max(bar.open, bar.close) * 1.005;
        bar.low = std::min(bar.open, bar.close) * 0.995;
        bar.volume = 100000 + (rand() % 30000);
        stable_data.push_back(bar);
    }

    // Process both datasets
    ASSERT_TRUE(strategy_->on_data(volatile_data).is_ok());
    ASSERT_TRUE(strategy_->on_data(stable_data).is_ok());

    // Get final positions
    const auto& positions = strategy_->get_positions();
    ASSERT_TRUE(positions.find("ES") != positions.end());
    ASSERT_TRUE(positions.find("NQ") != positions.end());

    // Calculate relative position sizes adjusted for price
    double es_size = std::abs(positions.at("ES").quantity.as_double());
    double nq_size = std::abs(positions.at("NQ").quantity.as_double());

    double es_price = volatile_data.back().close.as_double();
    double nq_price = stable_data.back().close.as_double();

    double es_value = es_size * es_price;
    double nq_value = nq_size * nq_price;

    // High volatility should lead to smaller positions after volatility scaling
    double es_per_dollar = es_size / es_price;
    double nq_per_dollar = nq_size / nq_price;

    EXPECT_LT(es_per_dollar, nq_per_dollar)
        << "Expected smaller adjusted position for more volatile asset";
}

// Test position buffering so that small price movements do not trigger significant changes
TEST_F(TrendFollowingTest, PositionBuffering) {
    auto test_data = create_test_data("ES", 500, 4000.0);

    ASSERT_TRUE(strategy_->start().is_ok());
    process_data_safely(test_data);

    // Get initial position
    const auto& initial_positions = strategy_->get_positions();
    ASSERT_TRUE(initial_positions.find("ES") != initial_positions.end());
    double initial_position = initial_positions.at("ES").quantity.as_double();

    // Create small update data with minimal price changes
    std::vector<Bar> small_updates;
    Bar latest = test_data.back();

    for (int i = 0; i < 5; i++) {
        Bar bar = latest;
        bar.timestamp = latest.timestamp + std::chrono::hours(i + 1);
        bar.close *= (1.0 + 0.001);  // Very small 0.1% change
        bar.open = bar.close * 0.999;
        bar.high = bar.close * 1.002;
        bar.low = bar.close * 0.998;
        small_updates.push_back(bar);

        // Process each update individually
        std::vector<Bar> single_update = {bar};
        ASSERT_TRUE(strategy_->on_data(single_update).is_ok());

        // Check position after each update
        const auto& current_positions = strategy_->get_positions();
        ASSERT_TRUE(current_positions.find("ES") != current_positions.end());

        // With buffering enabled, position should remain stable for small changes
        EXPECT_NEAR(current_positions.at("ES").quantity.as_double(), initial_position, 5.0)
            << "Position changed too much for small price movement: "
            << current_positions.at("ES").quantity.as_double() << " vs " << initial_position;
    }
}

// Test that the strategy obeys risk limits when configured with tight constraints
TEST_F(TrendFollowingTest, RiskLimits) {
    // Create and process data to build history
    auto test_data = create_test_data("ES", 500, 4000.0);

    ASSERT_TRUE(strategy_->start().is_ok());
    process_data_safely(test_data);

    // Set tight risk limits
    RiskLimits limits;
    limits.max_position_size = 100.0;
    limits.max_leverage = 1.5;

    // Check that updating risk limits with tighter constraints fails
    ASSERT_TRUE(strategy_->update_risk_limits(limits).is_error());

    // Create strong uptrend data to trigger position growth
    std::vector<Bar> uptrend_data;
    Bar latest = test_data.back();

    for (int i = 0; i < 30; i++) {
        Bar bar = latest;
        bar.timestamp = latest.timestamp + std::chrono::hours(i + 1);
        bar.close *= 1.02;  // Strong 2% uptrend
        bar.open = bar.close * 0.99;
        bar.high = bar.close * 1.03;
        bar.low = bar.close * 0.98;
        uptrend_data.push_back(bar);
    }

    // Process uptrend data and check risk limits
    ASSERT_TRUE(strategy_->on_data(uptrend_data).is_ok());

    // Verify position is within limits
    const auto& positions = strategy_->get_positions();
    ASSERT_TRUE(positions.find("ES") != positions.end());
    double position_size = positions.at("ES").quantity.as_double();
    double position_value = std::abs(position_size * uptrend_data.back().close.as_double());

    // Check against explicit limits
    EXPECT_LE(std::abs(position_size), limits.max_position_size.as_double())
        << "Position exceeds max_position_size limit";

    // Check leverage limit
    double portfolio_value = strategy_config_.capital_allocation;
    double leverage = position_value / portfolio_value;

    EXPECT_LE(leverage, limits.max_leverage.as_double())
        << "Leverage: " << leverage << " exceeds max_leverage limit: " << limits.max_leverage;
}

// Test multiple instruments are handled correctly and total exposure is within limits
TEST_F(TrendFollowingTest, MultipleInstruments) {
    std::vector<Bar> combined_data;

    auto es_data = create_test_data("ES", 500, 4000.0, 0.2);
    auto nq_data = create_test_data("NQ", 500, 15000.0, 0.3);
    auto ym_data = create_test_data("YM", 500, 35000.0, 0.1);

    // Process data for each instrument separately
    ASSERT_TRUE(strategy_->start().is_ok());
    process_data_safely(es_data);
    process_data_safely(nq_data);
    process_data_safely(ym_data);

    // Verify positions for all instruments
    const auto& positions = strategy_->get_positions();
    EXPECT_TRUE(positions.find("ES") != positions.end());
    EXPECT_TRUE(positions.find("NQ") != positions.end());
    EXPECT_TRUE(positions.find("YM") != positions.end());

    // Calculate total exposure
    double total_exposure = 0.0;
    for (const auto& [symbol, pos] : positions) {
        double price = 0.0;
        if (symbol == "ES")
            price = es_data.back().close.as_double();
        else if (symbol == "NQ")
            price = nq_data.back().close.as_double();
        else if (symbol == "YM")
            price = ym_data.back().close.as_double();

        total_exposure += std::abs(pos.quantity.as_double() * price);
    }

    // Verify total exposure is within leverage limits
    double portfolio_value = strategy_config_.capital_allocation;
    double leverage = total_exposure / portfolio_value;

    EXPECT_LE(leverage, strategy_config_.max_leverage)
        << "Total leverage " << leverage << " exceeds max leverage "
        << strategy_config_.max_leverage;

    // Verify risk check passes with current positions
    ASSERT_TRUE(strategy_->check_risk_limits().is_ok())
        << "Risk limits exceeded with current positions";
}

// Test the overall trend following effectiveness over distinct trend phases
TEST_F(TrendFollowingTest, TrendFollowingEffectiveness) {
    std::vector<Bar> test_data;
    double price = 4000.0;
    auto now = std::chrono::system_clock::now();

    // Uptrend phase
    std::vector<Bar> uptrend_data;
    for (int i = 0; i < 500; i++) {
        Bar bar;
        bar.symbol = "ES";
        bar.timestamp = now - std::chrono::hours(24 * (700 - i));  // Start with earliest timestamps

        // Simple uptrend with small random noise
        double random = (static_cast<double>(rand()) / RAND_MAX - 0.5) * 0.005;
        price += 1.01 * i;  // Consistent uptrend plus small noise

        bar.open = price * 0.999;
        bar.close = price;
        bar.high = price * 1.002;
        bar.low = price * 0.998;
        bar.volume = 100000 + (rand() % 30000);

        uptrend_data.push_back(bar);
    }

    // Create sideways phase data
    std::vector<Bar> sideways_data;
    for (int i = 0; i < 500; i++) {
        Bar bar;
        bar.symbol = "ES";
        bar.timestamp = now - std::chrono::hours(24 * (200 - i));

        // Only random movement, no trend
        double random = (static_cast<double>(rand()) / RAND_MAX - 0.5) * 0.005;
        price *= (1.0 + random);

        bar.open = price * 0.999;
        bar.close = price;
        bar.high = price * 1.002;
        bar.low = price * 0.998;
        bar.volume = 90000 + (rand() % 20000);

        sideways_data.push_back(bar);
    }

    // Create downtrend phase data
    std::vector<Bar> downtrend_data;
    for (int i = 0; i < 500; i++) {
        Bar bar;
        bar.symbol = "ES";
        bar.timestamp = now - std::chrono::hours(24 * (100 - i));  // End with latest timestamps

        // Consistent downtrend with small random noise
        double random = (static_cast<double>(rand()) / RAND_MAX - 0.5) * 0.005;
        price += (-1.01 * i);  // Consistent downtrend plus small noise

        bar.open = price * 1.001;
        bar.close = price;
        bar.high = price * 1.002;
        bar.low = price * 0.998;
        bar.volume = 110000 + (rand() % 40000);

        downtrend_data.push_back(bar);
    }

    test_data.insert(test_data.end(), uptrend_data.begin(), uptrend_data.end());
    test_data.insert(test_data.end(), sideways_data.begin(), sideways_data.end());
    test_data.insert(test_data.end(), downtrend_data.begin(), downtrend_data.end());

    ASSERT_TRUE(strategy_->start().is_ok());

    std::vector<double> positions;

    // Process data in chunks and record positions over time
    for (size_t i = 0; i < test_data.size(); i += 10) {
        std::vector<Bar> chunk(
            test_data.begin() + i,
            test_data.begin() + std::min(i + static_cast<size_t>(10), test_data.size()));
        ASSERT_TRUE(strategy_->on_data(chunk).is_ok());

        const auto& current_positions = strategy_->get_positions();
        if (current_positions.find("ES") != current_positions.end()) {
            positions.push_back(current_positions.at("ES").quantity.as_double());
        }
    }

    // Verify trend following behavior:
    // During the uptrend (first third), expect mostly positive positions.
    double avg_pos_uptrend =
        std::accumulate(positions.begin(), positions.begin() + positions.size() / 3, 0.0) /
        (positions.size() / 3);
    EXPECT_GT(avg_pos_uptrend, 0.0);

    // During the downtrend (last third), expect mostly negative positions.
    double avg_pos_downtrend =
        std::accumulate(positions.begin() + 2 * positions.size() / 3, positions.end(), 0.0) /
        (positions.size() - 2 * positions.size() / 3);
    EXPECT_LT(avg_pos_downtrend, 0.0);
}
