// HolidayChecker -- the market-calendar component in one place.
//
// Three concerns, kept as labelled sections below: whether the calendar can
// answer for a date at all (coverage), walking backwards to the previous
// session (lookback), and locating holidays.json on disk (path resolution).
// They were separate files named after the incidents that produced them; the
// component is one class, so the tests live together and the sections say
// what each block is for.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <sstream>
#include <string>

#include "trade_ngin/core/holiday_checker.hpp"

using namespace trade_ngin;

// ──────────────────────────────────────────────────────────────────────────
// Calendar coverage -- "not a holiday" vs "cannot answer".
// The calendar spans a finite set of years. `is_holiday` returning false for
// an uncovered date reads identically to "the market was open", so a replay
// of an uncovered year silently treats every closure as a trading day. These
// pin the distinction, and the generator rules behind the shipped file.
// (was: test_holiday_calendar_coverage.cpp)
// ──────────────────────────────────────────────────────────────────────────

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

    // Pins the loaded set as exactly {2024, 2025}: the description reports the
    // min and max, so nothing lies outside that range, and probing either side
    // plus both members leaves no room for a different set. An empty year
    // (2024 here) must still count as covered -- "no closures that year" is an
    // answer, unlike "that year was never loaded".
    EXPECT_EQ(checker.coverage_description(), "2024-2025");
    EXPECT_TRUE(checker.covers_year(2024));
    EXPECT_TRUE(checker.covers_year(2025));
    EXPECT_FALSE(checker.covers_year(2023));
    EXPECT_FALSE(checker.covers_year(2026));
}

TEST(HolidayCalendarCoverage, MalformedDatesDoNotClaimCoverage) {
    TempCalendar cal(R"({"2026":[{"date":"2026-12-25","name":"Christmas Day","type":"fixed"}]})");
    HolidayChecker checker(cal.path());

    EXPECT_FALSE(checker.covers_date(""));
    EXPECT_FALSE(checker.covers_date("26"));
    EXPECT_FALSE(checker.covers_date("not-a-date"));
}

// BA-1 / C-2 D1: a load that throws PART WAY THROUGH must publish nothing.
//
// The year key was inserted before that year's entries parsed, and the outer
// catch returned false without clearing, so a calendar that failed halfway was
// left marked "covered" with its closures missing. That is worse than an empty
// calendar: covers_date returns TRUE, the runner's fail-closed guard passes,
// is_holiday returns false with no warning (the year IS covered), and
// find_previous_trading_day walks onto a closed day. The guard only ever fired
// when the load failed TOTALLY.
TEST(HolidayCalendarCoverage, PartialLoadPublishesNothingRatherThanClaimingCoverage) {
    // 2025 parses cleanly; 2026's first entry has no "type", so the parse throws
    // after 2026 has already been inserted into the covered set -- the shape a
    // hand-edited or truncated calendar actually produces.
    TempCalendar cal(
        R"({"2025":[{"date":"2025-01-01","name":"New Year's Day","type":"fixed"}],)"
        R"("2026":[{"date":"2026-05-25","name":"Memorial Day"},)"
        R"({"date":"2026-12-25","name":"Christmas Day","type":"fixed"}]})");
    HolidayChecker checker(cal.path());

    EXPECT_FALSE(checker.loaded())
        << "a throw part way through the file is a failed load, not a partial success";
    EXPECT_FALSE(checker.covers_year(2026))
        << "2026 was inserted before its entries parsed; claiming coverage makes "
           "is_holiday(\"2026-05-25\") read as \"the market was open\"";
    EXPECT_FALSE(checker.covers_date("2026-05-26"))
        << "the runner's fail-closed guard consults covers_date and must fire";
    EXPECT_FALSE(checker.covers_year(2025))
        << "all or nothing: a half-applied calendar is not a calendar, so even the "
           "year that did parse must not be advertised";
    EXPECT_FALSE(checker.is_holiday("2026-05-25"))
        << "Memorial Day never loaded; false here means unknown, and covers_date says so";
}

// The success path still publishes, and says so.
TEST(HolidayCalendarCoverage, FullLoadReportsLoaded) {
    TempCalendar cal(R"({"2026":[{"date":"2026-12-25","name":"Christmas Day","type":"fixed"}]})");
    HolidayChecker checker(cal.path());

    EXPECT_TRUE(checker.loaded());
    EXPECT_TRUE(checker.covers_year(2026));
    EXPECT_TRUE(checker.is_holiday("2026-12-25"));
}

// A file that cannot be opened at all is also a failed load -- the case the
// original guard did catch, kept so the fix does not narrow it.
TEST(HolidayCalendarCoverage, MissingFileReportsNotLoaded) {
    HolidayChecker checker("/nonexistent/path/holidays.json");

    EXPECT_FALSE(checker.loaded());
    EXPECT_FALSE(checker.covers_year(2026));
    EXPECT_FALSE(checker.covers_date("2026-05-26"));
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

// ──────────────────────────────────────────────────────────────────────────
// Lookback -- find_previous_trading_day walks back over closed days.
// Bounded at 14 days: enough for Christmas-week stacks and 9/11-style
// multi-day exchange closures, and it fails closed when the bound is
// exhausted rather than returning a closed day as if it traded.
// (was: test_holiday_lookback.cpp)
// ──────────────────────────────────────────────────────────────────────────

// Regression test for audit finding §1.9: holiday lookback was bounded at 5
// days, which is borderline for Christmas-week stacks and outright wrong for
// 9/11-style multi-day exchange closures. Refactored into
// HolidayChecker::find_previous_trading_day with a 14-day bound and explicit
// fail-closed signaling when the bound is exhausted.

namespace {

// Build a YYYY-MM-DD string from a time_point.
std::string date_string(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm = *std::localtime(&t);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d");
    return ss.str();
}

// Parse YYYY-MM-DD into a time_point at local-midnight, plus a small fixed
// offset so the holiday-check string formatting is consistent across systems.
std::chrono::system_clock::time_point parse_date(const std::string& s) {
    std::tm tm{};
    std::istringstream iss(s);
    iss >> std::get_time(&tm, "%Y-%m-%d");
    tm.tm_hour = 12;  // noon to avoid any DST-edge surprise
    auto t = std::mktime(&tm);
    return std::chrono::system_clock::from_time_t(t);
}

// Write a holidays JSON file with the given list of dates marked as holidays.
void write_holidays_json(const std::filesystem::path& path,
                         const std::vector<std::string>& dates) {
    std::ofstream out(path);
    out << "{\n";
    out << "  \"2024\": [\n";
    for (size_t i = 0; i < dates.size(); ++i) {
        out << "    {\"date\": \"" << dates[i]
            << "\", \"name\": \"TEST\", \"type\": \"federal\"}";
        if (i + 1 < dates.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

class HolidayLookbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() /
                   ("holiday_test_" + std::to_string(::testing::UnitTest::GetInstance()
                                                          ->current_test_info()
                                                          ->result()
                                                          ->test_property_count()) +
                    "_" + std::to_string(std::rand()));
        std::filesystem::create_directories(tmp_dir_);
        holidays_path_ = tmp_dir_ / "holidays.json";
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(tmp_dir_, ec);
    }

    std::filesystem::path tmp_dir_;
    std::filesystem::path holidays_path_;
};

}  // namespace

// Tuesday with no holidays: previous trading day is Monday.
TEST_F(HolidayLookbackTest, NoHolidaysReturnsYesterday) {
    write_holidays_json(holidays_path_, {});
    HolidayChecker checker(holidays_path_.string());

    auto start = parse_date("2024-03-05");  // Tuesday
    auto prev = checker.find_previous_trading_day(start);
    ASSERT_TRUE(prev.has_value());
    EXPECT_EQ(date_string(*prev), "2024-03-04");  // Monday
}

// Monday with no holidays: skip Sunday and Saturday → Friday.
TEST_F(HolidayLookbackTest, WeekendSkippedToFriday) {
    write_holidays_json(holidays_path_, {});
    HolidayChecker checker(holidays_path_.string());

    auto start = parse_date("2024-03-04");  // Monday
    auto prev = checker.find_previous_trading_day(start);
    ASSERT_TRUE(prev.has_value());
    EXPECT_EQ(date_string(*prev), "2024-03-01");  // Friday
}

// 6 consecutive holidays before a Tuesday — pre-fix this would have silently
// returned a stale date because the 5-bound exhausted; post-fix the 14-bound
// walks past and returns the day before the stack.
TEST_F(HolidayLookbackTest, SixConsecutiveClosuresWalkBackCorrectly) {
    write_holidays_json(holidays_path_,
                        {"2024-03-04", "2024-03-05", "2024-03-06",
                         "2024-03-07", "2024-03-08", "2024-03-11"});
    HolidayChecker checker(holidays_path_.string());

    // Start: Tuesday 2024-03-12. Walk back skips:
    //   03-11 (Mon holiday), 03-10 (Sun), 03-09 (Sat),
    //   03-08 holiday, 03-07 holiday, 03-06 holiday, 03-05 holiday, 03-04 holiday
    //   -> 03-01 (Friday) is the previous trading day.
    auto start = parse_date("2024-03-12");
    auto prev = checker.find_previous_trading_day(start);
    ASSERT_TRUE(prev.has_value());
    EXPECT_EQ(date_string(*prev), "2024-03-01");
}

// 20-day closure stack with the default 14-day bound: fail closed (nullopt).
// The caller MUST treat this as an error, not silently use a stale date.
TEST_F(HolidayLookbackTest, ExhaustedBoundReturnsNullopt) {
    std::vector<std::string> all_march_days_as_holidays;
    for (int d = 1; d <= 31; ++d) {
        std::ostringstream s;
        s << "2024-03-" << std::setw(2) << std::setfill('0') << d;
        all_march_days_as_holidays.push_back(s.str());
    }
    // Also block the late-February days so the bound truly exhausts.
    for (int d = 20; d <= 29; ++d) {
        std::ostringstream s;
        s << "2024-02-" << std::setw(2) << std::setfill('0') << d;
        all_march_days_as_holidays.push_back(s.str());
    }
    write_holidays_json(holidays_path_, all_march_days_as_holidays);
    HolidayChecker checker(holidays_path_.string());

    auto start = parse_date("2024-03-15");
    auto prev = checker.find_previous_trading_day(start, /*max_lookback_days=*/14);
    EXPECT_FALSE(prev.has_value())
        << "find_previous_trading_day must fail closed when the lookback bound "
           "is exhausted, not return a stale date.";
}

// Explicit small-bound test — the original 5-day bug.
TEST_F(HolidayLookbackTest, OldBoundOfFiveExhaustsOnSixDayStack) {
    write_holidays_json(holidays_path_,
                        {"2024-03-04", "2024-03-05", "2024-03-06",
                         "2024-03-07", "2024-03-08", "2024-03-11"});
    HolidayChecker checker(holidays_path_.string());

    // Start Tuesday 2024-03-12. With bound 5 we walk back 5 days
    // (03-11, 03-10, 03-09, 03-08, 03-07) -- all non-trading -- and exhaust
    // before finding 03-01. The old code silently used 03-07; the new API
    // surfaces this via nullopt.
    auto start = parse_date("2024-03-12");
    auto prev = checker.find_previous_trading_day(start, /*max_lookback_days=*/5);
    EXPECT_FALSE(prev.has_value())
        << "5-day bound must surface failure on a 6+ day closure stack.";
}

// ──────────────────────────────────────────────────────────────────────────
// Path resolution -- where holidays.json is found.
// Fallback chain: TRADE_NGIN_HOLIDAYS_JSON env var, then the dev/source
// layout, then a deploy bundle beside the binary, then /etc. If none exist
// it returns the source-layout path so the load error names the likely spot.
// (was: test_holiday_path_resolution.cpp)
// ──────────────────────────────────────────────────────────────────────────

// Phase 6 §6a -- pins the holidays.json path-resolution fallback chain:
//   1. TRADE_NGIN_HOLIDAYS_JSON env var (returned as-is, even if missing)
//   2. ./include/trade_ngin/core/holidays.json (dev/source layout)
//   3. ./holidays.json (deploy bundle next to the binary)
//   4. /etc/trade_ngin/holidays.json (system-wide)
// If none exist, returns option (2) so the load-error log names the most
// likely expected location.

namespace {

class HolidayPathResolutionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Make sure the env var is unset at the start of each test; the
        // test that sets it cleans up after itself.
        unsetenv("TRADE_NGIN_HOLIDAYS_JSON");
    }
    void TearDown() override {
        unsetenv("TRADE_NGIN_HOLIDAYS_JSON");
    }
};

}  // namespace

// Env var wins, returned verbatim even if the file doesn't exist (so the
// load-error log names the misconfigured path -- silent fallback would be
// worse).
TEST_F(HolidayPathResolutionTest, EnvVarWinsAndPassesThroughVerbatim) {
    setenv("TRADE_NGIN_HOLIDAYS_JSON", "/nonexistent/custom/holidays.json", 1);
    EXPECT_EQ(HolidayChecker::resolve_holidays_path(),
              "/nonexistent/custom/holidays.json");
}

// Env var honored even when the file DOES exist at one of the fallback
// paths.
TEST_F(HolidayPathResolutionTest, EnvVarPreemptsFallbackChain) {
    auto tmp = std::filesystem::temp_directory_path() / "test_holidays_env.json";
    {
        std::ofstream(tmp) << "{}";
    }
    setenv("TRADE_NGIN_HOLIDAYS_JSON", tmp.string().c_str(), 1);
    EXPECT_EQ(HolidayChecker::resolve_holidays_path(), tmp.string());
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

// With no env var and no override files set up, the resolver returns the
// dev-layout path so the load-error log points operators at the most
// likely expected location.
TEST_F(HolidayPathResolutionTest, ReturnsDevLayoutPathOnFallback) {
    const std::string p = HolidayChecker::resolve_holidays_path();
    // The dev-layout path is the recommended default. The test environment
    // may or may not have the file present in the CWD; what we pin is the
    // shape of the returned string when nothing else matches.
    //
    // If the file IS present at the dev-layout path (because tests run
    // from the repo root), the resolver still returns it -- that's the
    // intended behavior. If absent, we get the dev-layout path anyway
    // (option (2) above).
    EXPECT_TRUE(p == "include/trade_ngin/core/holidays.json" ||
                p == "holidays.json" ||
                p == "/etc/trade_ngin/holidays.json")
        << "Unexpected resolved path: " << p;
}

// Loading from a fully-resolved path should produce a valid HolidayChecker
// (even with an empty JSON file -- just means no holidays registered).
TEST_F(HolidayPathResolutionTest, ConstructorAcceptsResolvedPath) {
    auto tmp = std::filesystem::temp_directory_path() / "test_holidays_ctor.json";
    {
        std::ofstream(tmp) << "{}";  // empty calendar
    }
    setenv("TRADE_NGIN_HOLIDAYS_JSON", tmp.string().c_str(), 1);
    HolidayChecker checker(HolidayChecker::resolve_holidays_path());
    EXPECT_FALSE(checker.is_holiday("2024-12-25"));  // empty calendar = nothing is a holiday
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}
