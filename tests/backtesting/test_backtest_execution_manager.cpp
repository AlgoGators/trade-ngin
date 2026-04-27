#include <gtest/gtest.h>
#include <map>
#include <unordered_map>
#include "../core/test_base.hpp"
#include "trade_ngin/backtest/backtest_execution_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

namespace {

Position make_position(const std::string& symbol, double qty, double avg_price = 100.0) {
    return Position(symbol, Quantity(qty), Price(avg_price), Decimal(0.0), Decimal(0.0),
                    Timestamp{});
}

}  // namespace

class BacktestExecutionManagerTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        BacktestExecutionConfig config;
        config.explicit_fee_per_contract = 1.50;
        manager_ = std::make_unique<BacktestExecutionManager>(config);
    }

    std::unique_ptr<BacktestExecutionManager> manager_;
    Timestamp ts_{};
};

TEST_F(BacktestExecutionManagerTest, EmptyNewPositionsProducesNoExecutions) {
    std::map<std::string, Position> current;
    std::map<std::string, Position> next;
    std::unordered_map<std::string, double> prices = {{"ES", 4000.0}};
    auto execs = manager_->generate_executions(current, next, prices, ts_);
    EXPECT_TRUE(execs.empty());
    EXPECT_EQ(manager_->get_execution_count(), 0);
}

TEST_F(BacktestExecutionManagerTest, NewPositionWithoutPriorEntryGeneratesBuy) {
    std::map<std::string, Position> current;
    std::map<std::string, Position> next = {{"ES", make_position("ES", 5.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4000.0}};
    auto execs = manager_->generate_executions(current, next, prices, ts_);
    ASSERT_EQ(execs.size(), 1u);
    EXPECT_EQ(execs[0].symbol, "ES");
    EXPECT_EQ(execs[0].side, Side::BUY);
    EXPECT_DOUBLE_EQ(static_cast<double>(execs[0].filled_quantity), 5.0);
    EXPECT_DOUBLE_EQ(static_cast<double>(execs[0].fill_price), 4000.0);
}

TEST_F(BacktestExecutionManagerTest, ShrinkingPositionGeneratesSell) {
    std::map<std::string, Position> current = {{"ES", make_position("ES", 10.0)}};
    std::map<std::string, Position> next = {{"ES", make_position("ES", 3.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4000.0}};
    auto execs = manager_->generate_executions(current, next, prices, ts_);
    ASSERT_EQ(execs.size(), 1u);
    EXPECT_EQ(execs[0].side, Side::SELL);
    EXPECT_DOUBLE_EQ(static_cast<double>(execs[0].filled_quantity), 7.0);
}

TEST_F(BacktestExecutionManagerTest, FlippingFromLongToShortGeneratesSingleSell) {
    std::map<std::string, Position> current = {{"ES", make_position("ES", 4.0)}};
    std::map<std::string, Position> next = {{"ES", make_position("ES", -2.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4000.0}};
    auto execs = manager_->generate_executions(current, next, prices, ts_);
    ASSERT_EQ(execs.size(), 1u);
    EXPECT_EQ(execs[0].side, Side::SELL);
    EXPECT_DOUBLE_EQ(static_cast<double>(execs[0].filled_quantity), 6.0);
}

TEST_F(BacktestExecutionManagerTest, NoChangeProducesNoExecution) {
    std::map<std::string, Position> current = {{"ES", make_position("ES", 5.0)}};
    std::map<std::string, Position> next = {{"ES", make_position("ES", 5.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4000.0}};
    auto execs = manager_->generate_executions(current, next, prices, ts_);
    EXPECT_TRUE(execs.empty());
}

TEST_F(BacktestExecutionManagerTest, SubThresholdChangeIsSkipped) {
    std::map<std::string, Position> current = {{"ES", make_position("ES", 1.0)}};
    // Change of 5e-5 is below the 1e-4 threshold.
    std::map<std::string, Position> next = {{"ES", make_position("ES", 1.00005)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4000.0}};
    auto execs = manager_->generate_executions(current, next, prices, ts_);
    EXPECT_TRUE(execs.empty());
}

TEST_F(BacktestExecutionManagerTest, MissingPriceSkipsExecution) {
    std::map<std::string, Position> current;
    std::map<std::string, Position> next = {{"ES", make_position("ES", 5.0)}};
    std::unordered_map<std::string, double> prices;  // no entry for ES
    auto execs = manager_->generate_executions(current, next, prices, ts_);
    EXPECT_TRUE(execs.empty());
}

TEST_F(BacktestExecutionManagerTest, NonPositivePriceSkipsExecution) {
    std::map<std::string, Position> current;
    std::map<std::string, Position> next = {{"ES", make_position("ES", 5.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 0.0}};
    auto execs = manager_->generate_executions(current, next, prices, ts_);
    EXPECT_TRUE(execs.empty());

    prices["ES"] = -10.0;
    execs = manager_->generate_executions(current, next, prices, ts_);
    EXPECT_TRUE(execs.empty());
}

TEST_F(BacktestExecutionManagerTest, MultipleSymbolsAllGenerateExecutions) {
    std::map<std::string, Position> current;
    std::map<std::string, Position> next = {
        {"ES", make_position("ES", 5.0)},
        {"NQ", make_position("NQ", -3.0)},
        {"CL", make_position("CL", 2.5)},
    };
    std::unordered_map<std::string, double> prices = {
        {"ES", 4000.0}, {"NQ", 15000.0}, {"CL", 80.0}};
    auto execs = manager_->generate_executions(current, next, prices, ts_);
    EXPECT_EQ(execs.size(), 3u);
}

TEST_F(BacktestExecutionManagerTest, OrderAndExecIdsAreUnique) {
    std::map<std::string, Position> current;
    std::map<std::string, Position> next = {
        {"ES", make_position("ES", 1.0)},
        {"NQ", make_position("NQ", 1.0)},
    };
    std::unordered_map<std::string, double> prices = {{"ES", 4000.0}, {"NQ", 15000.0}};
    auto execs = manager_->generate_executions(current, next, prices, ts_);
    ASSERT_EQ(execs.size(), 2u);
    EXPECT_NE(execs[0].order_id, execs[1].order_id);
    EXPECT_NE(execs[0].exec_id, execs[1].exec_id);
}

TEST_F(BacktestExecutionManagerTest, GenerateExecutionDirectlyBuysOnPositiveDelta) {
    auto exec = manager_->generate_execution("ES", 4.0, 4000.0, ts_);
    EXPECT_EQ(exec.symbol, "ES");
    EXPECT_EQ(exec.side, Side::BUY);
    EXPECT_DOUBLE_EQ(static_cast<double>(exec.filled_quantity), 4.0);
    EXPECT_DOUBLE_EQ(static_cast<double>(exec.fill_price), 4000.0);
    EXPECT_FALSE(exec.is_partial);
}

TEST_F(BacktestExecutionManagerTest, GenerateExecutionDirectlySellsOnNegativeDelta) {
    auto exec = manager_->generate_execution("ES", -2.5, 4000.0, ts_);
    EXPECT_EQ(exec.side, Side::SELL);
    EXPECT_DOUBLE_EQ(static_cast<double>(exec.filled_quantity), 2.5);
}

TEST_F(BacktestExecutionManagerTest, ExecutionReportContainsTransactionCosts) {
    auto exec = manager_->generate_execution("ES", 10.0, 4000.0, ts_);
    // commissions_fees should be |qty| × explicit_fee_per_contract = 10 * 1.50 = 15
    EXPECT_DOUBLE_EQ(static_cast<double>(exec.commissions_fees), 15.0);
    // total_transaction_costs is at least the explicit fees
    EXPECT_GE(static_cast<double>(exec.total_transaction_costs),
              static_cast<double>(exec.commissions_fees));
}

TEST_F(BacktestExecutionManagerTest, ExecCounterAdvancesPerExecutionGenerated) {
    EXPECT_EQ(manager_->get_execution_count(), 0);
    manager_->generate_execution("ES", 1.0, 4000.0, ts_);
    EXPECT_EQ(manager_->get_execution_count(), 1);
    manager_->generate_execution("ES", 1.0, 4000.0, ts_);
    EXPECT_EQ(manager_->get_execution_count(), 2);
}

TEST_F(BacktestExecutionManagerTest, ResetClearsCounterAndTCData) {
    manager_->update_market_data("ES", 1'000'000.0, 4010.0, 4000.0);
    manager_->generate_execution("ES", 1.0, 4000.0, ts_);
    ASSERT_GT(manager_->get_execution_count(), 0);
    ASSERT_GT(manager_->get_adv("ES"), 0.0);

    manager_->reset();
    EXPECT_EQ(manager_->get_execution_count(), 0);
    EXPECT_DOUBLE_EQ(manager_->get_adv("ES"), 0.0);
}

TEST_F(BacktestExecutionManagerTest, UpdateMarketDataExposesADV) {
    EXPECT_DOUBLE_EQ(manager_->get_adv("ES"), 0.0);  // no data yet
    manager_->update_market_data("ES", 1'000'000.0, 4010.0, 4000.0);
    EXPECT_GT(manager_->get_adv("ES"), 0.0);
}

TEST_F(BacktestExecutionManagerTest, GetTransactionCostManagerReturnsLiveReference) {
    auto& tc = manager_->get_transaction_cost_manager();
    tc.update_market_data("ES", 500'000.0, 4010.0, 4000.0);
    // Reading via the manager wrapper should now see the same data.
    EXPECT_GT(manager_->get_adv("ES"), 0.0);
}
