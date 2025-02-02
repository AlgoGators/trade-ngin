#include <gtest/gtest.h>
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "../core/test_base.hpp"
#include "../order/test_utils.hpp"
#include <memory>
#include <chrono>

using namespace trade_ngin;
using namespace trade_ngin::testing;

class TrendFollowingStrategyTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();

        // Create database connection
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());

        // Create base strategy configuration
        strategy_config_.capital_allocation = 1000000.0;  // $1M initial capital
        strategy_config_.max_leverage = 2.0;
        strategy_config_.asset_classes = {AssetClass::FUTURES};
        strategy_config_.frequencies = {DataFrequency::DAILY};
        strategy_config_.save_signals = true;
        strategy_config_.save_positions = true;

        // Create trend following specific configuration
        trend_config_.risk_target = 0.20;        // 20% annualized vol target
        trend_config_.idm = 2.5;                 // Instrument diversification multiplier
        trend_config_.use_position_buffering = true;
        trend_config_.ema_windows = {
            {2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}
        };
        trend_config_.vol_lookback_short = 22;   // 1 month
        trend_config_.vol_lookback_long = 252;   // 1 year

        // Initialize strategy
        strategy_ = std::make_unique<TrendFollowingStrategy>(
            "TEST_TREND_1",
            strategy_config_,
            trend_config_,
            db_
        );

        ASSERT_TRUE(strategy_->initialize().is_ok());
    }

    void TearDown() override {
        strategy_.reset();
        db_.reset();
        TestBase::TearDown();
    }

    // Helper to create test market data
    std::vector<Bar> create_price_series(
        const std::string& symbol,
        const std::vector<double>& prices,
        std::chrono::system_clock::time_point start_time = std::chrono::system_clock::now()) {
        
        std::vector<Bar> bars;
        for (size_t i = 0; i < prices.size(); ++i) {
            Bar bar;
            bar.symbol = symbol;
            bar.timestamp = start_time - std::chrono::hours(24 * (prices.size() - i - 1));
            bar.open = prices[i];
            bar.high = prices[i] * 1.01;
            bar.low = prices[i] * 0.99;
            bar.close = prices[i];
            bar.volume = 10000.0;
            bars.push_back(bar);
        }
        return bars;
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    StrategyConfig strategy_config_;
    TrendFollowingConfig trend_config_;
    std::unique_ptr<TrendFollowingStrategy> strategy_;
};

TEST_F(TrendFollowingStrategyTest, Initialization) {
    EXPECT_EQ(strategy_->get_state(), StrategyState::INITIALIZED);
    
    const auto& config = strategy_->get_config();
    EXPECT_DOUBLE_EQ(config.capital_allocation, 1000000.0);
    EXPECT_DOUBLE_EQ(config.max_leverage, 2.0);
    
    const auto& positions = strategy_->get_positions();
    EXPECT_TRUE(positions.empty());
}

TEST_F(TrendFollowingStrategyTest, UpTrendSignalGeneration) {
    // Create upward trending price series
    std::vector<double> prices = {
        100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110,
        111, 112, 113, 114, 115, 116, 117, 118, 119, 120
    };
    auto bars = create_price_series("AAPL", prices);

    // Start strategy
    ASSERT_TRUE(strategy_->start().is_ok());
    
    // Process data
    ASSERT_TRUE(strategy_->on_data(bars).is_ok());

    // Verify positions
    const auto& positions = strategy_->get_positions();
    ASSERT_TRUE(positions.count("AAPL") > 0);
    const auto& pos = positions.at("AAPL");
    EXPECT_GT(pos.quantity, 0.0) << "Should have long position in uptrend";
}

TEST_F(TrendFollowingStrategyTest, DownTrendSignalGeneration) {
    // Create downward trending price series
    std::vector<double> prices = {
        120, 119, 118, 117, 116, 115, 114, 113, 112, 111, 110,
        109, 108, 107, 106, 105, 104, 103, 102, 101, 100
    };
    auto bars = create_price_series("MSFT", prices);

    // Start strategy
    ASSERT_TRUE(strategy_->start().is_ok());
    
    // Process data
    ASSERT_TRUE(strategy_->on_data(bars).is_ok());

    // Verify positions
    const auto& positions = strategy_->get_positions();
    ASSERT_TRUE(positions.count("MSFT") > 0);
    const auto& pos = positions.at("MSFT");
    EXPECT_LT(pos.quantity, 0.0) << "Should have short position in downtrend";
}

TEST_F(TrendFollowingStrategyTest, ChoppyMarketSignals) {
    // Create choppy/sideways price series
    std::vector<double> prices = {
        100, 102, 99, 103, 98, 104, 97, 105, 96, 106, 95,
        107, 94, 108, 93, 109, 92, 110, 91, 111, 90
    };
    auto bars = create_price_series("GOOG", prices);

    // Start strategy
    ASSERT_TRUE(strategy_->start().is_ok());
    
    // Process data
    ASSERT_TRUE(strategy_->on_data(bars).is_ok());

    // Verify positions
    const auto& positions = strategy_->get_positions();
    if (positions.count("GOOG") > 0) {
        const auto& pos = positions.at("GOOG");
        EXPECT_NEAR(std::abs(pos.quantity), 0.0, 1e-6) 
            << "Should have small/no position in choppy market";
    }
}

TEST_F(TrendFollowingStrategyTest, VolatilityScaling) {
    // Create two trending series with different volatilities
    std::vector<double> low_vol_prices = {
        100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110  // Low vol uptrend
    };
    std::vector<double> high_vol_prices = {
        100, 105, 110, 115, 120, 125, 130, 135, 140, 145, 150  // High vol uptrend
    };

    auto low_vol_bars = create_price_series("LOW_VOL", low_vol_prices);
    auto high_vol_bars = create_price_series("HIGH_VOL", high_vol_prices);

    // Start strategy
    ASSERT_TRUE(strategy_->start().is_ok());
    
    // Process both series
    ASSERT_TRUE(strategy_->on_data(low_vol_bars).is_ok());
    ASSERT_TRUE(strategy_->on_data(high_vol_bars).is_ok());

    // Get positions
    const auto& positions = strategy_->get_positions();
    ASSERT_TRUE(positions.count("LOW_VOL") > 0);
    ASSERT_TRUE(positions.count("HIGH_VOL") > 0);

    // High vol series should have smaller position size
    double low_vol_size = std::abs(positions.at("LOW_VOL").quantity);
    double high_vol_size = std::abs(positions.at("HIGH_VOL").quantity);
    EXPECT_GT(low_vol_size, high_vol_size) 
        << "High volatility position should be scaled down";
}

TEST_F(TrendFollowingStrategyTest, PositionBuffering) {
    // Create price series that trends then reverses slightly
    std::vector<double> prices = {
        100, 102, 104, 106, 108, 110, 112, 114, 116, 118, 120,  // Uptrend
        119, 118, 117  // Small reversal
    };
    auto bars = create_price_series("BUFF", prices);

    // Enable position buffering
    trend_config_.use_position_buffering = true;
    strategy_ = std::make_unique<TrendFollowingStrategy>(
        "TEST_TREND_2",
        strategy_config_,
        trend_config_,
        db_
    );
    ASSERT_TRUE(strategy_->initialize().is_ok());
    ASSERT_TRUE(strategy_->start().is_ok());

    // Process initial trend
    std::vector<Bar> initial_bars(bars.begin(), bars.begin() + 11);
    ASSERT_TRUE(strategy_->on_data(initial_bars).is_ok());
    
    // Get initial position
    const auto& initial_positions = strategy_->get_positions();
    ASSERT_TRUE(initial_positions.count("BUFF") > 0);
    double initial_size = initial_positions.at("BUFF").quantity;

    // Process reversal
    std::vector<Bar> reversal_bars(bars.begin() + 11, bars.end());
    ASSERT_TRUE(strategy_->on_data(reversal_bars).is_ok());
    
    // Get final position
    const auto& final_positions = strategy_->get_positions();
    ASSERT_TRUE(final_positions.count("BUFF") > 0);
    double final_size = final_positions.at("BUFF").quantity;

    // Position should not change dramatically due to buffering
    double position_change = std::abs(final_size - initial_size) / initial_size;
    EXPECT_LT(position_change, 0.2) 
        << "Position change should be limited by buffering";
}

TEST_F(TrendFollowingStrategyTest, RiskManagement) {
    // Create multiple trending series
    std::vector<double> series1 = {100, 102, 104, 106, 108, 110};  // Uptrend
    std::vector<double> series2 = {200, 204, 208, 212, 216, 220};  // Uptrend
    std::vector<double> series3 = {50, 51, 52, 53, 54, 55};        // Uptrend

    auto bars1 = create_price_series("SYM1", series1);
    auto bars2 = create_price_series("SYM2", series2);
    auto bars3 = create_price_series("SYM3", series3);

    // Start strategy
    ASSERT_TRUE(strategy_->start().is_ok());
    
    // Process all series
    ASSERT_TRUE(strategy_->on_data(bars1).is_ok());
    ASSERT_TRUE(strategy_->on_data(bars2).is_ok());
    ASSERT_TRUE(strategy_->on_data(bars3).is_ok());

    // Calculate total exposure
    const auto& positions = strategy_->get_positions();
    double total_exposure = 0.0;
    for (const auto& [symbol, pos] : positions) {
        total_exposure += std::abs(pos.quantity * pos.average_price);
    }

    // Verify leverage limits
    EXPECT_LE(total_exposure / strategy_config_.capital_allocation, 
              strategy_config_.max_leverage)
        << "Total exposure should not exceed max leverage";
}

TEST_F(TrendFollowingStrategyTest, SignalPersistence) {
    // Create price series with strong trend
    std::vector<double> prices = {
        100, 102, 104, 106, 108, 110, 112, 114, 116, 118, 120
    };
    auto bars = create_price_series("PERSIST", prices);

    // Start strategy
    ASSERT_TRUE(strategy_->start().is_ok());
    
    // Process initial data
    ASSERT_TRUE(strategy_->on_data(bars).is_ok());

    // Get initial position
    const auto& initial_positions = strategy_->get_positions();
    ASSERT_TRUE(initial_positions.count("PERSIST") > 0);
    double initial_size = initial_positions.at("PERSIST").quantity;

    // Stop and restart strategy
    ASSERT_TRUE(strategy_->stop().is_ok());
    ASSERT_TRUE(strategy_->start().is_ok());

    // Process same data again
    ASSERT_TRUE(strategy_->on_data(bars).is_ok());

    // Get new position
    const auto& final_positions = strategy_->get_positions();
    ASSERT_TRUE(final_positions.count("PERSIST") > 0);
    double final_size = final_positions.at("PERSIST").quantity;

    // Positions should be similar
    EXPECT_NEAR(final_size, initial_size, std::abs(initial_size) * 0.1)
        << "Positions should be consistent across strategy restarts";
}

TEST_F(TrendFollowingStrategyTest, MultipleTimeframeSignals) {
    // Create price series long enough for multiple EMA windows
    std::vector<double> prices;
    double price = 100.0;
    for (int i = 0; i < 300; ++i) {  // Long enough for longest EMA window
        prices.push_back(price);
        price += 0.5;  // Steady uptrend
    }
    auto bars = create_price_series("MULTI", prices);

    // Start strategy
    ASSERT_TRUE(strategy_->start().is_ok());
    
    // Process data in chunks to simulate different timeframes
    for (size_t i = 0; i < bars.size(); i += 50) {
        size_t end_idx = std::min(i + 50, bars.size());
        std::vector<Bar> chunk(bars.begin() + i, bars.begin() + end_idx);
        ASSERT_TRUE(strategy_->on_data(chunk).is_ok());

        // Check position after each chunk
        const auto& positions = strategy_->get_positions();
        if (positions.count("MULTI") > 0) {
            const auto& pos = positions.at("MULTI");
            // Position should gradually increase as more EMAs confirm trend
            if (i >= 128) {  // Longest EMA window is fully formed
                EXPECT_GT(pos.quantity, 0.0) 
                    << "Should have established position after all EMAs formed";
            }
        }
    }
}

TEST_F(TrendFollowingStrategyTest, CrossoverSignalAccuracy) {
    // Create price series with clear crossover points
    std::vector<double> prices = {
        100, 99, 98, 97, 96, 95,           // Downtrend
        94, 93, 92, 91, 90,                // Continued down
        91, 93, 95, 97, 99, 101,           // Reversal up
        103, 105, 107, 109, 111            // Continued up
    };
    auto bars = create_price_series("CROSS", prices);

    // Start strategy
    ASSERT_TRUE(strategy_->start().is_ok());
    
    // Track position changes
    std::vector<double> position_sizes;
    
    for (size_t i = 0; i < bars.size(); ++i) {
        ASSERT_TRUE(strategy_->on_data({bars[i]}).is_ok());
        
        const auto& positions = strategy_->get_positions();
        double pos_size = 0.0;
        if (positions.count("CROSS") > 0) {
            pos_size = positions.at("CROSS").quantity;
        }
        position_sizes.push_back(pos_size);
    }

    // Verify position transitions
    // Should be short during downtrend
    EXPECT_LT(position_sizes[10], 0.0) << "Should be short during downtrend";
    
    // Should reduce short position during reversal
    EXPECT_GT(position_sizes[15], position_sizes[10]) 
        << "Should reduce short position during reversal";
    
    // Should be long during uptrend
    EXPECT_GT(position_sizes.back(), 0.0) << "Should be long during uptrend";
}

TEST_F(TrendFollowingStrategyTest, StrategyMetricsTracking) {
    // Create price series with profit potential
    std::vector<double> prices = {
        100, 102, 104, 106, 108, 110,  // Uptrend
        109, 108, 107,                 // Small reversal
        110, 112, 114, 116             // Resume uptrend
    };
    auto bars = create_price_series("METRICS", prices);

    // Start strategy
    ASSERT_TRUE(strategy_->start().is_ok());
    
    // Process all data
    ASSERT_TRUE(strategy_->on_data(bars).is_ok());

    // Check strategy metrics
    const auto& metrics = strategy_->get_metrics();
    
    EXPECT_GT(metrics.total_trades, 0) << "Should have executed some trades";
    EXPECT_GT(metrics.total_pnl, 0.0) << "Should have positive P&L from uptrend";
    EXPECT_GT(metrics.sharpe_ratio, 0.0) << "Should have positive Sharpe ratio";
}

TEST_F(TrendFollowingStrategyTest, MarketRegimeAdaptation) {
    // Create price series with different volatility regimes
    std::vector<double> prices;
    double price = 100.0;
    
    // Low volatility uptrend
    for (int i = 0; i < 20; ++i) {
        prices.push_back(price);
        price += 0.5;
    }
    
    // High volatility period
    for (int i = 0; i < 20; ++i) {
        prices.push_back(price);
        price += (i % 2 == 0) ? 2.0 : -1.5;
    }
    
    // Return to low volatility
    for (int i = 0; i < 20; ++i) {
        prices.push_back(price);
        price += 0.5;
    }

    auto bars = create_price_series("REGIME", prices);

    // Start strategy and track positions
    ASSERT_TRUE(strategy_->start().is_ok());
    
    std::vector<double> position_sizes;
    
    for (size_t i = 0; i < bars.size(); ++i) {
        ASSERT_TRUE(strategy_->on_data({bars[i]}).is_ok());
        
        const auto& positions = strategy_->get_positions();
        double pos_size = 0.0;
        if (positions.count("REGIME") > 0) {
            pos_size = std::abs(positions.at("REGIME").quantity);
        }
        position_sizes.push_back(pos_size);
    }

    // Calculate average position sizes in different regimes
    double low_vol_1 = std::accumulate(position_sizes.begin(), 
                                     position_sizes.begin() + 20, 0.0) / 20;
    double high_vol = std::accumulate(position_sizes.begin() + 20,
                                    position_sizes.begin() + 40, 0.0) / 20;
    double low_vol_2 = std::accumulate(position_sizes.begin() + 40,
                                     position_sizes.end(), 0.0) / 20;

    EXPECT_GT(low_vol_1, high_vol) 
        << "Position sizes should be reduced in high volatility regime";
    EXPECT_GT(low_vol_2, high_vol)
        << "Position sizes should increase again in low volatility regime";
}

TEST_F(TrendFollowingStrategyTest, ExecutionAndFillProcessing) {
    // Create initial price series
    std::vector<double> prices = {
        100, 102, 104, 106, 108, 110, 112, 114, 116, 118, 120
    };
    auto bars = create_price_series("EXEC", prices);

    // Start strategy
    ASSERT_TRUE(strategy_->start().is_ok());
    
    // Process initial data to generate positions
    ASSERT_TRUE(strategy_->on_data(bars).is_ok());

    // Create a fill report
    ExecutionReport fill;
    fill.symbol = "EXEC";
    fill.side = Side::BUY;
    fill.filled_quantity = 100;
    fill.fill_price = 115.0;
    fill.fill_time = std::chrono::system_clock::now();
    fill.commission = 1.0;
    fill.is_partial = false;

    // Process the fill
    ASSERT_TRUE(strategy_->on_execution(fill).is_ok());

    // Verify position update
    const auto& positions = strategy_->get_positions();
    ASSERT_TRUE(positions.count("EXEC") > 0);
    const auto& pos = positions.at("EXEC");
    EXPECT_DOUBLE_EQ(pos.quantity, fill.filled_quantity);
    EXPECT_DOUBLE_EQ(pos.average_price, fill.fill_price);
}

TEST_F(TrendFollowingStrategyTest, RiskLimitEnforcement) {
    // Set tight risk limits
    RiskLimits tight_limits;
    tight_limits.max_position_size = 1000;
    tight_limits.max_notional_value = 100000.0;
    ASSERT_TRUE(strategy_->update_risk_limits(tight_limits).is_ok());

    // Create strongly trending price series
    std::vector<double> prices = {
        100, 105, 110, 115, 120, 125, 130, 135, 140, 145, 150
    };
    auto bars = create_price_series("RISK", prices);

    // Start strategy
    ASSERT_TRUE(strategy_->start().is_ok());
    
    // Process data
    ASSERT_TRUE(strategy_->on_data(bars).is_ok());

    // Verify position respects limits
    const auto& positions = strategy_->get_positions();
    if (positions.count("RISK") > 0) {
        const auto& pos = positions.at("RISK");
        EXPECT_LE(std::abs(pos.quantity), tight_limits.max_position_size)
            << "Position size should respect risk limits";
        
        double notional = std::abs(pos.quantity * pos.average_price);
        EXPECT_LE(notional, tight_limits.max_notional_value)
            << "Notional value should respect risk limits";
    }
}