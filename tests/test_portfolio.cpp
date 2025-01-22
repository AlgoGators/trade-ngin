#include <gtest/gtest.h>
#include "../trade_ngin/system/portfolio.hpp"
#include "../trade_ngin/system/market_data.hpp"
#include <memory>
#include <vector>

class PortfolioTest : public ::testing::Test {
protected:
    std::unique_ptr<Portfolio> portfolio;
    std::vector<MarketData> market_data;

    void SetUp() override {
        portfolio = std::make_unique<Portfolio>(100000.0); // Start with 100k capital
        
        // Setup sample market data
        MarketData data;
        data.symbol = "GC.c.0";
        data.timestamp = "2024-01-22";
        data.open = 2020.0;
        data.high = 2025.0;
        data.low = 2015.0;
        data.close = 2022.0;
        data.volume = 1000;
        market_data.push_back(data);
    }
};

TEST_F(PortfolioTest, TestInitialization) {
    EXPECT_DOUBLE_EQ(portfolio->getCurrentCapital(), 100000.0);
    EXPECT_DOUBLE_EQ(portfolio->getCurrentPosition(), 0.0);
}

TEST_F(PortfolioTest, TestSignalProcessing) {
    // Process a buy signal
    portfolio->processSignal(market_data[0], 1.0); // Buy signal
    EXPECT_GT(portfolio->getCurrentPosition(), 0.0);
    
    // Process a sell signal
    portfolio->processSignal(market_data[0], -1.0); // Sell signal
    EXPECT_LT(portfolio->getCurrentPosition(), 0.0);
}

TEST_F(PortfolioTest, TestPerformanceMetrics) {
    // Process a series of trades to generate performance metrics
    MarketData data = market_data[0];
    
    // Buy at 2020
    data.close = 2020.0;
    portfolio->processSignal(data, 1.0);
    
    // Price moves up to 2040
    data.close = 2040.0;
    portfolio->processSignal(data, 0.0); // Close position
    
    // Verify metrics
    EXPECT_GT(portfolio->getTotalReturn(), 0.0);
    EXPECT_GT(portfolio->getWinRate(), 0.0);
    EXPECT_GT(portfolio->getProfitFactor(), 1.0);
    EXPECT_GE(portfolio->getMaxDrawdown(), 0.0);
    EXPECT_NE(portfolio->getSharpeRatio(), 0.0);
    EXPECT_GT(portfolio->getAnnualizedReturn(), 0.0);
    EXPECT_GT(portfolio->getTotalTrades(), 0);
    EXPECT_GT(portfolio->getWinningTrades(), 0);
}

TEST_F(PortfolioTest, TestPositionTracking) {
    // Test adding and updating positions
    portfolio->updatePosition("GC.c.0", 1.0, 1900.0);
    portfolio->updatePosition("CL.c.0", -2.0, 75.0);
    
    auto positions = portfolio->getPositions();
    ASSERT_EQ(positions.size(), 2);
    
    // Check position values
    EXPECT_EQ(positions["GC.c.0"].size, 1.0);
    EXPECT_EQ(positions["GC.c.0"].price, 1900.0);
    EXPECT_EQ(positions["CL.c.0"].size, -2.0);
    EXPECT_EQ(positions["CL.c.0"].price, 75.0);
    
    // Test position modification
    portfolio->updatePosition("GC.c.0", 0.5, 1920.0);
    positions = portfolio->getPositions();
    EXPECT_EQ(positions["GC.c.0"].size, 0.5);
    EXPECT_EQ(positions["GC.c.0"].price, 1920.0);
}

TEST_F(PortfolioTest, TestRiskManagement) {
    // Test position limits
    portfolio->setPositionLimit("GC.c.0", 2.0);
    
    // Try to exceed position limit
    EXPECT_THROW(portfolio->updatePosition("GC.c.0", 2.5, 1900.0), std::runtime_error);
    
    // Test valid position within limit
    EXPECT_NO_THROW(portfolio->updatePosition("GC.c.0", 1.5, 1900.0));
    
    // Test portfolio-wide risk limits
    portfolio->setMaxDrawdown(0.1);  // 10% max drawdown
    
    // Simulate a large loss that would exceed drawdown limit
    EXPECT_THROW(portfolio->updatePosition("GC.c.0", 1.5, 1700.0), std::runtime_error);
}

TEST_F(PortfolioTest, TestTradeStats) {
    // Initialize trade history
    portfolio->recordTrade("GC.c.0", 1.0, 1900.0, true);   // Buy
    portfolio->recordTrade("GC.c.0", -1.0, 1920.0, false); // Sell
    portfolio->recordTrade("CL.c.0", -1.0, 75.0, true);    // Short
    portfolio->recordTrade("CL.c.0", 1.0, 73.0, false);    // Cover
    
    // Test trade statistics
    auto stats = portfolio->getTradeStats();
    
    EXPECT_EQ(stats.total_trades, 4);
    EXPECT_EQ(stats.winning_trades, 2);  // Both trades were profitable
    EXPECT_NEAR(stats.win_rate, 1.0, 0.001);
    EXPECT_GT(stats.avg_profit_per_trade, 0.0);
    EXPECT_GT(stats.sharpe_ratio, 0.0);
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 