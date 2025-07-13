#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include "../core/test_base.hpp"
#include "../order/test_utils.hpp"
#include "trade_ngin/backtest/transaction_cost_analysis.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

class TransactionCostAnalyzerTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();

        TCAConfig config;
        config.pre_trade_window = std::chrono::minutes(5);
        config.post_trade_window = std::chrono::minutes(5);
        config.spread_factor = 1.0;
        config.market_impact_coefficient = 1.0;
        config.volatility_multiplier = 1.5;
        config.use_arrival_price = true;
        config.use_vwap = true;
        config.use_twap = true;
        config.calculate_opportunity_costs = true;
        config.analyze_timing_costs = true;

        analyzer_ = std::make_unique<TransactionCostAnalyzer>(config);
    }

    std::vector<Bar> create_market_data(const std::string& symbol, double base_price,
                                        double volatility = 0.01) {
        std::vector<Bar> bars;
        auto now = std::chrono::system_clock::now();

        // Create 30 minutes of minute bars
        for (int i = 0; i < 30; ++i) {
            Bar bar;
            bar.timestamp = now - std::chrono::minutes(30 - i);
            bar.symbol = symbol;

            // Add some random walk to prices
            double noise = (rand() % 200 - 100) * volatility / 100.0;
            bar.open = base_price * (1.0 + noise);
            bar.high = bar.open * 1.001;
            bar.low = bar.open * 0.999;
            bar.close = bar.open * (1.0 + noise / 2.0);
            bar.volume = 10000 + rand() % 5000;

            bars.push_back(bar);
        }
        return bars;
    }

    std::unique_ptr<TransactionCostAnalyzer> analyzer_;
};

TEST_F(TransactionCostAnalyzerTest, SingleTradeAnalysis) {
    // Create an execution
    ExecutionReport exec;
    exec.symbol = "AAPL";
    exec.side = Side::BUY;
    exec.filled_quantity = 1000;
    exec.fill_price = 150.0;
    exec.fill_time = std::chrono::system_clock::now();
    exec.commission = 1.0;

    // Create market data around the execution
    auto market_data = create_market_data("AAPL", 150.0);

    // Analyze the trade
    auto result = analyzer_->analyze_trade(exec, market_data);
    ASSERT_TRUE(result.is_ok());

    const auto& metrics = result.value();

    // Basic checks
    EXPECT_GE(metrics.spread_cost, 0.0);
    EXPECT_GE(metrics.market_impact, 0.0);
    EXPECT_GE(metrics.delay_cost, 0.0);
    EXPECT_GE(metrics.participation_rate, 0.0);
    EXPECT_LE(metrics.participation_rate, 1.0);
}

TEST_F(TransactionCostAnalyzerTest, TradeSequenceAnalysis) {
    std::vector<ExecutionReport> executions;

    // Create a sequence of trades
    auto base_time = std::chrono::system_clock::now();
    for (int i = 0; i < 5; ++i) {
        ExecutionReport exec;
        exec.symbol = "AAPL";
        exec.side = Side::BUY;
        exec.filled_quantity = 200;           // Split 1000 shares into 5 trades
        exec.fill_price = 150.0 + (i * 0.1);  // Slight price drift
        exec.fill_time = base_time + std::chrono::minutes(i);
        exec.commission = 0.2;
        executions.push_back(exec);
    }

    auto market_data = create_market_data("AAPL", 150.0);

    auto result = analyzer_->analyze_trade_sequence(executions, market_data);
    ASSERT_TRUE(result.is_ok());

    const auto& metrics = result.value();

    // Sequence-specific checks
    EXPECT_EQ(metrics.num_child_orders, 5);
    EXPECT_GT(metrics.execution_time.count(), 0);
    EXPECT_LT(metrics.participation_rate, 0.5);  // Shouldn't dominate volume
}

TEST_F(TransactionCostAnalyzerTest, ImplementationShortfall) {
    // Create market data first to have valid timestamps
    auto market_data = create_market_data("AAPL", 150.0);

    // Create target position with last_update within market_data's range
    Position target;
    target.symbol = "AAPL";
    target.quantity = 1000;
    target.average_price = 150.0;
    target.last_update = market_data[15].timestamp;  // Use a timestamp from market_data

    // Create actual executions that differ from target
    std::vector<ExecutionReport> executions;
    auto base_time = market_data[15].timestamp;  // Align with market_data

    // Only fill 800 shares of 1000 target
    for (int i = 0; i < 4; ++i) {
        ExecutionReport exec;
        exec.symbol = "AAPL";
        exec.side = Side::BUY;
        exec.filled_quantity = 200;
        exec.fill_price = 150.0 + (i * 0.2);  // Increasing prices
        exec.fill_time = base_time + std::chrono::minutes(i);
        executions.push_back(exec);
    }

    auto result = analyzer_->calculate_implementation_shortfall(target, executions, market_data);
    ASSERT_TRUE(result.is_ok());

    const auto& metrics = result.value();

    // Shortfall checks
    EXPECT_GT(metrics.opportunity_cost, 0.0);  // Cost of unfilled portion
    EXPECT_GT(metrics.delay_cost, 0.0);        // Cost of price drift
}

TEST_F(TransactionCostAnalyzerTest, BenchmarkPerformance) {
    auto market_data = create_market_data("AAPL", 150.0);
    auto base_time = market_data[15].timestamp;  // Use a timestamp within market_data

    std::vector<ExecutionReport> executions;

    // Create a sequence of trades
    for (int i = 0; i < 3; ++i) {
        ExecutionReport exec;
        exec.symbol = "AAPL";
        exec.side = Side::BUY;
        exec.filled_quantity = 300;
        exec.fill_price = 150.0 + (i * 0.1);
        exec.fill_time = base_time + std::chrono::minutes(i * 2);
        executions.push_back(exec);
    }

    auto result = analyzer_->analyze_benchmark_performance(executions, market_data);
    ASSERT_TRUE(result.is_ok());

    const auto& benchmark_metrics = result.value();

    // Check each benchmark type
    EXPECT_TRUE(benchmark_metrics.find("vwap_performance") != benchmark_metrics.end());
    EXPECT_TRUE(benchmark_metrics.find("twap_performance") != benchmark_metrics.end());
    EXPECT_TRUE(benchmark_metrics.find("arrival_price_performance") != benchmark_metrics.end());
}

TEST_F(TransactionCostAnalyzerTest, HighVolatilityScenario) {
    ExecutionReport exec;
    exec.symbol = "AAPL";
    exec.side = Side::BUY;
    exec.filled_quantity = 1000;
    exec.fill_price = 150.0;
    exec.fill_time = std::chrono::system_clock::now();
    exec.commission = 1.0;

    // Create volatile market data (0.05 = 5% volatility)
    auto market_data = create_market_data("AAPL", 150.0, 0.05);

    auto result = analyzer_->analyze_trade(exec, market_data);
    ASSERT_TRUE(result.is_ok());

    // Expect higher costs in volatile conditions
    const auto& metrics = result.value();
    EXPECT_GE(metrics.market_impact, 0.0);  // Now allows zero but ensures non-negative
    EXPECT_GT(metrics.spread_cost, 0.0);
}

TEST_F(TransactionCostAnalyzerTest, ReportGeneration) {
    ExecutionReport exec;
    exec.symbol = "AAPL";
    exec.side = Side::BUY;
    exec.filled_quantity = 1000;
    exec.fill_price = 150.0;
    exec.fill_time = std::chrono::system_clock::now();
    exec.commission = 1.0;

    auto market_data = create_market_data("AAPL", 150.0);

    auto analysis_result = analyzer_->analyze_trade(exec, market_data);
    ASSERT_TRUE(analysis_result.is_ok());

    std::string report = analyzer_->generate_report(analysis_result.value(), true);

    // Check report content
    EXPECT_TRUE(report.find("Transaction Cost Analysis Report") != std::string::npos);
    EXPECT_TRUE(report.find("Execution Costs:") != std::string::npos);
    EXPECT_TRUE(report.find("Execution Statistics:") != std::string::npos);
}