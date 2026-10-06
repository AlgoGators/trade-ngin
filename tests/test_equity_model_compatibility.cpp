#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <chrono>
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
#include "trade_ngin/live/execution_manager.hpp"

using namespace trade_ngin;

namespace {
struct HostTimezone {
    bool existed = std::getenv("TZ") != nullptr;
    std::string original = existed ? std::getenv("TZ") : "";
    explicit HostTimezone(const char* zone) { setenv("TZ",zone,1);tzset(); }
    ~HostTimezone() { if(existed)setenv("TZ",original.c_str(),1);else unsetenv("TZ");tzset(); }
};
struct OwnedCalendar {
    std::filesystem::path directory;
    OwnedCalendar() {
        directory=std::filesystem::temp_directory_path()/
            ("qt-equity-model-calendar-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if(!std::filesystem::create_directory(directory)) throw std::runtime_error("exclusive calendar directory required");
    }
    void write(const std::string& bytes) { std::ofstream(directory/"calendar.json")<<bytes; }
    std::string path() const { return (directory/"calendar.json").string(); }
    ~OwnedCalendar() { std::filesystem::remove(directory/"calendar.json");std::filesystem::remove(directory); }
};
}

TEST(EquityModelCompat, UtcDateDoesNotShiftWithHostTimezone) {
    HostTimezone zone("Asia/Tokyo");
    Timestamp stamp;
    ASSERT_TRUE(core::parse_utc_date("2026-09-25", stamp));
    EXPECT_EQ(core::format_utc_date(stamp), "2026-09-25");
    EXPECT_EQ(core::format_utc_datetime(stamp), "2026-09-25 00:00:00");
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(stamp.time_since_epoch()).count(), 1790294400);
}

TEST(EquityModelCompat, PreviousSessionWalkUsesUtcCalendarRatherThanHostDate) {
    HostTimezone zone("America/New_York");
    OwnedCalendar fixture;
    fixture.write(R"({"2026":[{"date":"2026-09-25","name":"Synthetic closure","type":"market"}]})");
    HolidayChecker checker(fixture.path());
    ASSERT_TRUE(checker.loaded());ASSERT_TRUE(checker.covers_date("2026-09-28"));
    Timestamp monday;ASSERT_TRUE(core::parse_utc_date("2026-09-28",monday));
    auto previous=checker.find_previous_trading_day(monday);
    ASSERT_TRUE(previous);EXPECT_EQ(core::format_utc_date(*previous),"2026-09-24");
}

TEST(EquityModelCompat, CalendarGapAndFailedReloadNeverClaimMissingCoverage) {
    OwnedCalendar fixture;
    fixture.write(R"({"2025":[],"2027":[]})");
    HolidayChecker checker(fixture.path());
    ASSERT_TRUE(checker.loaded());EXPECT_EQ(checker.coverage_years(),2u);
    EXPECT_FALSE(checker.covers_date("2026-09-25"));
    fixture.write(R"({"2026":[{"date":"2026-09-25"}]})");
    EXPECT_FALSE(checker.reload());EXPECT_TRUE(checker.loaded());
    EXPECT_TRUE(checker.covers_date("2025-09-25"));
    EXPECT_FALSE(checker.covers_date("2026-09-25"));
}

TEST(EquityModelCompat, PreviousSessionRefusesUncoveredYearAndCalendarGap) {
    OwnedCalendar fixture;fixture.write(R"({"2026":[],"2028":[]})");
    HolidayChecker checker(fixture.path());
    Timestamp start;ASSERT_TRUE(core::parse_utc_date("2026-01-01",start));
    EXPECT_FALSE(checker.find_previous_trading_day(start));
    ASSERT_TRUE(core::parse_utc_date("2027-07-06",start));
    EXPECT_FALSE(checker.find_previous_trading_day(start));
}

TEST(EquityModelCompat, PreviousSessionRefusesUnloadedCalendar) {
    OwnedCalendar fixture;fixture.write("not JSON");
    HolidayChecker checker(fixture.path());
    Timestamp start;ASSERT_TRUE(core::parse_utc_date("2026-09-25",start));
    EXPECT_FALSE(checker.loaded());EXPECT_FALSE(checker.find_previous_trading_day(start));
}

TEST(EquityModelCompat, CalendarRefusesPartialYearKeysAndMismatchedDates) {
    for(const char* bytes:{R"({"2026suffix":[]})",R"({"2026":{}})",
                          R"({"2026":[{"date":"2025-01-01","name":"Wrong year","type":"market"}]})",
                          R"({"2026":[{"date":"2026-02-31","name":"Invalid date","type":"market"}]})"}) {
        OwnedCalendar fixture;fixture.write(bytes);
        HolidayChecker checker(fixture.path());
        EXPECT_FALSE(checker.loaded());EXPECT_EQ(checker.coverage_years(),0u);
        EXPECT_FALSE(checker.covers_date("2026-09-25"));
    }
}

TEST(EquityModelCompat, CalendarCoverageRequiresAnExactRealDate) {
    OwnedCalendar fixture;fixture.write(R"({"2026":[]})");
    HolidayChecker checker(fixture.path());ASSERT_TRUE(checker.loaded());
    for(const char* date:{"2026", "2026junk", "2026-02-31", "2026-09-25tail", "2026-9-25"})
        EXPECT_FALSE(checker.covers_date(date));
    EXPECT_TRUE(checker.covers_date("2026-09-25"));
}

TEST(EquityModelCompat, RegisteredCalendarIsHeldByAtomicSharedOwnership) {
    OwnedCalendar fixture;fixture.write(R"({"2026":[]})");
    auto checker=std::make_shared<HolidayChecker>(fixture.path());
    EquityInstrument::set_holiday_checker(checker);
    std::weak_ptr<HolidayChecker> weak=checker;checker.reset();
    EXPECT_FALSE(weak.expired());
    EXPECT_TRUE(EquityInstrument::get_holiday_checker()->covers_date("2026-09-25"));
    EquityInstrument::set_holiday_checker(nullptr);
    EXPECT_TRUE(weak.expired());
}

TEST(EquityModelCompat, DateParserRefusesTrailingTextWithoutChangingOutput) {
    Timestamp stamp = std::chrono::system_clock::from_time_t(123);
    EXPECT_FALSE(core::parse_utc_date("2026-09-25 00:00:00", stamp));
    EXPECT_EQ(std::chrono::system_clock::to_time_t(stamp), 123);
}

TEST(EquityModelCompat, StrategyBasisWinsAndHeldBasisSurvives) {
    EXPECT_DOUBLE_EQ(LivePnLManager::resolve_day_t_cost_basis(12, 9), 12);
    EXPECT_DOUBLE_EQ(LivePnLManager::resolve_day_t_cost_basis(0, 9), 9);
    EXPECT_DOUBLE_EQ(LivePnLManager::resolve_day_t_cost_basis(-1, 0), 0);
}

TEST(EquityModelCompat, CostBasisUnrealizedKeepsSignedQuantityAndDoesNotInventBasis) {
    EXPECT_DOUBLE_EQ(LivePnLManager::unrealized_from_cost_basis(3, 10, 12), 6);
    EXPECT_DOUBLE_EQ(LivePnLManager::unrealized_from_cost_basis(-3, 10, 12), -6);
    EXPECT_DOUBLE_EQ(LivePnLManager::unrealized_from_cost_basis(3, 0, 12), 0);
    EXPECT_DOUBLE_EQ(LivePnLManager::unrealized_from_cost_basis(3, 10, 0), -30);
}

TEST(EquityModelCompat, EquityPointValueNeverUsesAnUnregisteredFuturesMultiplier) {
    auto& registry=InstrumentRegistry::instance();
    LivePnLManager manager(1000,registry);
    EXPECT_EQ(manager.get_asset_type(),AssetType::FUTURE);
    const double future=manager.get_point_value("MES");
    EXPECT_DOUBLE_EQ(future,5);
    manager.set_asset_type(AssetType::EQUITY);
    EXPECT_DOUBLE_EQ(manager.get_point_value("MES"),1);
    manager.set_asset_type(AssetType::FUTURE);
    EXPECT_DOUBLE_EQ(manager.get_point_value("MES"),future);
}

TEST(EquityModelCompat, EquityFinalizationRetainsTradeRealizedAndMarksTrueBasis) {
    LivePnLManager manager(1000,InstrumentRegistry::instance());
    manager.set_asset_type(AssetType::EQUITY);
    Position holding;holding.symbol="SYN";holding.quantity=3;holding.average_price=10;
    holding.realized_pnl=7;
    auto result=manager.finalize_previous_day({holding},{{"SYN",12}},{{"SYN",11}},1000,0,
        LivePnLManager::UnrealizedPolicy::MARK_TO_MARKET);
    ASSERT_FALSE(result.is_error());ASSERT_EQ(result.value().finalized_positions.size(),1);
    EXPECT_DOUBLE_EQ(result.value().finalized_positions[0].realized_pnl.as_double(),7);
    EXPECT_DOUBLE_EQ(result.value().finalized_positions[0].unrealized_pnl.as_double(),6);
    EXPECT_DOUBLE_EQ(result.value().finalized_unrealized_pnl,6);
    EXPECT_DOUBLE_EQ(result.value().finalized_daily_pnl,3);
}

TEST(EquityModelCompat, DefaultFuturesFinalizationKeepsSettlementIdentity) {
    LivePnLManager manager(1000,InstrumentRegistry::instance());
    Position holding;holding.symbol="SYN";holding.quantity=3;holding.average_price=10;
    holding.realized_pnl=7;
    auto result=manager.finalize_previous_day({holding},{{"SYN",12}},{{"SYN",11}},1000,0);
    ASSERT_FALSE(result.is_error());ASSERT_EQ(result.value().finalized_positions.size(),1);
    EXPECT_DOUBLE_EQ(result.value().finalized_positions[0].realized_pnl.as_double(),3);
    EXPECT_DOUBLE_EQ(result.value().finalized_positions[0].unrealized_pnl.as_double(),0);
    EXPECT_DOUBLE_EQ(result.value().finalized_unrealized_pnl,0);
}

TEST(EquityModelCompat, EquitySnapshotReportsTheSameUnrealizedAsItsCostBasis) {
    LivePnLManager manager(1000,InstrumentRegistry::instance());
    manager.set_asset_type(AssetType::EQUITY);
    Position holding;holding.symbol="SYN";holding.quantity=3;holding.average_price=10;
    auto result=manager.calculate_position_pnls({holding},{{"SYN",12}},{{"SYN",11}});
    ASSERT_FALSE(result.is_error());
    auto snapshot=manager.get_current_snapshot();ASSERT_FALSE(snapshot.is_error());
    EXPECT_DOUBLE_EQ(snapshot.value().unrealized_pnl,6);
    manager.set_asset_type(AssetType::FUTURE);
    EXPECT_DOUBLE_EQ(manager.get_current_snapshot().value().unrealized_pnl,0);
}

TEST(EquityModelCompat, ShortingRequiresBothRegTAndPermission) {
    EquitySpec spec; spec.exchange="NYSE";
    EXPECT_FALSE(EquityInstrument("SYN",spec).is_short_allowed());
    spec.short_selling_allowed=true;
    EXPECT_FALSE(EquityInstrument("SYN",spec).is_short_allowed());
    spec.account_mode=EquityAccountMode::REG_T;
    EXPECT_TRUE(EquityInstrument("SYN",spec).is_short_allowed());
    spec.short_selling_allowed=false;
    EXPECT_FALSE(EquityInstrument("SYN",spec).is_short_allowed());
}

TEST(EquityModelCompat, LoggerEmptySubdirectoryPreservesDefaultWireShape) {
    LoggerConfig config;
    EXPECT_TRUE(config.log_subdirectory.empty());
    EXPECT_FALSE(config.to_json().contains("log_subdirectory"));
    config.from_json({{"log_subdirectory","2026-09-25"}});
    EXPECT_EQ(config.log_subdirectory,"2026-09-25");
    EXPECT_EQ(config.to_json()["log_subdirectory"],"2026-09-25");
}

TEST(EquityModelCompat, StrictMissingPriceIsNotFilledAtCarriedCostBasis) {
    ExecutionManager manager;
    Position holding; holding.symbol="SYN";holding.quantity=3;holding.average_price=10;
    std::unordered_map<std::string,Position> current{{"SYN",holding}};
    std::vector<std::string> unpriced;
    auto result=manager.generate_daily_executions(current,{}, {},Timestamp{},PricingPolicy::STRICT,&unpriced);
    ASSERT_FALSE(result.is_error());
    EXPECT_TRUE(result.value().empty());
    EXPECT_EQ(unpriced,std::vector<std::string>{"SYN"});
}

TEST(EquityModelCompat, StrictCloseWithoutMarkIsReportedAndNotExecuted) {
    ExecutionManager manager;
    Position holding; holding.symbol="SYN";holding.quantity=3;holding.average_price=10;
    std::vector<std::string> unpriced;
    auto result=manager.generate_daily_executions({},{{"SYN",holding}}, {},Timestamp{},PricingPolicy::STRICT,&unpriced);
    ASSERT_FALSE(result.is_error());
    EXPECT_TRUE(result.value().empty());
    EXPECT_EQ(unpriced,std::vector<std::string>{"SYN"});
}

TEST(EquityModelCompat, EquityStrictUsesSignedTypedSellFeeAndUtcRunDate) {
    ExecutionManager manager;
    transaction_cost::AssetCostConfig costs;
    costs.symbol="SYN";costs.asset_type=AssetType::EQUITY;
    costs.commission_per_unit=0;costs.apply_regulatory_fees=true;
    costs.sec_fee_per_million=20;costs.finra_taf_per_share=.01;costs.finra_taf_cap_per_trade=100;
    manager.get_transaction_cost_manager().register_asset_config(costs);
    Timestamp stamp;ASSERT_TRUE(core::parse_utc_date("2026-09-25",stamp));
    Position holding;holding.symbol="SYN";holding.quantity=3;holding.average_price=10;
    auto sell=manager.generate_daily_executions({},{{"SYN",holding}},{{"SYN",10}},stamp,PricingPolicy::STRICT,nullptr);
    ASSERT_FALSE(sell.is_error());ASSERT_EQ(sell.value().size(),1);
    EXPECT_EQ(sell.value()[0].order_id,"DAILY_SYN_20260925");
    EXPECT_GT(sell.value()[0].commissions_fees.as_double(),0);
    auto buy=manager.generate_daily_executions({{"SYN",holding}},{},{{"SYN",10}},stamp,PricingPolicy::STRICT,nullptr);
    ASSERT_FALSE(buy.is_error());ASSERT_EQ(buy.value().size(),1);
    EXPECT_DOUBLE_EQ(buy.value()[0].commissions_fees.as_double(),0);
}
