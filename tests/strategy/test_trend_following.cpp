#include <gtest/gtest.h>
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include <memory>
#include <vector>
#include <cmath>
#include <numeric>
#include <algorithm>

using namespace trade_ngin;
using namespace trade_ngin::testing;

class TrendFollowingTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        
        // Initialize database
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        auto connect_result = db_->connect();
        ASSERT_TRUE(connect_result.is_ok());

        // Create base configuration
        strategy_config_.capital_allocation = 1000000.0;  // $1M
        strategy_config_.max_leverage = 2.0;
        strategy_config_.asset_classes = {AssetClass::FUTURES};
        strategy_config_.frequencies = {DataFrequency::DAILY};
        strategy_config_.save_signals = true;
        strategy_config_.save_positions = true;

        // Add trading parameters for test symbols
        for (const auto& symbol : {"ES", "NQ", "YM"}) {
            strategy_config_.trading_params[symbol] = 50.0;  // Contract multiplier
            strategy_config_.position_limits[symbol] = 100.0;  // Position limit
        }

        // Create trend following configuration
        trend_config_.risk_target = 0.2;        // 20% annualized volatility target
        trend_config_.idm = 2.5;                // Instrument diversification multiplier
        trend_config_.use_position_buffering = true;
        trend_config_.ema_windows = {
            {2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}
        };
        trend_config_.vol_lookback_short = 22;   // 1 month
        trend_config_.vol_lookback_long = 252;   // 1 year
        trend_config_.fdm = {
            {1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}
        };
        
        // Create strategy instance
        strategy_ = std::make_unique<TrendFollowingStrategy>(
            "TEST_TREND",
            strategy_config_,
            trend_config_,
            db_
        );

        // Initialize strategy
        auto init_result = strategy_->initialize();
        ASSERT_TRUE(init_result.is_ok());
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
    std::vector<Bar> create_test_data(
        const std::string& symbol,
        int num_bars = 300,  // Enough for the longest EMA window
        double start_price = 100.0,
        double volatility = 0.02) {
        
        std::vector<Bar> data;
        auto now = std::chrono::system_clock::now();
        double price = start_price;

        if (start_price <= 0.0) {
            start_price = 100.0;
        }

        volatility = std::max(0.001, std::min(volatility, 0.1));
        
        for (int i = 0; i < num_bars; i++) {
            Bar bar;
            bar.symbol = symbol;
            bar.timestamp = now - std::chrono::hours(24 * (num_bars - i));
            
            // Generate price movement with some trend and volatility
            double trend = std::sin(i * 0.1) * 0.005;  // Cyclical trend
            double random = (static_cast<double>(rand()) / RAND_MAX - 0.5) * volatility;
            
            // Ensure price doesn't get too close to zero
            price = std::max(0.1 * start_price, price * (1.0 + trend + random));
            
            bar.open = price;
            bar.high = price * (1.0 + volatility * 0.5);
            bar.low = price * (1.0 - volatility * 0.5);
            bar.close = price * (1.0 + random);
            bar.volume = 100000 + (rand() % 50000);
            
            data.push_back(bar);
        }
        
        return data;
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    StrategyConfig strategy_config_;
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
        "INVALID_TEST",
        strategy_config_,
        trend_config_,
        db_
    );
    
    auto result = invalid_strategy->initialize();
    EXPECT_TRUE(result.is_error());
}

// Test signal generation and error handling for edge cases
TEST_F(TrendFollowingTest, SignalGeneration) {
    auto test_data = create_test_data("ES", 300, 4000.0);
    
    // Start strategy
    ASSERT_TRUE(strategy_->start().is_ok());
    
    // Process invalid data (e.g., a default-constructed Bar with no fields set)
    std::vector<Bar> invalid_data = { Bar() };
    auto result = strategy_->on_data(invalid_data);
    EXPECT_TRUE(result.is_error());
    
    // Test with empty data – should be handled gracefully
    std::vector<Bar> empty_data;
    result = strategy_->on_data(empty_data);
    EXPECT_TRUE(result.is_ok());
    
    // Test with missing fields (only symbol set)
    Bar missing_fields;
    missing_fields.symbol = "ES";
    std::vector<Bar> missing_data{missing_fields};
    result = strategy_->on_data(missing_data);
    EXPECT_TRUE(result.is_error());
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
    // Create interleaved data for symbols "ES" and "NQ"
    std::vector<Bar> interleaved_data;
    auto now = std::chrono::system_clock::now();
    
    for (int i = 0; i < 300; ++i) {
        Bar es_bar;
        es_bar.symbol = "ES";
        es_bar.timestamp = now + std::chrono::seconds(i * 2);
        es_bar.close = 4000.0 + i;
        
        Bar nq_bar;
        nq_bar.symbol = "NQ";
        nq_bar.timestamp = now + std::chrono::seconds(i * 2 + 1);
        nq_bar.close = 15000.0 + i;
        
        interleaved_data.push_back(es_bar);
        interleaved_data.push_back(nq_bar);
    }
    
    ASSERT_TRUE(strategy_->start().is_ok());
    ASSERT_TRUE(strategy_->on_data(interleaved_data).is_ok()) <<
        "Failed to process interleaved data: ";
    
    const auto& positions = strategy_->get_positions();
    EXPECT_TRUE(positions.find("ES") != positions.end());
    EXPECT_TRUE(positions.find("NQ") != positions.end());
}

// Test parameter sensitivity by varying the IDM value
TEST_F(TrendFollowingTest, ParameterSensitivity) {
    auto test_data = create_test_data("ES", 300, 4000.0);
    
    // Test with different IDM values and record resulting position sizes
    std::vector<double> idm_values = {1.0, 2.5, 5.0};
    std::vector<double> position_sizes;
    
    for (double idm : idm_values) {
        trend_config_.idm = idm;
        auto test_strategy = std::make_unique<TrendFollowingStrategy>(
            "TEST_" + std::to_string(idm),
            strategy_config_,
            trend_config_,
            db_
        );
        
        ASSERT_TRUE(test_strategy->initialize().is_ok());
        ASSERT_TRUE(test_strategy->start().is_ok());
        ASSERT_TRUE(test_strategy->on_data(test_data).is_ok());
        
        const auto& positions = test_strategy->get_positions();
        ASSERT_TRUE(positions.find("ES") != positions.end());
        position_sizes.push_back(std::abs(positions.at("ES").quantity));
    }
    
    // Expect that a higher IDM leads to larger positions
    for (size_t i = 1; i < position_sizes.size(); ++i) {
        EXPECT_GT(position_sizes[i], position_sizes[i - 1]);
    }
}

// Test recovery from extreme market conditions (stress recovery)
TEST_F(TrendFollowingTest, MarketStressRecovery) {
    std::vector<Bar> stress_data;
    double price = 4000.0;
    
    // Normal phase data
    auto normal_data = create_test_data("ES", 300, price);
    stress_data.insert(stress_data.end(), normal_data.begin(), normal_data.end());
    
    // Crash phase: simulate a sharp drop by reducing the close by 5% per bar
    auto crash_data = create_test_data("ES", 50, price);
    for (auto& bar : crash_data) {
        bar.close *= 0.95;
        price = bar.close;
    }
    stress_data.insert(stress_data.end(), crash_data.begin(), crash_data.end());
    
    // Recovery phase: simulate a gradual recovery by increasing the close by 2% per bar
    auto recovery_data = create_test_data("ES", 100, price);
    for (auto& bar : recovery_data) {
        bar.close *= 1.02;
    }
    stress_data.insert(stress_data.end(), recovery_data.begin(), recovery_data.end());
    
    ASSERT_TRUE(strategy_->start().is_ok());
    
    // Process data in chunks to observe behavior
    std::vector<double> positions;
    for (size_t i = 0; i < stress_data.size(); i += 10) {
        std::vector<Bar> chunk(stress_data.begin() + i,
                                stress_data.begin() + std::min(i + static_cast<size_t>(10), stress_data.size()));
        ASSERT_TRUE(strategy_->on_data(chunk).is_ok());
        
        const auto& current_positions = strategy_->get_positions();
        if (current_positions.find("ES") != current_positions.end()) {
            positions.push_back(current_positions.at("ES").quantity);
        }
        
        // Verify that risk limits are respected even under stress
        ASSERT_TRUE(strategy_->check_risk_limits().is_ok());
    }
    
    // During the crash phase we expect (on average) negative positions
    size_t crash_start = positions.size() / 4;
    size_t crash_end = positions.size() / 2;
    double crash_phase_pos = std::accumulate(positions.begin() + crash_start,
                                             positions.begin() + crash_end, 0.0) / (crash_end - crash_start);
    EXPECT_LT(crash_phase_pos, 0.0);
    
    // During the recovery phase we expect (on average) positive positions
    size_t recovery_start = (3 * positions.size()) / 4;
    double recovery_phase_pos = std::accumulate(positions.begin() + recovery_start,
                                                positions.end(), 0.0) / (positions.size() - recovery_start);
    EXPECT_GT(recovery_phase_pos, 0.0);
}

// Test position calculation and scaling under a strong trend
TEST_F(TrendFollowingTest, PositionScaling) {
    auto test_data = create_test_data("ES", 300, 4000.0, 0.01);
    for (auto& bar : test_data) {
        bar.close *= 1.01;  // Add a consistent uptrend
    }
    
    ASSERT_TRUE(strategy_->start().is_ok());
    ASSERT_TRUE(strategy_->on_data(test_data).is_ok());
    
    const auto& positions = strategy_->get_positions();
    ASSERT_TRUE(positions.find("ES") != positions.end());
    
    double position_size = std::abs(positions.at("ES").quantity);
    EXPECT_GT(position_size, 0.0);
    EXPECT_LT(position_size, strategy_config_.position_limits["ES"]);
}

// Test volatility calculation differences between high- and low-volatility data
TEST_F(TrendFollowingTest, VolatilityCalculation) {
    auto volatile_data = create_test_data("ES", 300, 4000.0, 0.05);
    auto stable_data = create_test_data("NQ", 300, 15000.0, 0.01);
    
    ASSERT_TRUE(strategy_->start().is_ok());
    
    auto result1 = strategy_->on_data(volatile_data);
    auto result2 = strategy_->on_data(stable_data);
    ASSERT_TRUE(result1.is_ok() && result2.is_ok());
    
    const auto& positions = strategy_->get_positions();
    
    // High volatility should lead to smaller positions (after scaling for contract values)
    double es_size = std::abs(positions.at("ES").quantity);
    double nq_size = std::abs(positions.at("NQ").quantity);
    
    double es_value = es_size * 4000.0;
    double nq_value = nq_size * 15000.0;
    
    EXPECT_LT(es_value, nq_value);
}

// Test position buffering so that small price movements do not trigger significant changes
TEST_F(TrendFollowingTest, PositionBuffering) {
    auto test_data = create_test_data("ES", 300, 4000.0);
    
    ASSERT_TRUE(strategy_->start().is_ok());
    
    // Process data several times with only small changes in price
    for (int i = 0; i < 5; ++i) {
        test_data.back().close *= (1.0 + 0.001);  // 0.1% change
        ASSERT_TRUE(strategy_->on_data(test_data).is_ok());
        
        const auto& positions = strategy_->get_positions();
        if (i == 0) {
            last_position_ = positions.at("ES").quantity;
        } else {
            // For very small price movements, the position should remain nearly the same
            EXPECT_NEAR(positions.at("ES").quantity, last_position_, 1.0);
        }
    }
}

// Test that the strategy obeys risk limits when configured with tight constraints
TEST_F(TrendFollowingTest, RiskLimits) {
    RiskLimits limits;
    limits.max_position_size = 10.0;
    limits.max_leverage = 1.5;
    ASSERT_TRUE(strategy_->update_risk_limits(limits).is_ok());
    
    auto test_data = create_test_data("ES", 300, 4000.0);
    for (auto& bar : test_data) {
        bar.close *= 1.02;  // Strong uptrend
    }
    
    ASSERT_TRUE(strategy_->start().is_ok());
    ASSERT_TRUE(strategy_->on_data(test_data).is_ok());
    
    const auto& positions = strategy_->get_positions();
    ASSERT_TRUE(positions.find("ES") != positions.end());
    
    double position_value = std::abs(positions.at("ES").quantity * 4000.0);
    EXPECT_LE(position_value, strategy_config_.capital_allocation * limits.max_leverage);
}

// Test multiple instruments are handled correctly and total exposure is within limits
TEST_F(TrendFollowingTest, MultipleInstruments) {
    std::vector<Bar> combined_data;
    
    auto es_data = create_test_data("ES", 300, 4000.0);
    auto nq_data = create_test_data("NQ", 300, 15000.0);
    auto ym_data = create_test_data("YM", 300, 35000.0);
    
    combined_data.insert(combined_data.end(), es_data.begin(), es_data.end());
    combined_data.insert(combined_data.end(), nq_data.begin(), nq_data.end());
    combined_data.insert(combined_data.end(), ym_data.begin(), ym_data.end());
    
    ASSERT_TRUE(strategy_->start().is_ok());
    ASSERT_TRUE(strategy_->on_data(combined_data).is_ok());
    
    const auto& positions = strategy_->get_positions();
    EXPECT_TRUE(positions.find("ES") != positions.end());
    EXPECT_TRUE(positions.find("NQ") != positions.end());
    EXPECT_TRUE(positions.find("YM") != positions.end());
    
    double total_exposure = 0.0;
    for (const auto& [symbol, pos] : positions) {
        total_exposure += std::abs(pos.quantity * pos.average_price);
    }
    
    EXPECT_LE(total_exposure, strategy_config_.capital_allocation * strategy_config_.max_leverage);
}

// Test the overall trend following effectiveness over distinct trend phases
TEST_F(TrendFollowingTest, TrendFollowingEffectiveness) {
    std::vector<Bar> test_data;
    double price = 4000.0;
    
    // Uptrend phase
    auto uptrend_data = create_test_data("ES", 100, price);
    for (auto& bar : uptrend_data) {
        bar.close *= 1.01;  // Consistent uptrend
        price = bar.close;
    }
    
    // Sideways phase
    auto sideways_data = create_test_data("ES", 100, price, 0.005);
    
    // Downtrend phase
    auto downtrend_data = create_test_data("ES", 100, price);
    for (auto& bar : downtrend_data) {
        bar.close *= 0.99;  // Consistent downtrend
    }
    
    test_data.insert(test_data.end(), uptrend_data.begin(), uptrend_data.end());
    test_data.insert(test_data.end(), sideways_data.begin(), sideways_data.end());
    test_data.insert(test_data.end(), downtrend_data.begin(), downtrend_data.end());
    
    ASSERT_TRUE(strategy_->start().is_ok());
    
    std::vector<double> positions;
    
    // Process data in chunks and record positions over time
    for (size_t i = 0; i < test_data.size(); i += 10) {
        std::vector<Bar> chunk(test_data.begin() + i, 
                                test_data.begin() + std::min(i + static_cast<size_t>(10), test_data.size()));
        ASSERT_TRUE(strategy_->on_data(chunk).is_ok());
        
        const auto& current_positions = strategy_->get_positions();
        if (current_positions.find("ES") != current_positions.end()) {
            positions.push_back(current_positions.at("ES").quantity);
        }
    }
    
    // Verify trend following behavior:
    // During the uptrend (first third), expect mostly positive positions.
    double avg_pos_uptrend = std::accumulate(positions.begin(), 
                                             positions.begin() + positions.size()/3, 0.0) 
                             / (positions.size()/3);
    EXPECT_GT(avg_pos_uptrend, 0.0);
    
    // During the downtrend (last third), expect mostly negative positions.
    double avg_pos_downtrend = std::accumulate(positions.begin() + 2*positions.size()/3, 
                                               positions.end(), 0.0) 
                               / (positions.size() - 2*positions.size()/3);
    EXPECT_LT(avg_pos_downtrend, 0.0);
}
