#include <gtest/gtest.h>
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/live/live_price_manager.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

using namespace trade_ngin;
namespace {
struct ComponentCleanup { ~ComponentCleanup(){Logger::register_component("");} };
Timestamp day(const char* text) {Timestamp result;if(!core::parse_utc_date(text,result))throw std::runtime_error("invalid test day");return result;}
Bar bar(const char* date,double price) {Bar result;result.symbol="SYN";result.timestamp=day(date);result.close=price;return result;}
// Deterministic test strategy feeds a fixed share quantity into the real
// PortfolioManager loop. It neither implements rounding nor replaces MODEL.
class FixedShares final:public BaseStrategy {
public:
    int calls=0;
    explicit FixedShares(std::string id):BaseStrategy(std::move(id),StrategyConfig{},nullptr) {
        Position value;value.symbol="SYN";value.quantity=3.5;value.average_price=10;positions_["SYN"]=value;
    }
    Result<void> on_data(const std::vector<Bar>&,StrategyConsumptionTrace* =nullptr) override {
        ++calls;return Result<void>();
    }
};
}

TEST(EquityModelV5, ExplicitPriorSessionUsesFridayEvenWithMondaysBarPresent) {
    LivePriceManager manager(nullptr);
    ASSERT_FALSE(manager.update_from_bars({bar("2026-09-28",50),bar("2026-09-24",10),
        bar("2026-09-25",12)},day("2026-09-28"),day("2026-09-25")).is_error());
    ASSERT_FALSE(manager.get_previous_day_price("SYN").is_error());
    EXPECT_DOUBLE_EQ(manager.get_previous_day_price("SYN").value(),12);
    EXPECT_DOUBLE_EQ(manager.get_two_days_ago_price("SYN").value(),10);
    EXPECT_DOUBLE_EQ(manager.get_latest_price("SYN").value(),12);
}

TEST(EquityModelV5, MissingResolvedPriorSessionIsNeverSubstituted) {
    LivePriceManager manager(nullptr);
    ASSERT_FALSE(manager.update_from_bars({bar("2026-09-24",10),bar("2026-09-28",50)},
        day("2026-09-28"),day("2026-09-25")).is_error());
    EXPECT_TRUE(manager.get_previous_day_price("SYN").is_error());
    // The pinned reference permits an earlier available T-2, while never
    // substituting it for the missing, caller-resolved T-1.
    EXPECT_DOUBLE_EQ(manager.get_two_days_ago_price("SYN").value(),10);
}

TEST(EquityModelV5, ExistingTwoArgumentPriceSelectionRetainsItsCalendarDayRule) {
    LivePriceManager manager(nullptr);
    ASSERT_FALSE(manager.update_from_bars({bar("2026-09-24",10),bar("2026-09-25",12),
        bar("2026-09-28",50)},day("2026-09-28")).is_error());
    EXPECT_TRUE(manager.get_previous_day_price("SYN").is_error());
    EXPECT_DOUBLE_EQ(manager.get_latest_price("SYN").value(),50);
    EXPECT_DOUBLE_EQ(manager.get_two_days_ago_price("SYN").value(),12);
}

TEST(EquityModelV5, DefaultFuturesPolicySerializationRetainsExistingShape) {
    LiveSpecificConfig live;
    EXPECT_EQ(live.to_json(),nlohmann::json({{"historical_days",300}}));
    PortfolioConfig portfolio;
    EXPECT_FALSE(portfolio.allow_fractional_positions);
    EXPECT_FALSE(portfolio.to_json().contains("allow_fractional_positions"));
}

TEST(EquityModelV5, ActualEquityPolicySnapshotRecordsParsedAndDefaultedValues) {
    LiveSpecificConfig live;
    live.from_json({{"execution_price_max_staleness_days",2},{"spinoff_child_policy","hold"}});
    live.record_equity_policy_snapshot();
    EXPECT_EQ(live.to_json(),nlohmann::json({{"historical_days",300},
        {"data_staleness_tolerance_days",4},{"execution_price_max_staleness_days",2},
        {"spinoff_child_policy","hold"}}));
}

TEST(EquityModelV5, FractionalOptInAcceptsTheActualManagerFirstPassWithoutRounding) {
    ComponentCleanup cleanup;
    PortfolioConfig config;config.total_capital=1000;config.allow_fractional_positions=true;
    PortfolioManager manager(config,"EQ_COMPAT_FRACTIONAL");
    auto strategy=std::make_shared<FixedShares>("EQ_COMPAT_FRACTIONAL_STRATEGY");
    ASSERT_FALSE(manager.add_strategy(strategy,1,false,false).is_error());
    PortfolioConsumptionTrace trace;
    ASSERT_FALSE(manager.process_market_data({bar("2026-09-25",10)},true,std::nullopt,&trace).is_error());
    EXPECT_EQ(strategy->calls,1);
    EXPECT_EQ(trace.pass_count,1);
    EXPECT_DOUBLE_EQ(manager.get_strategy_positions().at(strategy->get_metadata().id).at("SYN").quantity.as_double(),3.5);
}

TEST(EquityModelV5, DefaultManagerStillRoundsWholeContractsAfterItsExistingLoop) {
    ComponentCleanup cleanup;
    PortfolioConfig config;config.total_capital=1000;
    PortfolioManager manager(config,"FUT_COMPAT_FRACTIONAL");
    auto strategy=std::make_shared<FixedShares>("FUT_COMPAT_FRACTIONAL_STRATEGY");
    ASSERT_FALSE(manager.add_strategy(strategy,1,false,false).is_error());
    PortfolioConsumptionTrace trace;
    ASSERT_FALSE(manager.process_market_data({bar("2026-09-25",10)},true,std::nullopt,&trace).is_error());
    // Strategy invocation precedes the manager loop; count actual loop entries.
    EXPECT_EQ(strategy->calls,1);
    EXPECT_EQ(trace.pass_count,5);
    EXPECT_DOUBLE_EQ(manager.get_strategy_positions().at(strategy->get_metadata().id).at("SYN").quantity.as_double(),4);
}
