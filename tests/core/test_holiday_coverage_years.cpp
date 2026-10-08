// tests/core/test_holiday_coverage_years.cpp
//
// `HolidayChecker::coverage_years()` -- the count, asserted.
//
// `coverage_description()` prints "first-last" and READS as a range without being one. A
// calendar whose middle years failed to parse still says "2020-2030" while covering two, and
// every consumer of that string -- the runners' refusal message (BA-1), the out-of-range warn,
// the "extend the calendar" ERROR -- would repeat the claim. The count is what distinguishes
// them, and it is what the equity runner now prints beside the previous trading day it
// resolved (drift-F).
//
// The load path is what makes this non-trivial: `covered_years_` used to gain a year BEFORE
// that year's entries parsed, so a malformed file advertised coverage it did not have. These
// assertions pin the count against `covers_year()` and against the description, so the three
// can never disagree again.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "trade_ngin/core/holiday_checker.hpp"

using namespace trade_ngin;

namespace {

std::filesystem::path find_repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

}  // namespace

TEST(HolidayCoverageYears, AnUnloadedCheckerCoversNothing) {
    HolidayChecker checker("/nonexistent/market_holidays.json");
    EXPECT_FALSE(checker.loaded());
    EXPECT_EQ(checker.coverage_years(), 0u);
    EXPECT_EQ(checker.coverage_description(), "no years loaded");
}

TEST(HolidayCoverageYears, TheCountAgreesWithCoversYearAcrossTheWholeDescribedRange) {
    auto path = find_repo_file("include/trade_ngin/core/holidays.json");
    if (path.empty()) GTEST_SKIP() << "holidays.json not found from the working directory";

    HolidayChecker checker(path.string());
    ASSERT_TRUE(checker.loaded()) << "failed to load " << path.string();

    const size_t years = checker.coverage_years();
    EXPECT_GT(years, 0u);

    // The description is "first-last". Count how many of those years are actually covered and
    // require the count to match -- this is the assertion that catches a gapped calendar
    // describing itself as a range.
    const std::string desc = checker.coverage_description();
    const auto dash = desc.find('-');
    ASSERT_NE(dash, std::string::npos) << desc;
    const int first = std::stoi(desc.substr(0, dash));
    const int last = std::stoi(desc.substr(dash + 1));
    ASSERT_LE(first, last);

    size_t covered = 0;
    for (int y = first; y <= last; ++y) {
        if (checker.covers_year(y)) ++covered;
    }
    EXPECT_EQ(covered, years)
        << "covers_year() and coverage_years() disagree over " << desc;
    EXPECT_EQ(years, static_cast<size_t>(last - first + 1))
        << "the calendar describes itself as the contiguous range " << desc
        << " but covers " << years << " year(s): a gap in the middle would make every "
           "'loaded: " << desc << "' message a false claim";

    // Endpoints are inside coverage, one step beyond either end is not.
    EXPECT_TRUE(checker.covers_year(first));
    EXPECT_TRUE(checker.covers_year(last));
    EXPECT_FALSE(checker.covers_year(first - 1));
    EXPECT_FALSE(checker.covers_year(last + 1));
}

TEST(HolidayCoverageYears, CoverageSpansEveryDateTheEquityGateWindowsUse) {
    auto path = find_repo_file("include/trade_ngin/core/holidays.json");
    if (path.empty()) GTEST_SKIP() << "holidays.json not found from the working directory";

    HolidayChecker checker(path.string());
    ASSERT_TRUE(checker.loaded());

    // Every date any equity replay in this campaign has run on, plus the pointed corp-action
    // replays B-5a and B-5c used. A run outside coverage is refused outright, so these are
    // the years the branch actually needs.
    for (const auto& date : {"2004-05-03", "2017-01-04", "2019-06-03", "2020-04-03",
                             "2024-04-01", "2025-06-30", "2026-04-15", "2026-08-06"}) {
        EXPECT_TRUE(checker.covers_date(date))
            << date << " is outside the calendar (" << checker.coverage_description()
            << ", " << checker.coverage_years() << " years)";
    }
}
