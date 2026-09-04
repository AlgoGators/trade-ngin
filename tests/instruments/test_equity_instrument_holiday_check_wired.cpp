#include <gtest/gtest.h>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/instruments/equity.hpp"

using namespace trade_ngin;

// Regression test for audit finding §1.10: EquityInstrument::is_market_open()
// was wired to consult HolidayChecker only via a static slot that was never
// populated in production -- so it silently allowed Christmas-day trades.
// Phase 3d calls EquityInstrument::set_holiday_checker() at app startup; this
// test verifies the wired path actually rejects holiday timestamps.

namespace {

std::chrono::system_clock::time_point make_local(int year, int month, int day,
                                                 int hour, int minute) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_isdst = -1;
    auto t = std::mktime(&tm);
    return std::chrono::system_clock::from_time_t(t);
}

class EquityInstrumentHolidayTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() /
                   ("equity_holiday_" + std::to_string(std::rand()));
        std::filesystem::create_directories(tmp_dir_);
        holidays_path_ = tmp_dir_ / "holidays.json";

        std::ofstream out(holidays_path_);
        out << R"({
  "2026": [
    {"date": "2026-12-25", "name": "Christmas Day", "type": "federal"}
  ]
})";
        out.close();

        spec_.exchange = "NASDAQ";
        spec_.currency = "USD";
        spec_.tick_size = 0.01;
        spec_.trading_hours = "09:30-16:00";

        // Make a fresh checker and wire it into the static slot.
        auto checker = std::make_shared<HolidayChecker>(holidays_path_.string());
        EquityInstrument::set_holiday_checker(checker);
    }

    void TearDown() override {
        // Reset the static so subsequent tests aren't surprised by our mock.
        EquityInstrument::set_holiday_checker(nullptr);
        std::error_code ec;
        std::filesystem::remove_all(tmp_dir_, ec);
    }

    std::filesystem::path tmp_dir_;
    std::filesystem::path holidays_path_;
    EquitySpec spec_;
};

}  // namespace

// Friday 2026-12-25 is Christmas (configured holiday). Even at 10:00 local
// (well inside the 09:30-16:00 trading window) and a weekday, is_market_open
// must report false because the holiday check fires.
TEST_F(EquityInstrumentHolidayTest, ChristmasReturnsClosedWhenCheckerWired) {
    EquityInstrument aapl("AAPL", spec_);
    auto christmas_10am = make_local(2026, 12, 25, 10, 0);
    EXPECT_FALSE(aapl.is_market_open(christmas_10am));
}

// Sanity: an ordinary Wednesday at 10am is open.
TEST_F(EquityInstrumentHolidayTest, RegularWeekdayWithinHoursReturnsOpen) {
    EquityInstrument aapl("AAPL", spec_);
    auto wed_10am = make_local(2026, 12, 9, 10, 0);  // Wednesday
    EXPECT_TRUE(aapl.is_market_open(wed_10am));
}

// If no checker is wired (e.g. pre-fix or after TearDown), the method falls
// back to weekday-and-hours only. This documents the contract: the checker is
// the holiday-aware path; without it, holidays slip through.
TEST_F(EquityInstrumentHolidayTest, WithoutCheckerHolidayWouldBeMissed) {
    EquityInstrument::set_holiday_checker(nullptr);
    EquityInstrument aapl("AAPL", spec_);
    auto christmas_10am = make_local(2026, 12, 25, 10, 0);
    // Christmas 2026 is a Friday -- weekday, within hours -- so without a
    // checker, the method incorrectly returns true. This is the pre-fix bug,
    // pinned here as a contract assertion (NOT a desired behavior).
    EXPECT_TRUE(aapl.is_market_open(christmas_10am))
        << "Without a holiday checker, is_market_open should fall through to "
           "weekday+hours only. If this fails, the fallback contract changed.";
}
