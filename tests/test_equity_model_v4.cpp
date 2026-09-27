// Actual imported strategy and pinned-main adjustment functions; no DB or delivery.
#include <gtest/gtest.h>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"
#include "trade_ngin/data/market_data_utils.hpp"

using namespace trade_ngin;

namespace {
struct ComponentCleanup { ~ComponentCleanup(){Logger::register_component("");} };
ExecutionReport fill(Side side,double quantity,double price,double costs) {
    ExecutionReport result;result.symbol="SYN";result.side=side;result.filled_quantity=quantity;
    result.fill_price=price;result.total_transaction_costs=costs;return result;
}
}

TEST(EquityModelV4, ActualMeanReversionPersistsGrossRealizedAndNetRiskMetrics) {
    ComponentCleanup cleanup;
    MeanReversionStrategy strategy("SYN",StrategyConfig{},MeanReversionConfig{},nullptr);
    ASSERT_FALSE(strategy.on_execution(fill(Side::BUY,100,10,1)).is_error());
    EXPECT_DOUBLE_EQ(strategy.get_positions().at("SYN").realized_pnl.as_double(),0);
    ASSERT_FALSE(strategy.on_execution(fill(Side::SELL,100,30,2)).is_error());
    const auto& position=strategy.get_positions().at("SYN");
    EXPECT_DOUBLE_EQ(position.realized_pnl.as_double(),2000);
    EXPECT_DOUBLE_EQ(position.quantity.as_double(),0);
    EXPECT_DOUBLE_EQ(strategy.get_metrics().realized_pnl,1997);
    EXPECT_DOUBLE_EQ(strategy.get_metrics().total_pnl,1997);
}

TEST(EquityModelV4, ActualMeanReversionFlipRealizesOnlyTheClosedShares) {
    ComponentCleanup cleanup;
    MeanReversionStrategy strategy("SYN",StrategyConfig{},MeanReversionConfig{},nullptr);
    ASSERT_FALSE(strategy.on_execution(fill(Side::BUY,100,10,1)).is_error());
    ASSERT_FALSE(strategy.on_execution(fill(Side::SELL,150,30,2)).is_error());
    const auto& position=strategy.get_positions().at("SYN");
    EXPECT_DOUBLE_EQ(position.realized_pnl.as_double(),2000);
    EXPECT_DOUBLE_EQ(position.quantity.as_double(),-50);
    EXPECT_DOUBLE_EQ(position.average_price.as_double(),30);
    EXPECT_DOUBLE_EQ(strategy.get_metrics().realized_pnl,1997);
}

TEST(EquityModelV4, ActualMeanReversionShortCoverCarriesBasisUntilTheFlip) {
    ComponentCleanup cleanup;
    MeanReversionStrategy strategy("SYN",StrategyConfig{},MeanReversionConfig{},nullptr);
    ASSERT_FALSE(strategy.on_execution(fill(Side::SELL,100,30,1)).is_error());
    ASSERT_FALSE(strategy.on_execution(fill(Side::BUY,25,20,2)).is_error());
    EXPECT_DOUBLE_EQ(strategy.get_positions().at("SYN").average_price.as_double(),30);
    ASSERT_FALSE(strategy.on_execution(fill(Side::BUY,100,10,3)).is_error());
    const auto& position=strategy.get_positions().at("SYN");
    EXPECT_DOUBLE_EQ(position.quantity.as_double(),25);
    EXPECT_DOUBLE_EQ(position.average_price.as_double(),10);
    EXPECT_DOUBLE_EQ(position.realized_pnl.as_double(),1750);
    EXPECT_DOUBLE_EQ(strategy.get_metrics().realized_pnl,1744);
}

TEST(EquityModelV4, ActualMeanReversionPartialFillUsesWeightedShareBasis) {
    ComponentCleanup cleanup;
    MeanReversionStrategy strategy("SYN",StrategyConfig{},MeanReversionConfig{},nullptr);
    ASSERT_FALSE(strategy.on_execution(fill(Side::BUY,1.5,10,1)).is_error());
    ASSERT_FALSE(strategy.on_execution(fill(Side::BUY,.5,14,1)).is_error());
    EXPECT_DOUBLE_EQ(strategy.get_positions().at("SYN").quantity.as_double(),2);
    EXPECT_DOUBLE_EQ(strategy.get_positions().at("SYN").average_price.as_double(),11);
    EXPECT_DOUBLE_EQ(strategy.get_positions().at("SYN").realized_pnl.as_double(),0);
}

TEST(EquityModelV4, LegacyBaseStrategyDefaultExecutionRemainsUnchanged) {
    ComponentCleanup cleanup;
    BaseStrategy strategy("legacy",StrategyConfig{},nullptr);
    ASSERT_FALSE(strategy.on_execution(fill(Side::BUY,100,10,1)).is_error());
    ASSERT_FALSE(strategy.on_execution(fill(Side::SELL,150,30,2)).is_error());
    EXPECT_DOUBLE_EQ(strategy.get_positions().at("SYN").realized_pnl.as_double(),2997);
    EXPECT_DOUBLE_EQ(strategy.get_metrics().realized_pnl,2997);
}

TEST(EquityModelV4, ActualBackwardFactorsCombineSplitDividendAndNewestAnchor) {
    const auto factors=market_data_utils::compute_backward_adjustment_factors({
        {100,0,1},{50,0,2},{49,1,1}});
    ASSERT_EQ(factors.size(),3);
    EXPECT_DOUBLE_EQ(factors[2],1);
    EXPECT_NEAR(factors[1],.98,1e-14);
    EXPECT_NEAR(factors[0],.49,1e-14);
}

TEST(EquityModelV4, ActualBackwardFactorsDoNotInventEventsOrBars) {
    EXPECT_TRUE(market_data_utils::compute_backward_adjustment_factors({}).empty());
    EXPECT_EQ(market_data_utils::compute_backward_adjustment_factors({{12,0,1}}),std::vector<double>{1});
    EXPECT_EQ(market_data_utils::compute_backward_adjustment_factors({{10,0,1},{0,3,2}}),
        (std::vector<double>{1,1}));
}
