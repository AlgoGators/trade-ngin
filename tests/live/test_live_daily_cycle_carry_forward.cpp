// tests/live/test_live_daily_cycle_carry_forward.cpp
//
// T-OR.3 -- the closed-day path.
//
// The equity runner has processed weekends and holidays as CARRY-FORWARD days since
// `eaa292d1`: the book, live_results and equity_curve are written from the previous
// session while signal generation and execution are skipped. That behaviour was
// implemented as five inline copies of `dow == 0 || dow == 6 || is_holiday(...)` inside
// main(), and nothing anywhere covered the predicate or the carry decision -- `grep
// is_holiday tests/live tests/strategy` returned nothing. A closed day that silently
// starts trading, or a carried book that silently gains a realized figure, would be
// invisible until it showed up in the P&L.
//
// The two decisions now live in LiveDailyCycle, and this is their test.

#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/live_daily_cycle.hpp"

using namespace trade_ngin;

namespace {

std::tm utc_tm_for(const std::string& ymd) {
    std::tm t{};
    t.tm_year = std::stoi(ymd.substr(0, 4)) - 1900;
    t.tm_mon = std::stoi(ymd.substr(5, 2)) - 1;
    t.tm_mday = std::stoi(ymd.substr(8, 2));
    t.tm_hour = 12;
    // tm_wday is what the weekend test reads, and the runner gets it from gmtime_r.
    // timegm/gmtime_r round-trip populates it the same way here.
    std::time_t as_utc = timegm(&t);
    std::tm out{};
    gmtime_r(&as_utc, &out);
    return out;
}

Position held(const std::string& symbol, double qty, double basis, double unrealized,
              double realized) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(basis);
    p.unrealized_pnl = Decimal(unrealized);
    p.realized_pnl = Decimal(realized);
    p.last_update = std::chrono::system_clock::now();
    return p;
}

// The shipped calendar, found from wherever the test binary was launched. Same walk the
// suite's other calendar tests use: ctest runs from build/, a direct invocation may run
// from the source root.
std::string shipped_calendar_path() {
    for (const auto* p : {"include/trade_ngin/core/holidays.json",
                          "../include/trade_ngin/core/holidays.json",
                          "../../include/trade_ngin/core/holidays.json"}) {
        if (std::filesystem::exists(p)) return p;
    }
    return "";
}

}  // namespace

class LiveDailyCycleCarryForwardTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string path = shipped_calendar_path();
        if (path.empty()) {
            GTEST_SKIP() << "shipped holidays.json not reachable from cwd";
        }
        holidays_ = std::make_unique<HolidayChecker>(path);
        ASSERT_TRUE(holidays_->loaded())
            << "the calendar is the input under test; without it every date reports open";
        ASSERT_TRUE(holidays_->covers_date("2026-08-03"))
            << "calendar does not cover 2026, so these assertions would prove nothing";
    }

    std::unique_ptr<HolidayChecker> holidays_;
};

// ---------------------------------------------------------------------------
// the predicate
// ---------------------------------------------------------------------------

TEST_F(LiveDailyCycleCarryForwardTest, SaturdayAndSundayAreNonTrading) {
    EXPECT_TRUE(LiveDailyCycle::is_non_trading_day(utc_tm_for("2026-08-01"), *holidays_))
        << "2026-08-01 is a Saturday";
    EXPECT_TRUE(LiveDailyCycle::is_non_trading_day(utc_tm_for("2026-08-02"), *holidays_))
        << "2026-08-02 is a Sunday";
}

TEST_F(LiveDailyCycleCarryForwardTest, AWeekdayHolidayIsNonTrading) {
    ASSERT_TRUE(holidays_->is_holiday("2026-07-03"))
        << "fixture precondition: the observed Independence Day holiday is in the calendar";
    EXPECT_TRUE(LiveDailyCycle::is_non_trading_day(utc_tm_for("2026-07-03"), *holidays_));
}

TEST_F(LiveDailyCycleCarryForwardTest, AnOrdinaryWeekdayIsATradingDay) {
    EXPECT_FALSE(LiveDailyCycle::is_non_trading_day(utc_tm_for("2026-08-03"), *holidays_))
        << "2026-08-03 is a Monday and not a holiday";
    EXPECT_FALSE(LiveDailyCycle::is_non_trading_day(utc_tm_for("2026-08-06"), *holidays_))
        << "2026-08-06 is a Thursday and not a holiday";
}

// A holiday that lands on a weekend is still non-trading -- the weekend test short
// circuits before the calendar is consulted, so the two rules cannot disagree.
TEST_F(LiveDailyCycleCarryForwardTest, AHolidayThatIsAlsoAWeekendIsNonTrading) {
    EXPECT_TRUE(LiveDailyCycle::is_non_trading_day(utc_tm_for("2026-07-04"), *holidays_))
        << "2026-07-04 is a Saturday; the observed holiday is 07-03";
}

// ---------------------------------------------------------------------------
// the carry decision
// ---------------------------------------------------------------------------

// The carried book IS the previous book on everything that describes the holding, and
// carries no flow. Nothing traded, so nothing was realized; nothing printed, so nothing
// was re-marked.
TEST_F(LiveDailyCycleCarryForwardTest, CarriedBookKeepsQuantityBasisAndMarkAndRealizesNothing) {
    std::unordered_map<std::string, Position> previous;
    previous["AAPL"] = held("AAPL", 100.0, 150.25, 375.0, 42.5);
    previous["MSFT"] = held("MSFT", -0.0, 0.0, 0.0, -17.0);  // a closed row's residue

    const auto carried = LiveDailyCycle::carry_forward(previous);

    ASSERT_EQ(carried.size(), previous.size());
    for (const auto& [symbol, before] : previous) {
        auto it = carried.find(symbol);
        ASSERT_NE(it, carried.end()) << symbol << " was dropped from the carried book";
        EXPECT_DOUBLE_EQ(it->second.quantity.as_double(), before.quantity.as_double());
        EXPECT_DOUBLE_EQ(it->second.average_price.as_double(), before.average_price.as_double())
            << "a carried holding's cost basis is not re-derived on a closed day";
        EXPECT_DOUBLE_EQ(it->second.unrealized_pnl.as_double(),
                         before.unrealized_pnl.as_double())
            << "no bar closed, so the mark is the previous session's";
        EXPECT_DOUBLE_EQ(it->second.realized_pnl.as_double(), 0.0)
            << "daily_realized_pnl is a FLOW; carrying yesterday's makes it a running total";
    }

    // The source book is not mutated -- it is still needed as the T-1 snapshot.
    EXPECT_DOUBLE_EQ(previous.at("AAPL").realized_pnl.as_double(), 42.5);
}

// A carried book generates no executions: every delta against the previous book is
// zero. This is the property the runner's closed-day skip relies on, asserted rather
// than assumed.
TEST_F(LiveDailyCycleCarryForwardTest, CarriedBookHasNoDeltaAgainstThePreviousBook) {
    std::unordered_map<std::string, Position> previous;
    previous["AAPL"] = held("AAPL", 100.0, 150.25, 375.0, 42.5);
    previous["MSFT"] = held("MSFT", 40.0, 300.0, -120.0, 0.0);

    const auto carried = LiveDailyCycle::carry_forward(previous);

    for (const auto& [symbol, after] : carried) {
        const double delta =
            after.quantity.as_double() - previous.at(symbol).quantity.as_double();
        EXPECT_DOUBLE_EQ(delta, 0.0) << symbol << " would have traded on a closed day";
    }
}

TEST_F(LiveDailyCycleCarryForwardTest, AnEmptyBookCarriesForwardAsAnEmptyBook) {
    EXPECT_TRUE(LiveDailyCycle::carry_forward({}).empty());
}
