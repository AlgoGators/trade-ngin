// The trading-day calendar of the per-contract roll rule and the exchange rules ported from step 0's
// r0lib.py (HD 2026-10-23, ruling 1): Monday to Friday less the exchange holidays; a Saturday (MBT)
// or Sunday bar never counts; outside the holiday file's coverage the calendar fails closed. The
// last trading day and first notice day per root family are checked against published exchange dates.
#include <gtest/gtest.h>

#include <set>

#include "contract_rolls_test_util.hpp"

using namespace trade_ngin::contract_rolls;
using contract_rolls_test::d;

namespace {

TradingCalendar cal() {
    const std::string path = contract_rolls_test::shipped_holidays_path();
    if (path.empty()) ADD_FAILURE() << "shipped holidays.json not found from the working directory";
    return contract_rolls_test::shipped_calendar();
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
        EXPECT_EQ(ymd(ExchangeRules::last_trading_day(k.root, k.y, k.m, c)), k.ltd)
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
