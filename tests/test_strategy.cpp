#include <gtest/gtest.h>
#include "../trade_ngin/system/test_trend_strategy.hpp"
#include <vector>
#include <memory>
#include <iostream>

class StrategyTest : public ::testing::Test {
protected:
    void SetUp() override {
        strategy = std::make_unique<TrendStrategy>(1000000.0, 0.15, 0.05, 0.30, 2.0);
    }

    std::unique_ptr<TrendStrategy> strategy;
};

TEST_F(StrategyTest, TestSignalGeneration) {
    // Create sample market data
    std::vector<MarketData> data;
    for (int i = 0; i < 100; i++) {
        MarketData md;
        md.timestamp = "2023-01-" + std::to_string(i + 1);
        md.symbol = "GC.c.0";
        md.open = 1000.0 + i;
        md.high = 1005.0 + i;
        md.low = 995.0 + i;
        md.close = 1002.0 + i;
        md.volume = 1000;
        data.push_back(md);
    }

    // Generate signals
    auto signals = strategy->generateSignals(data);
    
    // Verify signal properties
    ASSERT_FALSE(signals.empty());
    ASSERT_EQ(signals.size(), data.size());
    
    // Check that signals are within expected bounds (-20 to 20)
    for (const auto& signal : signals) {
        EXPECT_GE(signal, -20.0);
        EXPECT_LE(signal, 20.0);
    }
}

TEST_F(StrategyTest, TestPositionSizing) {
    // Test position sizing with different volatility levels
    std::vector<MarketData> data;
    // Create data with increasing volatility and trend changes
    double price = 1000.0;
    for (int i = 0; i < 100; i++) {
        MarketData md;
        md.timestamp = "2023-01-" + std::to_string(i + 1);
        md.symbol = "GC.c.0";
        
        // Create a price series with both trend and volatility changes
        double trend = 0.05 * sin(i * M_PI / 25.0);  // Longer cycle for trend
        double vol = 0.02 * (1.0 + i / 100.0);       // Increasing volatility
        double noise = vol * sin(i * M_PI / 5.0);    // Higher frequency noise
        
        price *= (1.0 + trend + noise);
        
        md.open = price * (1.0 - vol/2.0);
        md.high = price * (1.0 + vol);
        md.low = price * (1.0 - vol);
        md.close = price;
        md.volume = 1000;
        data.push_back(md);
    }

    auto signals = strategy->generateSignals(data);
    
    // Debug output
    std::cout << "\nSignal values:\n";
    for (size_t i = 0; i < signals.size(); i++) {
        if (i % 10 == 0) {  // Print every 10th value
            std::cout << "Day " << i << ": Price=" << data[i].close 
                     << ", Signal=" << signals[i] << std::endl;
        }
    }
    
    // Verify position sizing responds to volatility
    double prev_pos = 0.0;
    int direction_changes = 0;
    
    for (size_t i = 1; i < signals.size(); i++) {
        if (signals[i] * prev_pos < 0) {
            direction_changes++;
            std::cout << "Direction change at day " << i 
                     << ": " << prev_pos << " -> " << signals[i] << std::endl;
        }
        prev_pos = signals[i];
    }
    
    std::cout << "Total direction changes: " << direction_changes << std::endl;
    
    // Expect some direction changes but not too frequent
    EXPECT_GT(direction_changes, 0);
    EXPECT_LT(direction_changes, signals.size() / 2);
}

TEST_F(StrategyTest, TestForecastNormalization) {
    // Test that forecasts are properly normalized
    std::vector<MarketData> data;
    // Create data with extreme moves
    for (int i = 0; i < 100; i++) {
        MarketData md;
        md.timestamp = "2023-01-" + std::to_string(i + 1);
        md.symbol = "GC.c.0";
        md.open = 1000.0;
        md.high = 1000.0 * (1 + 0.1 * sin(i));  // 10% moves
        md.low = 1000.0 * (1 - 0.1 * sin(i));
        md.close = 1000.0 * (1 + 0.05 * sin(i));
        md.volume = 1000;
        data.push_back(md);
    }

    auto signals = strategy->generateSignals(data);
    
    // Verify forecasts are normalized and capped
    for (const auto& signal : signals) {
        EXPECT_GE(signal, -20.0);
        EXPECT_LE(signal, 20.0);
    }
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 