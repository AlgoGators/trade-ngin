#include <gtest/gtest.h>
#include "trade_ngin/risk/risk_manager.hpp"
#include "../core/test_base.hpp"
#include <memory>
#include <vector>
#include <chrono>
#include <algorithm>

using namespace trade_ngin;
using namespace trade_ngin::testing;

class RiskManagerTest : public ::TestBase {
protected:
    void SetUp() override {
        RiskConfig config;
        config.var_limit = 0.15;            // 15% VaR limit
        config.jump_risk_limit = 0.10;      // 10% jump risk threshold
        config.max_correlation = 0.7;        // 70% correlation limit
        config.max_gross_leverage = 4.0;     // 4x gross leverage
        config.max_net_leverage = 2.0;       // 2x net leverage
        config.capital = 1000000.0;         // $1M capital
        config.confidence_level = 0.99;      // 99% confidence
        config.lookback_period = 252;        // 1 year lookback

        risk_manager_ = std::make_unique<RiskManager>(config);

        // Setup some basic market data that all tests can use
        auto market_data = create_test_market_data({
            {"AAPL", {100, 101, 102, 103, 104}},
            {"MSFT", {200, 202, 204, 206, 208}},
            {"GOOG", {2500, 2520, 2540, 2560, 2580}}
        });
        ASSERT_TRUE(risk_manager_->update_market_data(market_data).is_ok());
    }

    std::unordered_map<std::string, Position> create_test_positions(
        const std::vector<std::tuple<std::string, double, double>>& position_data) {
        
        std::unordered_map<std::string, Position> positions;
        auto now = std::chrono::system_clock::now();

        for (const auto& [symbol, quantity, price] : position_data) {
            Position pos{
                symbol,
                quantity,
                price,
                0.0,  // unrealized P&L
                0.0,  // realized P&L
                now
            };
            positions[symbol] = pos;
        }
        return positions;
    }

    std::vector<Bar> create_test_market_data(
        const std::vector<std::pair<std::string, std::vector<double>>>& price_series) {
        
        std::vector<Bar> bars;
        auto now = std::chrono::system_clock::now();

        for (const auto& [symbol, prices] : price_series) {
            for (size_t i = 0; i < prices.size(); ++i) {
                Bar bar;
                bar.symbol = symbol;
                bar.timestamp = now - std::chrono::hours(24 * (prices.size() - i - 1));
                bar.open = prices[i];
                bar.high = prices[i] * 1.01;
                bar.low = prices[i] * 0.99;
                bar.close = prices[i];
                bar.volume = 10000.0;
                bars.push_back(bar);
            }
        }

        // Sort bars by timestamp
        std::sort(bars.begin(), bars.end(), 
            [](const Bar& a, const Bar& b) { return a.timestamp < b.timestamp; });
        return bars;
    }

    std::unique_ptr<RiskManager> risk_manager_;
};

TEST_F(RiskManagerTest, InitializationAndConfig) {
    const auto& config = risk_manager_->get_config();
    EXPECT_DOUBLE_EQ(config.var_limit, 0.15);
    EXPECT_DOUBLE_EQ(config.capital, 1000000.0);
    EXPECT_DOUBLE_EQ(config.confidence_level, 0.99);
}

TEST_F(RiskManagerTest, EmptyPortfolio) {
    std::unordered_map<std::string, Position> empty_positions;
    auto result = risk_manager_->process_positions(empty_positions);
    
    ASSERT_TRUE(result.is_ok());
    const auto& risk_result = result.value();
    EXPECT_FALSE(risk_result.risk_exceeded);
    EXPECT_DOUBLE_EQ(risk_result.recommended_scale, 1.0);
}

TEST_F(RiskManagerTest, GrossLeverageExceeded) {
    // Create positions that exceed gross leverage (4x)
    auto positions = create_test_positions({
        {"AAPL", 10000, 104.0},   // $1.04M
        {"MSFT", 5000, 208.0},    // $1.04M
        {"GOOG", 1000, 2580.0}    // $2.58M
    }); // Total: $4.66M on $1M capital = 4.66x leverage

    auto result = risk_manager_->process_positions(positions);
    ASSERT_TRUE(result.is_ok());
    const auto& risk_result = result.value();

    EXPECT_TRUE(risk_result.risk_exceeded);
    EXPECT_GT(risk_result.gross_leverage, 4.0);
    EXPECT_LT(risk_result.leverage_multiplier, 1.0);
}

TEST_F(RiskManagerTest, NetLeverageExceeded) {
    // Create positions that exceed net leverage (2x)
    auto positions = create_test_positions({
        {"AAPL", 10000, 104.0},    // $1.04M long
        {"MSFT", 5000, 208.0}      // $1.04M long
    }); // Total net: $2.08M on $1M capital = 2.08x leverage

    auto result = risk_manager_->process_positions(positions);
    ASSERT_TRUE(result.is_ok());
    const auto& risk_result = result.value();

    EXPECT_TRUE(risk_result.risk_exceeded);
    EXPECT_GT(risk_result.net_leverage, 2.0);
    EXPECT_LT(risk_result.leverage_multiplier, 1.0);
}

TEST_F(RiskManagerTest, VolatilityRisk) {
    // Update market data with volatile series
    auto volatile_data = create_test_market_data({
        {"AAPL", {100, 90, 110, 95, 115, 85}}  // 15% daily moves
    });

    ASSERT_TRUE(risk_manager_->update_market_data(volatile_data).is_ok());

    auto positions = create_test_positions({
        {"AAPL", 8000, 85.0}  // $680K position
    });

    auto result = risk_manager_->process_positions(positions);
    ASSERT_TRUE(result.is_ok());
    const auto& risk_result = result.value();

    EXPECT_TRUE(risk_result.risk_exceeded);
    EXPECT_GT(risk_result.portfolio_var, 0.15);
}

TEST_F(RiskManagerTest, CorrelationRisk) {
    // Create anti-correlated price series
    auto market_data = create_test_market_data({
        {"AAPL", {100, 102, 100, 102, 100}},
        {"MSFT", {200, 196, 200, 196, 200}}  // Opposite moves
    });

    ASSERT_TRUE(risk_manager_->update_market_data(market_data).is_ok());

    auto positions = create_test_positions({
        {"AAPL", 5000, 100.0},  // $500K long
        {"MSFT", -2500, 200.0}  // $500K short
    });

    auto result = risk_manager_->process_positions(positions);
    ASSERT_TRUE(result.is_ok());
    const auto& risk_result = result.value();

    EXPECT_LT(risk_result.correlation_risk, 0.7);
    EXPECT_NEAR(risk_result.correlation_multiplier, 1.0, 0.1);
}

TEST_F(RiskManagerTest, JumpRiskExceeded) {
    // Create market data with a price jump
    auto jump_data = create_test_market_data({
        {"AAPL", {100, 101, 102, 115, 116}}  // 13% jump
    });

    ASSERT_TRUE(risk_manager_->update_market_data(jump_data).is_ok());

    auto positions = create_test_positions({
        {"AAPL", 10000, 116.0}
    });

    auto result = risk_manager_->process_positions(positions);
    ASSERT_TRUE(result.is_ok());
    const auto& risk_result = result.value();

    EXPECT_TRUE(risk_result.risk_exceeded);
    EXPECT_GT(risk_result.jump_risk, 0.10);
}

TEST_F(RiskManagerTest, InvalidMarketData) {
    // Empty data should be handled gracefully
    auto result = risk_manager_->update_market_data({});
    ASSERT_TRUE(result.is_ok());

    // Should still be able to process positions using previously loaded data
    auto positions = create_test_positions({
        {"AAPL", 1000, 100.0}
    });

    auto risk_result = risk_manager_->process_positions(positions);
    ASSERT_TRUE(risk_result.is_ok());
}

TEST_F(RiskManagerTest, PositionSymbolMismatch) {
    // Position in symbol without market data
    auto positions = create_test_positions({
        {"AAPL", 1000, 104.0},    // Has market data
        {"UNKNOWN", 1000, 100.0}   // No market data
    });

    auto result = risk_manager_->process_positions(positions);
    ASSERT_TRUE(result.is_ok());
    
    // Should still calculate leverage risks
    const auto& risk_result = result.value();
    EXPECT_GT(risk_result.gross_leverage, 0.0);
}

TEST_F(RiskManagerTest, MultipleRiskFactors) {
    // Create data with high volatility and correlation
    auto market_data = create_test_market_data({
        {"AAPL", {100, 90, 110, 95, 115}},
        {"MSFT", {200, 180, 220, 190, 230}}
    });

    ASSERT_TRUE(risk_manager_->update_market_data(market_data).is_ok());

    auto positions = create_test_positions({
        {"AAPL", 8000, 115.0},   // $920K
        {"MSFT", 4000, 230.0}    // $920K
    });

    auto result = risk_manager_->process_positions(positions);
    ASSERT_TRUE(result.is_ok());
    const auto& risk_result = result.value();

    EXPECT_TRUE(risk_result.risk_exceeded);
    EXPECT_LT(risk_result.recommended_scale, 0.7);  // Significant reduction needed
}