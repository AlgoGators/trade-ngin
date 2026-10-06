#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include "trade_ngin/apps/equity_multi_live_runner.hpp"

using namespace trade_ngin;

TEST(EquityMultiLiveAccounting,
     AccumulatesRealizedAndCostsAndEmitsOnlyLiveResultColumns) {
    apps::EquityMultiLivePriorAccounting prior;
    prior.found = true;
    prior.current_portfolio_value = 1020.0;
    prior.total_pnl = 20.0;
    prior.total_realized_pnl = 50.0;
    prior.total_unrealized_pnl = -20.0;
    prior.total_transaction_costs = 10.0;

    apps::EquityMultiLiveDailyAccounting daily;
    daily.initial_capital = 1000.0;
    daily.daily_realized_pnl = 30.0;
    daily.total_unrealized_pnl = -5.0;
    daily.daily_transaction_costs = 2.0;
    daily.gross_notional = 500.0;
    daily.net_notional = 300.0;
    daily.margin_posted = 500.0;
    daily.trading_days = 252;

    auto result = apps::derive_equity_multi_live_accounting(prior, daily);

    ASSERT_TRUE(result.is_ok())
        << (result.is_error() ? result.error()->what() : "");
    const auto& row = result.value();
    EXPECT_DOUBLE_EQ(row.daily_unrealized_pnl, 15.0);
    EXPECT_DOUBLE_EQ(row.daily_pnl, 43.0);
    EXPECT_DOUBLE_EQ(row.total_realized_pnl, 80.0);
    EXPECT_DOUBLE_EQ(row.total_transaction_costs, 12.0);
    EXPECT_DOUBLE_EQ(row.total_pnl, 63.0);
    EXPECT_DOUBLE_EQ(row.current_portfolio_value, 1063.0);
    EXPECT_NEAR(row.daily_return, 4.215686274509804, 1e-12);
    EXPECT_DOUBLE_EQ(row.total_cumulative_return, 6.3);
    EXPECT_NEAR(row.total_annualized_return, 6.3, 1e-12);
    EXPECT_NEAR(row.gross_leverage, 500.0 / 1063.0, 1e-12);
    EXPECT_NEAR(row.net_leverage, 300.0 / 1063.0, 1e-12);
    EXPECT_DOUBLE_EQ(row.cash_available, 563.0);

    const auto metrics = row.live_result_metrics();
    EXPECT_EQ(metrics.at("total_realized_pnl"), 80.0);
    EXPECT_EQ(metrics.at("daily_realized_pnl"), 30.0);
    EXPECT_EQ(metrics.at("daily_transaction_costs"), 2.0);
    EXPECT_EQ(metrics.at("total_transaction_costs"), 12.0);
    EXPECT_EQ(metrics.at("total_cumulative_return"), 6.3);
    EXPECT_FALSE(metrics.contains("initial_capital"));
    EXPECT_FALSE(metrics.contains("total_return"));
    EXPECT_FALSE(metrics.contains("gross_leverage"));
}

TEST(EquityMultiLiveAccounting, FirstDayStartsEveryCumulativeAtTheBookAnchor) {
    apps::EquityMultiLiveDailyAccounting daily;
    daily.initial_capital = 1000.0;
    daily.daily_realized_pnl = 10.0;
    daily.total_unrealized_pnl = 5.0;
    daily.daily_transaction_costs = 1.0;
    daily.trading_days = 1;

    auto result = apps::derive_equity_multi_live_accounting({}, daily);

    ASSERT_TRUE(result.is_ok());
    EXPECT_DOUBLE_EQ(result.value().daily_pnl, 14.0);
    EXPECT_DOUBLE_EQ(result.value().total_pnl, 14.0);
    EXPECT_DOUBLE_EQ(result.value().current_portfolio_value, 1014.0);
    EXPECT_DOUBLE_EQ(result.value().total_realized_pnl, 10.0);
    EXPECT_DOUBLE_EQ(result.value().total_transaction_costs, 1.0);
    EXPECT_DOUBLE_EQ(result.value().total_annualized_return, 1.4);
}

TEST(EquityMultiLiveAccounting, RefusesAnInconsistentOrInvalidAccountingFrame) {
    apps::EquityMultiLiveDailyAccounting daily;
    daily.initial_capital = 1000.0;
    daily.trading_days = 2;

    apps::EquityMultiLivePriorAccounting inconsistent;
    inconsistent.found = true;
    inconsistent.current_portfolio_value = 1020.0;
    inconsistent.total_pnl = 20.0;
    inconsistent.total_realized_pnl = 50.0;
    inconsistent.total_unrealized_pnl = -10.0;
    inconsistent.total_transaction_costs = 10.0;
    EXPECT_TRUE(
        apps::derive_equity_multi_live_accounting(inconsistent, daily).is_error());

    daily.daily_transaction_costs = -1.0;
    EXPECT_TRUE(apps::derive_equity_multi_live_accounting({}, daily).is_error());
    daily.daily_transaction_costs = 0.0;
    daily.total_unrealized_pnl = std::numeric_limits<double>::infinity();
    EXPECT_TRUE(apps::derive_equity_multi_live_accounting({}, daily).is_error());
}

namespace {

Position held_position(const std::string& symbol, double quantity, double basis) {
    Position position;
    position.symbol = symbol;
    position.quantity = Quantity(quantity);
    position.average_price = Decimal(basis);
    position.realized_pnl = Decimal(0.0);
    position.unrealized_pnl = Decimal(0.0);
    return position;
}

}  // namespace

TEST(EquityOwnerCorporateActions,
     AppliesPriceRenameAndTerminationToOneOwnerWithoutTouchingAnother) {
    apps::EquityOwnerCorporateActionInput alpha;
    alpha.positions.emplace("OLD", held_position("OLD", 10.0, 100.0));
    alpha.price_restatements.push_back(
        {"OLD", "2026-11-02", CorpActionType::SPLIT, 2.0, 0.0, 0.0,
         CorpActionEvent::BasisProvenance::FORMED_ON_OR_BEFORE_EX_DATE,
         "owned-test"});
    alpha.aliases.push_back({"OLD", "NEW", "2026-11-02", "owned-test"});
    alpha.holding_start_dates.emplace("OLD", "2026-10-01");
    alpha.as_of_date = "2026-11-04";
    alpha.terminations.push_back(
        {"NEW", "2026-11-03", "delisted", "", 0.0, false});
    alpha.final_closes.emplace("NEW", 60.0);
    alpha.termination_feed_last_date = "2026-11-03";

    apps::EquityOwnerCorporateActionInput beta;
    beta.positions.emplace("OLD", held_position("OLD", 3.0, 25.0));
    beta.as_of_date = "2026-11-04";

    auto adjusted = apps::apply_equity_owner_corporate_actions(std::move(alpha));
    auto untouched = apps::apply_equity_owner_corporate_actions(std::move(beta));

    ASSERT_TRUE(adjusted.is_ok());
    ASSERT_TRUE(untouched.is_ok());
    ASSERT_TRUE(adjusted.value().positions.contains("NEW"));
    EXPECT_DOUBLE_EQ(
        adjusted.value().positions.at("NEW").quantity.as_double(), 0.0);
    EXPECT_DOUBLE_EQ(
        adjusted.value().positions.at("NEW").realized_pnl.as_double(), 200.0);
    ASSERT_EQ(adjusted.value().price_adjustments.size(), 1);
    ASSERT_TRUE(adjusted.value().post_class1_positions.contains("OLD"));
    EXPECT_DOUBLE_EQ(
        adjusted.value().post_class1_positions.at("OLD").quantity.as_double(),
        20.0);
    EXPECT_EQ(adjusted.value().applied_class1_ex_date.at("OLD"),
              "2026-11-02");
    EXPECT_EQ(adjusted.value().lifecycle_adjustments.size(), 2);
    ASSERT_EQ(adjusted.value().evidence_executions.size(), 1);
    EXPECT_EQ(adjusted.value().evidence_executions.front().symbol, "NEW");
    EXPECT_DOUBLE_EQ(adjusted.value().realized_by_symbol.at("NEW"), 200.0);

    ASSERT_TRUE(untouched.value().positions.contains("OLD"));
    EXPECT_DOUBLE_EQ(
        untouched.value().positions.at("OLD").quantity.as_double(), 3.0);
    EXPECT_DOUBLE_EQ(
        untouched.value().positions.at("OLD").average_price.as_double(), 25.0);
    EXPECT_TRUE(untouched.value().price_adjustments.empty());
    EXPECT_TRUE(untouched.value().lifecycle_adjustments.empty());
}

TEST(EquityOwnerCorporateActions, AppliesSpinoffBeforeOtherPriceRestatements) {
    apps::EquityOwnerCorporateActionInput input;
    input.positions.emplace("PARENT", held_position("PARENT", 10.0, 100.0));
    input.spinoffs.push_back(
        {"PARENT", "2026-11-02", 1.25, {{"CHILD", 0.5, 30.0}}});
    input.price_restatements.push_back(
        {"PARENT", "2026-11-02", CorpActionType::SPLIT, 2.0, 0.0, 0.0,
         CorpActionEvent::BasisProvenance::FORMED_ON_OR_BEFORE_EX_DATE,
         "owned-test"});
    input.as_of_date = "2026-11-04";
    input.spinoff_child_policy = SpinoffChildPolicy::LIQUIDATE_AT_FIRST_CLOSE;

    auto result = apps::apply_equity_owner_corporate_actions(std::move(input));

    ASSERT_TRUE(result.is_ok());
    ASSERT_TRUE(result.value().positions.contains("PARENT"));
    EXPECT_DOUBLE_EQ(
        result.value().positions.at("PARENT").quantity.as_double(), 20.0);
    EXPECT_DOUBLE_EQ(
        result.value().positions.at("PARENT").average_price.as_double(), 40.0);
    ASSERT_EQ(result.value().price_adjustments.size(), 1);
    ASSERT_TRUE(result.value().post_class1_positions.contains("PARENT"));
    EXPECT_DOUBLE_EQ(
        result.value().post_class1_positions.at("PARENT").quantity.as_double(),
        20.0);
    EXPECT_EQ(result.value().applied_class1_ex_date.at("PARENT"),
              "2026-11-02");
    ASSERT_EQ(result.value().lifecycle_adjustments.size(), 1);
    EXPECT_EQ(result.value().lifecycle_adjustments.front().outcome,
              LifecycleOutcome::SPUN_OFF_CHILD_SOLD);
    EXPECT_EQ(result.value().evidence_executions.size(), 2);
}
