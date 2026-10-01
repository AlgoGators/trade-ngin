#include <gtest/gtest.h>

#include <cmath>

#include "trade_ngin/live/corporate_actions_audit_log.hpp"
#include "trade_ngin/live/equity_sleeve_netting.hpp"
#include "trade_ngin/live/execution_manager.hpp"

using namespace trade_ngin::live;

namespace {

EquitySleevePositionBook sleeve(const std::string& owner, double target) {
    EquitySleevePositionBook book;
    book.strategy_name = owner;
    book.target_quantities["AAPL"] = target;
    book.reference_prices["AAPL"] = 100.0;
    return book;
}

}  // namespace

TEST(EquitySleeveNetting, KeepsGrossOwnerFillsButEmitsOneNetAccountOrder) {
    auto result = build_equity_sleeve_netting_plan(
        {sleeve("ALPHA", 7.0), sleeve("BETA", -3.0)});
    ASSERT_TRUE(result.is_ok()) << result.error()->what();

    const auto& plan = result.value();
    ASSERT_EQ(plan.logical_fills.size(), 2u);
    EXPECT_EQ(plan.logical_fills[0].strategy_name, "ALPHA");
    EXPECT_DOUBLE_EQ(plan.logical_fills[0].signed_quantity, 7.0);
    EXPECT_EQ(plan.logical_fills[1].strategy_name, "BETA");
    EXPECT_DOUBLE_EQ(plan.logical_fills[1].signed_quantity, -3.0);

    ASSERT_EQ(plan.account_orders.size(), 1u);
    EXPECT_EQ(plan.account_orders[0].symbol, "AAPL");
    EXPECT_DOUBLE_EQ(plan.account_orders[0].signed_quantity, 4.0);
    EXPECT_DOUBLE_EQ(std::abs(plan.logical_fills[0].signed_quantity) +
                         std::abs(plan.logical_fills[1].signed_quantity),
                     10.0);
}

TEST(EquitySleeveNetting, SplitsRuledNettingCreditProRataByAsIfCost) {
    auto result = build_equity_sleeve_netting_plan(
        {sleeve("ALPHA", 7.0), sleeve("BETA", -3.0)});
    ASSERT_TRUE(result.is_ok());

    auto attributed = reconcile_equity_sleeve_costs(
        result.value(),
        {{"ALPHA", "AAPL", 7.0, 0.07}, {"BETA", "AAPL", -3.0, 0.03}},
        {{"AAPL", 4.0, 0.04}});
    ASSERT_TRUE(attributed.is_ok()) << attributed.error()->what();
    ASSERT_EQ(attributed.value().size(), 2u);
    EXPECT_NEAR(attributed.value()[0].netting_adjustment, 0.042, 1e-12);
    EXPECT_NEAR(attributed.value()[1].netting_adjustment, 0.018, 1e-12);
    EXPECT_NEAR(attributed.value()[0].net_transaction_costs, 0.028, 1e-12);
    EXPECT_NEAR(attributed.value()[1].net_transaction_costs, 0.012, 1e-12);
    EXPECT_NEAR(attributed.value()[0].net_transaction_costs +
                    attributed.value()[1].net_transaction_costs,
                0.04, 1e-12);
}

TEST(EquitySleeveNetting, ExactInternalCrossHasNoExternalOrderOrCost) {
    auto result = build_equity_sleeve_netting_plan(
        {sleeve("ALPHA", 5.0), sleeve("BETA", -5.0)});
    ASSERT_TRUE(result.is_ok());
    EXPECT_TRUE(result.value().account_orders.empty());
    ASSERT_EQ(result.value().logical_fills.size(), 2u);

    auto attributed = reconcile_equity_sleeve_costs(
        result.value(),
        {{"ALPHA", "AAPL", 5.0, 0.05}, {"BETA", "AAPL", -5.0, 0.05}}, {});
    ASSERT_TRUE(attributed.is_ok());
    EXPECT_DOUBLE_EQ(attributed.value()[0].netting_adjustment, 0.05);
    EXPECT_DOUBLE_EQ(attributed.value()[1].netting_adjustment, 0.05);
    EXPECT_DOUBLE_EQ(attributed.value()[0].net_transaction_costs, 0.0);
    EXPECT_DOUBLE_EQ(attributed.value()[1].net_transaction_costs, 0.0);
}

TEST(EquitySleeveNetting, SupportsNegativeAdjustmentForConvexSameSideCost) {
    auto alpha = sleeve("ALPHA", 100.0);
    auto beta = sleeve("BETA", 60.0);
    auto result = build_equity_sleeve_netting_plan({alpha, beta});
    ASSERT_TRUE(result.is_ok());

    auto attributed = reconcile_equity_sleeve_costs(
        result.value(),
        {{"ALPHA", "AAPL", 100.0, 1.0}, {"BETA", "AAPL", 60.0, 0.36}},
        {{"AAPL", 160.0, 2.56}});
    ASSERT_TRUE(attributed.is_ok());
    EXPECT_NEAR(attributed.value()[0].netting_adjustment, -0.882352941176, 1e-12);
    EXPECT_NEAR(attributed.value()[1].netting_adjustment, -0.317647058824, 1e-12);
    EXPECT_NEAR(attributed.value()[0].net_transaction_costs +
                    attributed.value()[1].net_transaction_costs,
                2.56, 1e-12);
}

TEST(EquitySleeveNetting, FailsClosedOnAmbiguousOrInvalidOwnerBooks) {
    auto duplicate = build_equity_sleeve_netting_plan(
        {sleeve("ALPHA", 7.0), sleeve("ALPHA", -3.0)});
    EXPECT_TRUE(duplicate.is_error());

    auto empty_owner = sleeve("", 7.0);
    EXPECT_TRUE(build_equity_sleeve_netting_plan({empty_owner}).is_error());

    auto mismatched_price = sleeve("BETA", -3.0);
    mismatched_price.reference_prices["AAPL"] = 101.0;
    EXPECT_TRUE(build_equity_sleeve_netting_plan(
                    {sleeve("ALPHA", 7.0), mismatched_price})
                    .is_error());
}

TEST(EquitySleeveNetting, FailsClosedWhenExternalExecutionDoesNotMatchPlan) {
    auto result = build_equity_sleeve_netting_plan(
        {sleeve("ALPHA", 7.0), sleeve("BETA", -3.0)});
    ASSERT_TRUE(result.is_ok());

    EXPECT_TRUE(reconcile_equity_sleeve_costs(
                    result.value(),
                    {{"ALPHA", "AAPL", 7.0, 0.07},
                     {"BETA", "AAPL", -3.0, 0.03}},
                    {{"AAPL", 3.0, 0.04}})
                    .is_error());
    EXPECT_TRUE(reconcile_equity_sleeve_costs(
                    result.value(),
                    {{"ALPHA", "AAPL", 7.0, 0.07},
                     {"BETA", "AAPL", -3.0, 0.03}},
                    {{"AAPL", 4.0, -1.0}})
                    .is_error());
    EXPECT_TRUE(reconcile_equity_sleeve_costs(
                    result.value(),
                    {{"ALPHA", "AAPL", 7.0, 0.07},
                     {"BETA", "AAPL", -3.0, 0.03}}, {})
                    .is_error());
}

TEST(EquitySleeveNetting, GeneratesGrossOwnerExecutionsAndOneNetAccountExecution) {
    auto netting = build_equity_sleeve_netting_plan(
        {sleeve("ALPHA", 7.0), sleeve("BETA", -3.0)});
    ASSERT_TRUE(netting.is_ok());

    trade_ngin::ExecutionManager manager;
    auto generated = generate_equity_sleeve_executions(
        manager, netting.value(), trade_ngin::Timestamp{});
    ASSERT_TRUE(generated.is_ok()) << generated.error()->what();
    ASSERT_EQ(generated.value().sleeve_executions.size(), 2u);
    ASSERT_EQ(generated.value().account_executions.size(), 1u);

    const auto& alpha = generated.value().sleeve_executions[0];
    const auto& beta = generated.value().sleeve_executions[1];
    EXPECT_EQ(alpha.strategy_name, "ALPHA");
    EXPECT_EQ(alpha.execution.side, trade_ngin::Side::BUY);
    EXPECT_DOUBLE_EQ(alpha.execution.filled_quantity.as_double(), 7.0);
    EXPECT_EQ(beta.strategy_name, "BETA");
    EXPECT_EQ(beta.execution.side, trade_ngin::Side::SELL);
    EXPECT_DOUBLE_EQ(beta.execution.filled_quantity.as_double(), 3.0);

    const auto& account = generated.value().account_executions.front();
    EXPECT_EQ(account.side, trade_ngin::Side::BUY);
    EXPECT_DOUBLE_EQ(account.filled_quantity.as_double(), 4.0);
    EXPECT_NEAR(alpha.execution.net_transaction_costs().as_double() +
                    beta.execution.net_transaction_costs().as_double(),
                account.total_transaction_costs.as_double(), 1e-5);
}

TEST(EquityCorporateActionsIsolation, LegacyFileImportIsRestrictedToOriginalHouseOwner) {
    EXPECT_TRUE(trade_ngin::allows_legacy_corp_action_import(
        "EQUITY_MR_PORTFOLIO", "LIVE_EQUITY_MEAN_REVERSION",
        "EQUITY_MEAN_REVERSION"));
    EXPECT_FALSE(trade_ngin::allows_legacy_corp_action_import(
        "INVESTOR_A", "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION"));
    EXPECT_FALSE(trade_ngin::allows_legacy_corp_action_import(
        "EQUITY_MR_PORTFOLIO", "LIVE_EQUITY_ALPHA_BETA", "ALPHA"));
    EXPECT_FALSE(trade_ngin::allows_legacy_corp_action_import(
        "EQUITY_MR_PORTFOLIO", "LIVE_EQUITY_MEAN_REVERSION", "ALPHA"));
}
