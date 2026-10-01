#include <gtest/gtest.h>
#include <limits>
#include "trade_ngin/apps/equity_strategy_consumption.hpp"
#include "trade_ngin/strategy/equity_strategy_builder.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

using namespace trade_ngin;
namespace {
struct ComponentCleanup { ~ComponentCleanup(){Logger::register_component("");} };
StrategyConfig config() {StrategyConfig result;result.capital_allocation=1000;result.max_leverage=1;
    result.trading_params["SYN"]=1;result.position_limits["SYN"]=100;return result;}
MeanReversionConfig policy() {MeanReversionConfig result;result.lookback_period=3;
    result.vol_lookback=3;result.entry_threshold=.5;result.fractional_min_adv=1;return result;}
std::shared_ptr<PostgresDatabase> inert_database() {
    // No connect call; BaseStrategy::initialize requires a manager pointer only.
    return std::make_shared<PostgresDatabase>("host=/invalid/owned-test-sentinel dbname=invalid user=invalid");
}
std::vector<Bar> bars(std::initializer_list<double> prices) {
    std::vector<Bar> result;int day=1;
    for(auto price:prices) {Bar bar;bar.symbol="SYN";bar.close=price;bar.volume=100;
        bar.timestamp=Timestamp{}+std::chrono::hours(24*day++);result.push_back(bar);}
    return result;
}
void running(MeanReversionStrategy& strategy) {
    ASSERT_FALSE(strategy.initialize().is_error());ASSERT_FALSE(strategy.start().is_error());
}
}

TEST(EquityMeanReversionConsumption, ActualWarmupReadsNeverInventSizingThresholdsOrStopPolicy) {
    ComponentCleanup cleanup;
    MeanReversionStrategy strategy("warmup",config(),policy(),inert_database());running(strategy);
    StrategyConsumptionTrace trace;
    ASSERT_FALSE(strategy.on_data(bars({10}),&trace).is_error());
    ASSERT_EQ(trace.profile,StrategyConsumptionProfile::MeanReversion);
    const auto& read=trace.mean_reversion.symbols.at("SYN");
    EXPECT_EQ(read.lookback_period,3);EXPECT_EQ(read.vol_lookback,3);
    EXPECT_FALSE(read.entry_threshold);EXPECT_FALSE(read.exit_threshold);
    EXPECT_FALSE(read.capital_allocation);EXPECT_FALSE(read.use_stop_loss);
    ASSERT_FALSE(project_equity_strategy_consumption(trace).is_error());
    EXPECT_FALSE(project_equity_strategy_consumption(trace).value()["full_run_certification"].get<bool>());
}

TEST(EquityMeanReversionConsumption, ActualSizingRecordsFractionalEligibilityAndLimitWithIdenticalPlainResult) {
    ComponentCleanup cleanup;
    MeanReversionStrategy observed("observed",config(),policy(),inert_database());running(observed);
    MeanReversionStrategy plain("plain",config(),policy(),inert_database());running(plain);
    auto input=bars({10,11,8});StrategyConsumptionTrace trace;
    ASSERT_FALSE(observed.on_data(input,&trace).is_error());ASSERT_FALSE(plain.on_data(input).is_error());
    const auto& read=trace.mean_reversion.symbols.at("SYN");
    ASSERT_TRUE(read.sizing_reached);EXPECT_EQ(read.entry_threshold,.5);
    EXPECT_EQ(read.capital_allocation,1000);EXPECT_EQ(read.fractional_min_adv,1);
    EXPECT_EQ(read.fractional_eligible,true);EXPECT_TRUE(read.position_limit.present);
    EXPECT_EQ(read.position_limit.value,100);EXPECT_FALSE(read.exit_threshold);
    EXPECT_EQ(observed.get_target_positions().at("SYN").quantity,plain.get_target_positions().at("SYN").quantity);
    EXPECT_EQ(observed.get_positions().at("SYN").quantity,plain.get_positions().at("SYN").quantity);
    ASSERT_FALSE(project_equity_strategy_consumption(trace).is_error());
}

TEST(EquityMeanReversionConsumption, ActualOptOutDoesNotClaimPriceOrAdvThresholdReads) {
    ComponentCleanup cleanup;auto parameters=policy();parameters.allow_fractional_shares=false;
    MeanReversionStrategy strategy("whole",config(),parameters,inert_database());running(strategy);
    StrategyConsumptionTrace trace;ASSERT_FALSE(strategy.on_data(bars({10,11,8}),&trace).is_error());
    const auto& read=trace.mean_reversion.symbols.at("SYN");
    EXPECT_EQ(read.allow_fractional_shares,false);EXPECT_FALSE(read.fractional_min_price);
    EXPECT_FALSE(read.fractional_min_adv);EXPECT_FALSE(read.fractional_adv_reached);
    EXPECT_EQ(read.fractional_eligible,false);
    ASSERT_FALSE(project_equity_strategy_consumption(trace).is_error());
}

TEST(EquityMeanReversionConsumption, UnsupportedForeignProfileAndCapacityExcessAreRefused) {
    StrategyConsumptionTrace trace;
    EXPECT_TRUE(project_equity_strategy_consumption(trace).is_error());
    trace.profile=StrategyConsumptionProfile::Standard;
    EXPECT_TRUE(project_equity_strategy_consumption(trace).is_error());
    trace.profile=StrategyConsumptionProfile::MeanReversion;trace.mean_reversion.capacity_exceeded=true;
    EXPECT_TRUE(project_equity_strategy_consumption(trace).is_error());
}

TEST(EquityMeanReversionConsumption, ContradictoryAbsentBranchAndNonfiniteObservedValuesAreRefused) {
    StrategyConsumptionTrace trace;trace.profile=StrategyConsumptionProfile::MeanReversion;
    auto& read=trace.mean_reversion.symbols["SYN"];
    read.fractional_min_adv=50000;
    EXPECT_TRUE(project_equity_strategy_consumption(trace).is_error());
    read={};read.entry_threshold=std::numeric_limits<double>::infinity();
    EXPECT_TRUE(project_equity_strategy_consumption(trace).is_error());
    read={};read.position_limit.present=true;
    EXPECT_TRUE(project_equity_strategy_consumption(trace).is_error());
}

TEST(EquityMeanReversionConsumption, UnconfiguredBarsDoNotCreatePhantomConsumerSymbols) {
    ComponentCleanup cleanup;
    MeanReversionStrategy strategy("configured",config(),policy(),inert_database());running(strategy);
    auto input=bars({10});input.front().symbol="FOREIGN";
    StrategyConsumptionTrace trace;ASSERT_FALSE(strategy.on_data(input,&trace).is_error());
    EXPECT_TRUE(trace.mean_reversion.symbols.empty());
    EXPECT_TRUE(strategy.get_price_history().count("SYN"));
    EXPECT_FALSE(strategy.get_price_history().count("FOREIGN"));
}

TEST(EquityStrategyBuilder, OptimizerContradictionFailsClosed) {
    EXPECT_TRUE(apps::refuse_if_optimizer_requested(true).is_error());
    EXPECT_TRUE(apps::refuse_if_optimizer_requested(false).is_ok());
}

TEST(EquityStrategyBuilder, BacktestSnapshotContainsEveryEnabledSleeve) {
    const nlohmann::json first = {
        {"type", "MeanReversionStrategy"}, {"config", {{"lookback_period", 10}}}};
    const nlohmann::json second = {
        {"type", "MeanReversionStrategy"}, {"config", {{"lookback_period", 20}}}};
    const std::vector<apps::EquityStrategyEntry> entries = {
        {"FIRST", "MeanReversionStrategy", 0.7, first},
        {"SECOND", "MeanReversionStrategy", 0.3, second}};

    auto snapshot = apps::build_equity_backtest_config_snapshot(
        entries, {{"FIRST", 0.7}, {"SECOND", 0.3}});

    ASSERT_EQ(snapshot.at("strategies").size(), 2u);
    EXPECT_EQ(snapshot["strategies"]["FIRST"]["config"]["lookback_period"], 10);
    EXPECT_EQ(snapshot["strategies"]["SECOND"]["config"]["lookback_period"], 20);
    EXPECT_DOUBLE_EQ(snapshot["strategies"]["FIRST"]["allocation"], 0.7);
    EXPECT_DOUBLE_EQ(snapshot["strategies"]["SECOND"]["allocation"], 0.3);
}
