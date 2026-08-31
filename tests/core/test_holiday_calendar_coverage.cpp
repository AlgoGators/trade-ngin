#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "trade_ngin/core/holiday_checker.hpp"

using namespace trade_ngin;

// The calendar answers for a finite set of years. Before this was modelled,
// `is_holiday` returned false for every date outside that range, which reads
// identically to "the market was open" -- so a backtest or replay of an
// uncovered year silently treated every closure as a trading day. These tests
// pin the distinction between "not a holiday" and "cannot answer", and pin the
// generator rules that produce the shipped file
// (see scripts/generate_market_holidays.py).

namespace {

// Writes a throwaway calendar so coverage assertions do not depend on how many
// years the shipped file happens to span.
class TempCalendar {
public:
    explicit TempCalendar(const std::string& json) {
        path_ = std::filesystem::temp_directory_path() /
                ("holiday_coverage_" + std::to_string(::getpid()) + ".json");
        std::ofstream f(path_);
        f << json;
    }
    ~TempCalendar() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

std::string shipped_calendar_path() {
    for (const auto* p : {"include/trade_ngin/core/holidays.json",
                          "../include/trade_ngin/core/holidays.json",
                          "../../include/trade_ngin/core/holidays.json"}) {
        if (std::filesystem::exists(p)) return p;
    }
    return "";
}

}  // namespace

// "Cannot answer" must be distinguishable from "not a holiday". Without
// covers_date the two are the same value and the caller cannot tell them apart.
TEST(HolidayCalendarCoverage, OutOfRangeIsDistinguishableFromNotAHoliday) {
    TempCalendar cal(R"({"2026":[{"date":"2026-12-25","name":"Christmas Day","type":"fixed"}]})");
    HolidayChecker checker(cal.path());

    EXPECT_TRUE(checker.covers_year(2026));
    EXPECT_TRUE(checker.covers_date("2026-07-04"));
    EXPECT_FALSE(checker.is_holiday("2026-07-04")) << "covered year, genuinely not a closure";

    EXPECT_FALSE(checker.covers_year(2019));
    EXPECT_FALSE(checker.covers_date("2019-12-25"));
    EXPECT_FALSE(checker.is_holiday("2019-12-25"))
        << "uncovered: still false, but covers_date is what tells the caller it is unknown";
}

TEST(HolidayCalendarCoverage, CoverageYearsReflectTheLoadedFile) {
    TempCalendar cal(R"({"2024":[],"2025":[{"date":"2025-01-01","name":"New Year's Day","type":"fixed"}]})");
    HolidayChecker checker(cal.path());

    ASSERT_EQ(checker.coverage_years().size(), 2u);
    EXPECT_EQ(*checker.coverage_years().begin(), 2024);
    EXPECT_EQ(*checker.coverage_years().rbegin(), 2025);
    EXPECT_EQ(checker.coverage_description(), "2024-2025");
}

TEST(HolidayCalendarCoverage, MalformedDatesDoNotClaimCoverage) {
    TempCalendar cal(R"({"2026":[{"date":"2026-12-25","name":"Christmas Day","type":"fixed"}]})");
    HolidayChecker checker(cal.path());

    EXPECT_FALSE(checker.covers_date(""));
    EXPECT_FALSE(checker.covers_date("26"));
    EXPECT_FALSE(checker.covers_date("not-a-date"));
}

// The shipped calendar must span the windows we actually run: historical
// backtests reach back to the start of the equity data, and live needs runway.
TEST(HolidayCalendarCoverage, ShippedCalendarSpansBacktestHistoryAndRunway) {
    const std::string path = shipped_calendar_path();
    if (path.empty()) GTEST_SKIP() << "shipped holidays.json not reachable from cwd";
    HolidayChecker checker(path);

    EXPECT_TRUE(checker.covers_year(2021)) << "the 5-year backtest window starts here";
    EXPECT_TRUE(checker.covers_year(2024)) << "pre-2025 replays were previously blind";
    EXPECT_TRUE(checker.covers_year(2026)) << "current live year";
    EXPECT_TRUE(checker.covers_year(2030)) << "runway beyond the old 2027 cliff";
}

// Rules the generator encodes, pinned against the shipped file so a
// regeneration that breaks one of them fails here rather than in a live run.
TEST(HolidayCalendarCoverage, ShippedCalendarEncodesTheObservanceRules) {
    const std::string path = shipped_calendar_path();
    if (path.empty()) GTEST_SKIP() << "shipped holidays.json not reachable from cwd";
    HolidayChecker checker(path);

    // Juneteenth became an NYSE closure in 2022; it must not appear earlier.
    EXPECT_FALSE(checker.is_holiday("2021-06-18"));
    EXPECT_FALSE(checker.is_holiday("2021-06-19"));
    EXPECT_TRUE(checker.is_holiday("2022-06-20")) << "first NYSE observance";

    // Saturday holidays are observed the preceding Friday.
    EXPECT_TRUE(checker.is_holiday("2026-07-03")) << "July 4 2026 is a Saturday";
    EXPECT_FALSE(checker.is_holiday("2026-07-04"));

    // Sunday holidays are observed the following Monday.
    EXPECT_TRUE(checker.is_holiday("2027-07-05")) << "July 4 2027 is a Sunday";

    // NYSE exception: New Year's Day on a Saturday closes nothing -- the
    // preceding December 31 stays open.
    EXPECT_FALSE(checker.is_holiday("2021-12-31")) << "Jan 1 2022 is a Saturday";
    EXPECT_FALSE(checker.is_holiday("2022-01-01"));

    // Good Friday is a market closure though not a federal holiday.
    EXPECT_TRUE(checker.is_holiday("2024-03-29"));

    // Federal holidays the equity markets trade through must NOT be closures.
    EXPECT_FALSE(checker.is_holiday("2024-10-14")) << "Columbus Day";
    EXPECT_FALSE(checker.is_holiday("2024-11-11")) << "Veterans Day";
}

// Unscheduled closures are not rule-derivable and are carried explicitly.
TEST(HolidayCalendarCoverage, ShippedCalendarCarriesAdHocClosures) {
    const std::string path = shipped_calendar_path();
    if (path.empty()) GTEST_SKIP() << "shipped holidays.json not reachable from cwd";
    HolidayChecker checker(path);

    EXPECT_TRUE(checker.is_holiday("2012-10-29")) << "Hurricane Sandy";
    EXPECT_TRUE(checker.is_holiday("2012-10-30")) << "Hurricane Sandy";
    EXPECT_TRUE(checker.is_holiday("2001-09-11")) << "September 11 closure";
    EXPECT_TRUE(checker.is_holiday("2025-01-09")) << "National Day of Mourning (Carter)";
}
