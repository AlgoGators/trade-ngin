// The trading-day calendar of the per-contract roll rule and the exchange rules ported from step 0's
// r0lib.py (HD 2026-10-23, ruling 1): Monday to Friday less the exchange holidays; a Saturday (MBT)
// or Sunday bar never counts; outside the holiday file's coverage the calendar fails closed. The
// last trading day and first notice day per root family are checked against published exchange dates.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>

#include <unistd.h>

#include "contract_rolls_test_util.hpp"

using namespace trade_ngin::contract_rolls;
using contract_rolls_test::d;

namespace {

TradingCalendar cal(const std::string& root = "GC") {
    const std::string path = contract_rolls_test::shipped_futures_calendar_path();
    if (path.empty()) ADD_FAILURE() << "shipped futures_holidays.json not found from the working directory";
    return contract_rolls_test::shipped_calendar(root);
}

}  // namespace

TEST(RollCalendar, WeekendsAndHolidaysAreNotTradingDays) {
    const TradingCalendar c = cal();
    EXPECT_TRUE(c.is_trading_day(d("2026-08-28")));   // Friday
    EXPECT_FALSE(c.is_trading_day(d("2026-08-29")));  // Saturday: MBT prints, never a trading day
    EXPECT_FALSE(c.is_trading_day(d("2026-08-30")));  // Sunday: the Globex opening, never one
    EXPECT_FALSE(c.is_trading_day(d("2025-12-25")));  // Christmas
    EXPECT_FALSE(c.is_trading_day(d("2026-01-19")));  // MLK: a Globex holiday session
    EXPECT_FALSE(c.is_trading_day(d("2023-06-19")));  // Juneteenth, from 2022
    EXPECT_TRUE(c.is_trading_day(d("2021-06-18")));   // before Juneteenth was a holiday
}

TEST(RollCalendar, CountsTradingDaysAcrossAHolidayAndAWeekend) {
    const TradingCalendar c = cal();
    // Friday 2025-12-26: the day before is Wednesday 12-24 (Christmas between).
    EXPECT_EQ(c.trading_day_before(d("2025-12-26")), d("2025-12-24"));
    // Tuesday 2026-01-20, five trading days back: 01-16, 01-15, 01-14, 01-13, 01-12 (MLK Monday
    // 01-19 and the weekend skipped).
    EXPECT_EQ(c.trading_day_before(d("2026-01-20"), 5), d("2026-01-12"));
    // From a Saturday or Sunday the first trading day on or after is Monday.
    EXPECT_EQ(c.trading_day_on_or_after(d("2026-08-29")), d("2026-08-31"));
    EXPECT_EQ(c.trading_day_on_or_after(d("2026-08-30")), d("2026-08-31"));
    EXPECT_EQ(c.last_trading_day_of_month(2025, 12), d("2025-12-31"));
    EXPECT_EQ(c.nth_trading_day_of_month(2025, 12, 10), d("2025-12-12"));
}

TEST(RollCalendar, FailsClosedOutsideCoverage) {
    const TradingCalendar c([](const std::string&) { return false; },
                            [](const std::string& s) { return s.substr(0, 4) == "2026"; });
    EXPECT_TRUE(c.is_trading_day(d("2026-03-02")));
    EXPECT_THROW(c.is_trading_day(d("2027-01-04")), CalendarCoverageError);
    // A walk that crosses out of coverage throws too, rather than counting unknown days as open.
    EXPECT_THROW(c.trading_day_before(d("2026-01-01"), 1), CalendarCoverageError);
}

TEST(RollCalendar, ParsesAndPrintsDates) {
    EXPECT_EQ(ymd(d("2026-07-31")), "2026-07-31");
    EXPECT_THROW(parse_ymd("2026-7-31"), std::invalid_argument);
    EXPECT_THROW(parse_ymd("2026-02-30"), std::invalid_argument);
    EXPECT_THROW(parse_ymd("2026x07-31"), std::invalid_argument);
}

// Published exchange dates, one or more per root family.
TEST(ExchangeRules, LastTradingDayPerRootFamily) {
    const TradingCalendar c = cal();
    struct Case {
        const char* root;
        int y;
        unsigned m;
        const char* ltd;
    };
    const Case cases[] = {
        {"GC", 2024, 2, "2024-02-27"},   // third-last trading day of the month (R0 GC 2024)
        {"GC", 2025, 12, "2025-12-29"},
        {"6N", 2025, 6, "2025-06-16"},   // two trading days before the third Wednesday
        {"6E", 2023, 6, "2023-06-16"},   // ... across Juneteenth
        {"6C", 2026, 3, "2026-03-17"},   // one trading day before the third Wednesday
        {"6L", 2026, 7, "2026-06-30"},   // last trading day of the prior month
        {"CL", 2015, 8, "2015-07-21"},   // three trading days before the 25th of the prior month
        {"CL", 2026, 1, "2025-12-19"},   // ... four when the 25th is not one (Christmas)
        {"NG", 2026, 1, "2025-12-29"},   // three trading days before the month
        {"HO", 2025, 8, "2025-07-31"},   // last trading day of the prior month
        {"RB", 2026, 7, "2026-06-30"},
        {"HG", 2018, 5, "2018-05-29"},
        {"ZC", 2026, 3, "2026-03-13"},   // the trading day before the 15th
        {"ZN", 2025, 12, "2025-12-19"},  // seven trading days before the last of the month
        {"ZF", 2025, 12, "2025-12-31"},  // last trading day of the month
        {"LE", 2025, 12, "2025-12-31"},
        {"HE", 2025, 12, "2025-12-12"},  // tenth trading day
        {"GF", 2025, 11, "2025-11-20"},  // the Thursday before Thanksgiving
        {"GF", 2026, 1, "2026-01-29"},   // last Thursday
        {"ES", 2025, 12, "2025-12-19"},  // third Friday
        {"MES", 2026, 3, "2026-03-20"},
        {"MBT", 2026, 8, "2026-08-28"},  // last Friday (R0 MBTQ6)
    };
    for (const auto& k : cases) {
        EXPECT_EQ(ymd(ExchangeRules::last_trading_day(k.root, k.y, k.m, cal(k.root))), k.ltd)
            << k.root << " " << k.y << "-" << k.m;
    }
    EXPECT_THROW(ExchangeRules::last_trading_day("XX", 2026, 1, c), std::invalid_argument);
    EXPECT_TRUE(ExchangeRules::knows("ZW"));
    EXPECT_FALSE(ExchangeRules::knows("XX"));
}

TEST(ExchangeRules, FirstNoticeDayPerRootFamily) {
    const TradingCalendar c = cal();
    // Last trading day of the prior month: metals, grains, Treasuries.
    EXPECT_EQ(ymd(*ExchangeRules::first_notice_day("GC", 2026, 8, c)), "2026-07-31");
    EXPECT_EQ(ymd(*ExchangeRules::first_notice_day("GC", 2024, 2, c)), "2024-01-31");
    EXPECT_EQ(ymd(*ExchangeRules::first_notice_day("HG", 2018, 5, c)), "2018-04-30");
    EXPECT_EQ(ymd(*ExchangeRules::first_notice_day("ZN", 2025, 12, c)), "2025-11-28");
    EXPECT_EQ(ymd(*ExchangeRules::first_notice_day("ZC", 2026, 3, c)), "2026-02-27");
    // Live cattle: the first trading day of the contract month.
    EXPECT_EQ(ymd(*ExchangeRules::first_notice_day("LE", 2025, 12, c)), "2025-12-01");
    // Cash settled, or delivery only after the last trade: none.
    for (const char* root : {"6E", "CL", "NG", "HO", "RB", "MBT", "ES", "MES", "HE", "GF", "6L"}) {
        EXPECT_FALSE(ExchangeRules::first_notice_day(root, 2026, 3, c).has_value()) << root;
    }
}

// R2 fix round 1 (HD 2026-10-23): the roll calendar is the futures exchange's own, per product group,
// from include/trade_ngin/core/futures_holidays.json, not the NYSE's.
namespace {

bool trades(const std::string& root, const std::string& date) {
    return cal(root).is_trading_day(d(date));
}

std::string write_calendar(const std::string& text) {
    const auto p = std::filesystem::temp_directory_path() /
                   ("r2_futures_cal_" + std::to_string(::getpid()) + ".json");
    std::ofstream(p) << text;
    return p.string();
}

}  // namespace

TEST(FuturesCalendar, MapsEveryTradedRootToItsExchangeAndGroup) {
    const auto& c = contract_rolls_test::shipped_calendars();
    EXPECT_EQ(c.exchange_of("ZC"), "CME");
    EXPECT_EQ(c.group_of("ZC"), "grains");
    EXPECT_EQ(c.group_of("MES"), "equity_index");
    EXPECT_EQ(c.group_of("ES"), "equity_index");  // the micros' alias reads ES on the same calendar
    EXPECT_EQ(c.group_of("MBT"), "crypto");
    EXPECT_EQ(c.group_of("XX"), "");
    EXPECT_TRUE(c.for_root("XX").is_error());
    for (const char* r : {"6A", "6B", "6C", "6E", "6J", "6L", "6M", "6N", "6S", "CL", "GC", "GF", "HE",
                          "HG", "HO", "KE", "LE", "M2K", "MBT", "MES", "MNQ", "MYM", "NG", "PL", "RB",
                          "SI", "UB", "ZC", "ZF", "ZL", "ZM", "ZN", "ZR", "ZS", "ZT", "ZW"}) {
        EXPECT_TRUE(c.for_root(r).is_ok()) << r;
    }
}

// MM (HD 2026-10-27): MGC and SIL are COMEX metals like GC and SI, on the metals group's calendar,
// with their full-size roots' last trading day (the third-last business day of the month) and first
// notice day (the last trading day of the prior month); their vendor expirations equal GC's of the
// same month (MM step 0).
TEST(FuturesCalendar, TheGoldAndSilverMicrosAreMetalsWithTheirFullSizeRootsRules) {
    const auto& c = contract_rolls_test::shipped_calendars();
    for (const char* r : {"MGC", "SIL"}) {
        EXPECT_EQ(c.exchange_of(r), "CME") << r;
        EXPECT_EQ(c.group_of(r), "metals") << r;
        ASSERT_TRUE(c.for_root(r).is_ok()) << r;
        EXPECT_TRUE(ExchangeRules::knows(r)) << r;
    }
    // Sandy (2012-10-29) traded for metals, unlike equity index.
    EXPECT_TRUE(trades("MGC", "2012-10-29"));
    const TradingCalendar metals = cal("GC");
    const std::pair<const char*, const char*> pairs[] = {{"MGC", "GC"}, {"SIL", "SI"}};
    for (const auto& [micro, full] : pairs) {
        for (int y = 2011; y <= 2027; ++y) {  // a first notice day reads the prior month: 2010-01 needs 2009
            for (unsigned m = 1; m <= 12; ++m) {
                EXPECT_EQ(ymd(ExchangeRules::last_trading_day(micro, y, m, metals)),
                          ymd(ExchangeRules::last_trading_day(full, y, m, metals)))
                    << micro << " " << y << "-" << m;
                ASSERT_TRUE(ExchangeRules::first_notice_day(micro, y, m, metals).has_value()) << micro;
                EXPECT_EQ(ymd(*ExchangeRules::first_notice_day(micro, y, m, metals)),
                          ymd(*ExchangeRules::first_notice_day(full, y, m, metals)))
                    << micro << " " << y << "-" << m;
            }
        }
    }
    // Published: MGCZ0 2010-12-29, MGCQ6 2026-08-27, SILN3 2013-07-29.
    EXPECT_EQ(ymd(ExchangeRules::last_trading_day("MGC", 2010, 12, metals)), "2010-12-29");
    EXPECT_EQ(ymd(ExchangeRules::last_trading_day("MGC", 2026, 8, metals)), "2026-08-27");
    EXPECT_EQ(ymd(ExchangeRules::last_trading_day("SIL", 2013, 7, metals)), "2013-07-29");
}

TEST(FuturesCalendar, HurricaneSandyDaysTradedExceptEquityIndex) {
    for (const char* day : {"2012-10-29", "2012-10-30"}) {
        for (const char* r : {"ZN", "GC", "CL", "ZC", "6E", "LE"}) EXPECT_TRUE(trades(r, day)) << r << " " << day;
        EXPECT_FALSE(trades("ES", day)) << day;  // equity index settlements carried forward
    }
}

TEST(FuturesCalendar, December5th2018TradedExceptRatesAndEquityIndex) {
    for (const char* r : {"GC", "CL", "ZC", "6E", "HE", "MBT"}) EXPECT_TRUE(trades(r, "2018-12-05")) << r;
    EXPECT_FALSE(trades("ZN", "2018-12-05"));
    EXPECT_FALSE(trades("ES", "2018-12-05"));
}

TEST(FuturesCalendar, January9th2025TradedExceptEquityIndex) {
    for (const char* r : {"ZN", "GC", "CL", "ZC", "6E", "LE", "MBT"}) EXPECT_TRUE(trades(r, "2025-01-09")) << r;
    EXPECT_FALSE(trades("MES", "2025-01-09"));
}

TEST(FuturesCalendar, EmploymentReportGoodFridaysSettleRatesAndFxOnly) {
    for (const char* day : {"2012-04-06", "2015-04-03", "2021-04-02", "2023-04-07", "2026-04-03"}) {
        EXPECT_TRUE(trades("ZN", day)) << day;
        EXPECT_TRUE(trades("6E", day)) << day;
        for (const char* r : {"MES", "GC", "CL", "ZC", "LE"}) EXPECT_FALSE(trades(r, day)) << r << " " << day;
    }
    // Bitcoin settled on the 2023 and 2026 ones, not on 2021's.
    EXPECT_FALSE(trades("MBT", "2021-04-02"));
    EXPECT_TRUE(trades("MBT", "2023-04-07"));
    EXPECT_TRUE(trades("MBT", "2026-04-03"));
    // A Good Friday without the report closes every group.
    for (const char* r : {"ZN", "6E", "MBT", "GC"}) EXPECT_FALSE(trades(r, "2025-04-18")) << r;
}

TEST(FuturesCalendar, ScheduledHolidaysCloseEveryGroup) {
    for (const char* r : {"ZN", "6E", "MES", "GC", "CL", "ZC", "LE", "MBT"}) {
        EXPECT_FALSE(trades(r, "2026-01-19")) << r;  // MLK: Globex to the halt, next trade date
        EXPECT_FALSE(trades(r, "2023-06-19")) << r;  // Juneteenth, from 2022
        EXPECT_TRUE(trades(r, "2021-06-18")) << r;   // before it
        EXPECT_FALSE(trades(r, "2021-12-24")) << r;  // Christmas on a Saturday, observed Friday
        EXPECT_TRUE(trades(r, "2021-12-31")) << r;   // New Year on a Saturday: no Friday closure
        EXPECT_TRUE(trades(r, "2025-11-28")) << r;   // day after Thanksgiving: early close, settled
    }
}

TEST(FuturesCalendar, ExpiriesOnTheExchangeCalendarMatchTheData) {
    // The three expiries the NYSE calendar got wrong (the last bars in contract_ohlcv_1d).
    EXPECT_EQ(ymd(ExchangeRules::last_trading_day("NG", 2012, 11, cal("NG"))), "2012-10-29");
    EXPECT_EQ(ymd(ExchangeRules::last_trading_day("PL", 2012, 10, cal("PL"))), "2012-10-29");
    EXPECT_EQ(ymd(ExchangeRules::last_trading_day("HE", 2018, 12, cal("HE"))), "2018-12-14");
}

TEST(FuturesCalendar, RefusesAMalformedFile) {
    EXPECT_TRUE(FuturesCalendar::load("/nonexistent/futures_holidays.json").is_error());
    const std::string base = R"({"exchanges": {"CME": {"covers": {"first_year": 2026, "last_year": 2026},
        "groups": {"a": ["XX"], "b": ["YY"]}, "closures": [CLOSURES]}}})";
    auto with = [&](const std::string& closures, const std::string& groups = "") {
        std::string t = base;
        t.replace(t.find("CLOSURES"), 8, closures);
        if (!groups.empty()) t.replace(t.find(R"("a": ["XX"], "b": ["YY"])"), 24, groups);
        const std::string p = write_calendar(t);
        auto r = FuturesCalendar::load(p);
        std::filesystem::remove(p);
        return r.is_ok();
    };
    EXPECT_TRUE(with(R"({"date": "2026-01-19", "closes": ["all"]})"));
    EXPECT_FALSE(with(R"({"date": "2026-01-19", "closes": ["c"]})"));        // unknown group
    EXPECT_FALSE(with(R"({"date": "2027-01-18", "closes": ["all"]})"));      // outside coverage
    EXPECT_FALSE(with("", R"("a": ["XX"], "b": ["XX"])"));                     // root twice
    EXPECT_FALSE(with(R"({"date": "2026-1-19", "closes": ["all"]})"));       // not a date
    // Group closures apply to their roots only; the coverage fails closed.
    const std::string p = write_calendar(
        R"({"exchanges": {"CME": {"covers": {"first_year": 2026, "last_year": 2026},
            "groups": {"a": ["XX"], "b": ["YY"]}, "closures": [{"date": "2026-01-19", "closes": ["a"]}]}}})");
    auto c = FuturesCalendar::load(p);
    std::filesystem::remove(p);
    ASSERT_TRUE(c.is_ok());
    EXPECT_FALSE(c.value().for_root("XX").value().is_trading_day(d("2026-01-19")));
    EXPECT_TRUE(c.value().for_root("YY").value().is_trading_day(d("2026-01-19")));
    EXPECT_THROW(c.value().for_root("YY").value().is_trading_day(d("2027-01-04")), CalendarCoverageError);
}
