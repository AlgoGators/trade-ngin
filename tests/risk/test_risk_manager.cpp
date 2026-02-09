#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <vector>
#include "../core/test_base.hpp"
#include "trade_ngin/risk/risk_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

class RiskManagerTest : public ::TestBase {
protected:
    void SetUp() override {
        RiskConfig config;
        config.var_limit = 0.15;          // 15% VaR limit
        config.jump_risk_limit = 0.10;    // 10% jump risk threshold
        config.max_correlation = 0.7;     // 70% correlation limit
        config.max_net_leverage = 2.0;    // 2x net leverage
        config.capital = 1000000.0;       // $1M capital
        config.confidence_level = 0.99;   // 99% confidence
        config.lookback_period = 252;     // 1 year lookback

        risk_manager_ = std::make_unique<RiskManager>(config);

        // Setup some basic market data that all tests can use
        default_market_data_ = create_test_market_data({{"AAPL", {100, 101, 102, 103, 104}},
                                                        {"MSFT", {200, 202, 204, 206, 208}},
                                                        {"GOOG", {2500, 2520, 2540, 2560, 2580}}});
    }

    std::unordered_map<std::string, Position> create_test_positions(
        const std::vector<std::tuple<std::string, double, double>>& position_data) {
        std::unordered_map<std::string, Position> positions;
        auto now = std::chrono::system_clock::now();

        for (const auto& [symbol, quantity, price] : position_data) {
            Position pos{symbol, quantity, price,
                         0.0,  // unrealized P&L
                         0.0,  // realized P&L
                         now};
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
    std::vector<Bar> default_market_data_;
};

TEST_F(RiskManagerTest, InitializationAndConfig) {
    const auto& config = risk_manager_->get_config();
    EXPECT_DOUBLE_EQ(config.var_limit, 0.15);
    EXPECT_DOUBLE_EQ(config.capital.as_double(), 1000000.0);
    EXPECT_DOUBLE_EQ(config.confidence_level, 0.99);
}

TEST_F(RiskManagerTest, LeverageExceeded) {
    auto positions = create_test_positions(
        {{"AAPL", 10000, 104.0}, {"MSFT", 5000, 208.0}, {"GOOG", 1000, 2580.0}});
    auto market_data = risk_manager_->create_market_data(default_market_data_);
    auto result = risk_manager_->process_positions(positions, market_data);
    ASSERT_TRUE(result.is_ok());
    const auto& risk_result = result.value();
    EXPECT_TRUE(risk_result.risk_exceeded);
    EXPECT_GT(risk_result.net_leverage, 2.0);
    EXPECT_LT(risk_result.leverage_multiplier, 1.0);
}

TEST_F(RiskManagerTest, NetLeverageExceeded) {
    auto positions = create_test_positions({{"AAPL", 10000, 104.0}, {"MSFT", 5000, 208.0}});
    auto market_data = risk_manager_->create_market_data(default_market_data_);
    auto result = risk_manager_->process_positions(positions, market_data);
    ASSERT_TRUE(result.is_ok());
    const auto& risk_result = result.value();
    EXPECT_TRUE(risk_result.risk_exceeded);
    EXPECT_GT(risk_result.net_leverage, 2.0);
    EXPECT_LT(risk_result.leverage_multiplier, 1.0);
}

TEST_F(RiskManagerTest, VolatilityRisk) {
    auto volatile_data = create_test_market_data({{"AAPL", {100, 90, 110, 95, 115, 85}}});
    auto market_data = risk_manager_->create_market_data(volatile_data);
    auto positions = create_test_positions({{"AAPL", 8000, 85.0}});
    auto result = risk_manager_->process_positions(positions, market_data);
    ASSERT_TRUE(result.is_ok());
    const auto& risk_result = result.value();
    EXPECT_TRUE(risk_result.risk_exceeded);
    EXPECT_GT(risk_result.portfolio_var, 0.15);
}

// TEST_F(RiskManagerTest, CorrelationRisk) {
//     auto market_data_vec = create_test_market_data(
//         {{"AAPL", {100, 102, 100, 102, 100}}, {"MSFT", {200, 196, 200, 196, 200}}});
//     auto market_data = risk_manager_->create_market_data(market_data_vec);
//     auto positions = create_test_positions({{"AAPL", 5000, 100.0}, {"MSFT", -2500, 200.0}});
//     auto result = risk_manager_->process_positions(positions, market_data);
//     ASSERT_TRUE(result.is_ok());
//     const auto& risk_result = result.value();
//     EXPECT_LT(risk_result.correlation_risk, 0.7);
//     EXPECT_NEAR(risk_result.correlation_multiplier, 1.0, 0.1);
// }

TEST_F(RiskManagerTest, JumpRiskExceeded) {
    auto jump_data = create_test_market_data({{"AAPL", {100, 101, 102, 115, 116}}});
    auto market_data = risk_manager_->create_market_data(jump_data);
    auto positions = create_test_positions({{"AAPL", 10000, 116.0}});
    auto result = risk_manager_->process_positions(positions, market_data);
    ASSERT_TRUE(result.is_ok());
    const auto& risk_result = result.value();
    EXPECT_TRUE(risk_result.risk_exceeded);
    EXPECT_GT(risk_result.jump_risk, 0.10);
}

TEST_F(RiskManagerTest, InvalidMarketData) {
    auto empty_data = std::vector<Bar>{};
    auto market_data = risk_manager_->create_market_data(empty_data);
    auto positions = create_test_positions({{"AAPL", 1000, 100.0}});
    auto risk_result = risk_manager_->process_positions(positions, market_data);
    ASSERT_TRUE(risk_result.is_ok());
}

TEST_F(RiskManagerTest, PositionSymbolMismatch) {
    // Position in symbol without market data
    auto positions = create_test_positions({
        {"AAPL", 1000, 104.0},    // Has market data
        {"UNKNOWN", 1000, 100.0}  // No market data
    });

    auto market_data = risk_manager_->create_market_data(default_market_data_);
    auto result = risk_manager_->process_positions(positions, market_data);
    ASSERT_TRUE(result.is_ok());

    // Should still calculate leverage risks
    const auto& risk_result = result.value();
    EXPECT_NE(risk_result.net_leverage, 0.0);
}

// TEST_F(RiskManagerTest, MultipleRiskFactors) {
//     auto positions =
//         create_test_positions({{"AAPL", 1000, 104.0}, {"MSFT", 500, 208.0}, {"GOOG", 100, 2580.0}});
//     auto market_data = risk_manager_->create_market_data(default_market_data_);
//     auto result = risk_manager_->process_positions(positions, market_data);
//     ASSERT_TRUE(result.is_ok());
//     const auto& risk_result = result.value();
//
//     EXPECT_TRUE(risk_result.risk_exceeded);
//     EXPECT_LT(risk_result.recommended_scale, 0.7);  // Significant reduction needed
// }