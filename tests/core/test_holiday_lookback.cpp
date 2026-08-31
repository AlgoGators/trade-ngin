#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include "trade_ngin/core/holiday_checker.hpp"

using namespace trade_ngin;

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
