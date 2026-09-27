#include "trade_ngin/live/execution_manager.hpp"
#include <cmath>
#include "trade_ngin/apps/book_execution_phase.hpp"
#include <gtest/gtest.h>
#include <limits>
using namespace trade_ngin;
using namespace trade_ngin::transaction_cost;
namespace {
AssetCostConfig future() {
    AssetCostConfig c;c.symbol="SYN";c.tick_size=.25;c.point_value=50;
    c.baseline_spread_ticks=2;c.min_spread_ticks=1;c.max_spread_ticks=4;
    return c;
}
AssetCostConfig equity() {
    auto c=future();c.asset_type=AssetType::EQUITY;c.tick_size=.01;c.point_value=1;
    c.tick_constrained=false;c.commission_per_unit=.005;c.min_commission_per_order=1;
    c.max_commission_per_order=100;c.max_commission_pct=.01;
    c.apply_regulatory_fees=false;return c;
}
TransactionCostManager manager() {
    TransactionCostManager m;m.register_asset_config(future());m.register_asset_config(equity());
    m.update_market_data("SYN",1000000,100,99);return m;
}
void same_charge(const BookExecutionCharge& result,const TransactionCostResult& raw) {
    EXPECT_DOUBLE_EQ(result.raw.commissions_fees,raw.commissions_fees);
    EXPECT_DOUBLE_EQ(result.raw.implicit_price_impact,raw.implicit_price_impact);
    EXPECT_DOUBLE_EQ(result.raw.slippage_market_impact,raw.slippage_market_impact);
    EXPECT_DOUBLE_EQ(result.raw.total_transaction_costs,raw.total_transaction_costs);
    EXPECT_EQ(result.commissions_fees,Decimal(raw.commissions_fees));
    EXPECT_EQ(result.implicit_price_impact,Decimal(raw.implicit_price_impact));
    EXPECT_EQ(result.slippage_market_impact,Decimal(raw.slippage_market_impact));
    EXPECT_EQ(result.total_transaction_costs,Decimal(raw.total_transaction_costs));
}
}
TEST(BookExecutionPhase, ModelFuturesRetainsActualTrackedLookupAndLegacyRounding) {
    auto m=manager();CostChargeObservation used,baseline;
    const auto expected=m.calculate_costs("SYN",3,100,&baseline);
    auto actual=charge_book_execution(m,ModelTrackedExecutionCharge{"SYN",3,100},&used);
    ASSERT_TRUE(actual.is_ok());same_charge(actual.value(),expected);
    EXPECT_EQ(used.input_source,CostInputSource::internally_tracked);
    EXPECT_EQ(used.quantity,baseline.quantity);EXPECT_EQ(used.point_value,baseline.point_value);
}
TEST(BookExecutionPhase, ModelFallbackIsPreservedForItsLegacyPolicy) {
    TransactionCostManager m;CostChargeObservation used;
    auto actual=charge_book_execution(m,ModelTrackedExecutionCharge{"UNKNOWN",2,100},&used);
    ASSERT_TRUE(actual.is_ok());same_charge(actual.value(),m.calculate_costs("UNKNOWN",2,100));
    EXPECT_EQ(used.asset_lookup.path,AssetLookupPath::fallback);
}
TEST(BookExecutionPhase, ModelEquityUsesTypedCostForSameSymbolAsFutures) {
    auto m=manager();CostChargeObservation used;
    auto actual=charge_book_execution(m,ModelTrackedExecutionCharge{"SYN",.5,100,AssetType::EQUITY},&used);
    ASSERT_TRUE(actual.is_ok());same_charge(actual.value(),m.calculate_costs("SYN",.5,100,AssetType::EQUITY));
    EXPECT_EQ(used.point_value,1);EXPECT_EQ(used.asset_lookup.path,AssetLookupPath::exact_symbol);
}
TEST(BookExecutionPhase, GovernedFuturesKeepsSignedDeltaAndExplicitState) {
    auto m=manager();CostChargeObservation used;
    const GovernedExecutionCharge input{"SYN",Decimal(-2),100,123456,1.2,AssetType::FUTURE};
    auto actual=charge_book_execution(m,input,&used);ASSERT_TRUE(actual.is_ok());
    same_charge(actual.value(),m.calculate_costs("SYN",-2,100,123456,1.2));
    EXPECT_EQ(used.input_source,CostInputSource::explicit_values);
    EXPECT_EQ(used.quantity,-2);EXPECT_EQ(used.adv_argument,123456);EXPECT_EQ(used.volatility_multiplier_argument,1.2);
    EXPECT_EQ(input.signed_quantity,Decimal(-2));
}
TEST(BookExecutionPhase, GovernedEquityPreservesOneAtomAndFullTypedCharge) {
    auto m=manager();CostChargeObservation used;
    const GovernedExecutionCharge input{"SYN",Decimal::from_raw(-1),100,1000000,1,AssetType::EQUITY};
    auto actual=charge_book_execution(m,input,&used);ASSERT_TRUE(actual.is_ok());
    same_charge(actual.value(),m.calculate_costs("SYN",-1e-8,100,1000000,1,AssetType::EQUITY));
    EXPECT_EQ(input.signed_quantity.raw_value(),-1);ASSERT_TRUE(used.quantity.has_value());EXPECT_DOUBLE_EQ(*used.quantity,-1e-8);
    EXPECT_EQ(used.point_value,1);EXPECT_EQ(used.asset_lookup.path,AssetLookupPath::exact_symbol);
}
TEST(BookExecutionPhase, GovernedFractionalSaleDoesNotUseFuturesCommission) {
    auto m=manager();CostChargeObservation used;
    auto actual=charge_book_execution(m,GovernedExecutionCharge{"SYN",Decimal(-.5),14,1000000,1,AssetType::EQUITY},&used);
    ASSERT_TRUE(actual.is_ok());same_charge(actual.value(),m.calculate_costs("SYN",-.5,14,1000000,1,AssetType::EQUITY));
    EXPECT_EQ(actual.value().commissions_fees,Decimal(.07));
    EXPECT_NE(actual.value().commissions_fees,Decimal(.75));
}
TEST(BookExecutionPhase, GovernedZeroIsACarryAndCannotBecomeExecutionCost) {
    auto m=manager();EXPECT_TRUE(charge_book_execution(m,GovernedExecutionCharge{"SYN",Decimal(0),100,1000000,1,AssetType::EQUITY}).is_error());
}
TEST(BookExecutionPhase, GovernedNonfinitePriceCannotBecomeFallback) {
    auto m=manager();EXPECT_TRUE(charge_book_execution(m,GovernedExecutionCharge{"SYN",Decimal(1),std::numeric_limits<double>::infinity(),1000000,1,AssetType::EQUITY}).is_error());
}
TEST(BookExecutionPhase, GovernedMissingEquityLookupCannotUseFuturesWithSameSymbol) {
    TransactionCostManager m;m.register_asset_config(future());
    EXPECT_TRUE(charge_book_execution(m,GovernedExecutionCharge{"SYN",Decimal(1),100,1000000,1,AssetType::EQUITY}).is_error());
}
TEST(BookExecutionPhase, GovernedFuturesCannotRoundFractionalContracts) {
    auto m=manager();EXPECT_TRUE(charge_book_execution(m,GovernedExecutionCharge{"SYN",Decimal(.5),100,1000000,1,AssetType::FUTURE}).is_error());
}
TEST(BookExecutionPhase, GovernedMissingFuturesLookupCannotUseModelFallback) {
    TransactionCostManager m;m.register_asset_config(equity());
    EXPECT_TRUE(charge_book_execution(m,GovernedExecutionCharge{"SYN",Decimal(1),100,1000000,1,AssetType::FUTURE}).is_error());
}
class GovernedExecutionState : public testing::TestWithParam<int> {};
TEST_P(GovernedExecutionState, MissingOrNonfiniteExplicitStateCannotBecomeDefaults) {
    auto m=manager();GovernedExecutionCharge input{"SYN",Decimal(1),100,1000000,1,AssetType::EQUITY};
    const auto nan=std::numeric_limits<double>::quiet_NaN();
    switch(GetParam()) {
        case 0:input.reference_price=0;break;
        case 1:input.adv=0;break;
        case 2:input.volatility_multiplier=0;break;
        case 3:input.adv=nan;break;
        case 4:input.volatility_multiplier=nan;break;
    }
    EXPECT_TRUE(charge_book_execution(m,input).is_error());
}
INSTANTIATE_TEST_SUITE_P(StrictState,GovernedExecutionState,testing::Values(0,1,2,3,4));
TEST(BookExecutionPhase, UnsupportedAssetCannotSelectAnImplicitPolicy) {
    auto m=manager();EXPECT_TRUE(charge_book_execution(m,GovernedExecutionCharge{"SYN",Decimal(1),100,1000000,1,static_cast<AssetType>(-1)}).is_error());
}
TEST(BookExecutionPhase, CashOutsideDecimalRangeRefusesBeforeIntegerConversion) {
    auto m=manager();EXPECT_TRUE(charge_book_execution(m,GovernedExecutionCharge{"SYN",Decimal(1),1e300,1,1,AssetType::EQUITY}).is_error());
}
TEST(BookExecutionPhase, UnrepresentableAbsoluteQuantityCannotBecomeExecution) {
    auto m=manager();EXPECT_TRUE(charge_book_execution(m,GovernedExecutionCharge{"SYN",Decimal::from_raw(INT64_MIN),100,1000000,1,AssetType::EQUITY}).is_error());
}


TEST(BookExecutionPhase, ModelThrowingApiPreservesLegacyOverflowCategoryAndObservation) {
    TransactionCostManager::Config config;config.explicit_fee_per_contract=1e12;
    TransactionCostManager m(config);CostChargeObservation original,actual;
    const auto raw=m.calculate_costs("SYN",1,100,&original);
    ASSERT_TRUE(std::isfinite(raw.commissions_fees));
    ASSERT_THROW(Decimal(raw.commissions_fees),std::overflow_error);
    EXPECT_THROW(charge_model_book_execution(m,ModelTrackedExecutionCharge{"SYN",1,100},&actual),std::overflow_error);
    EXPECT_EQ(actual.quantity,original.quantity);
    EXPECT_EQ(actual.input_source,original.input_source);
    EXPECT_EQ(actual.effective_adv,original.effective_adv);
}
TEST(BookExecutionPhase, ModelThrowingApiPreservesInvalidDecimalMessage) {
    TransactionCostManager::Config config;config.explicit_fee_per_contract=std::numeric_limits<double>::quiet_NaN();
    TransactionCostManager m(config);
    const auto raw=m.calculate_costs("SYN",1,100);
    std::string original,actual;
    try { (void)Decimal(raw.commissions_fees); }
    catch(const std::invalid_argument& error){original=error.what();}
    ASSERT_FALSE(original.empty());
    try { (void)charge_model_book_execution(m,ModelTrackedExecutionCharge{"SYN",1,100}); }
    catch(const std::invalid_argument& error){actual=error.what();}
    EXPECT_EQ(actual,original);
}
TEST(BookExecutionPhase, ActualModelExecutionRetainsOverflowAndReachedCostTrace) {
    TransactionCostManager::Config config;config.explicit_fee_per_contract=1e12;
    ExecutionManager execution(config);ExecutionCallObservation call;
    const auto day=std::chrono::system_clock::time_point(std::chrono::sys_days(std::chrono::year{2026}/9/26));
    EXPECT_THROW(execution.generate_execution("SYN",1,100,day,0,"system",&call),std::overflow_error);
    EXPECT_EQ(call.state,ExecutionCallState::cost_call_reached);
    Position position;position.symbol="SYN";position.quantity=Decimal(1);position.average_price=Decimal(100);
    DailyExecutionObservation daily;
    EXPECT_THROW(execution.generate_daily_executions({{"SYN",position}},{},{{"SYN",100}},day,"system",&daily),std::overflow_error);
    EXPECT_NE(daily.state,DailyExecutionState::invalid_argument);
}
