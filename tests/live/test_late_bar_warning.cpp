// tests/live/test_late_bar_warning.cpp
//
// The late-bar warning (late_bar_warning.hpp). A held symbol's bar for day D is missing when the
// run dated D+1 settles the row dated D (the row books 0), and arrives before the next run. That
// run settles its own Day T-1 against the D close, so the move into D is on no stored row. The
// ruling: no catch-up, no refusal; the run that first consumes the late bar names it once.
//
// The worked case is session A's fixture: ZF short 2, the 2026-04-28 bar absent at the 04-29 run
// and back for the 04-30 run; -2 x (108.0546875 - 108.1953125) x 1000 = 281.25.

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "trade_ngin/live/late_bar_warning.hpp"

using namespace trade_ngin;

namespace {

Timestamp day(const std::string& s) {
    const int y = std::stoi(s.substr(0, 4));
    const unsigned m = static_cast<unsigned>(std::stoi(s.substr(5, 2)));
    const unsigned d = static_cast<unsigned>(std::stoi(s.substr(8, 2)));
    return Timestamp(std::chrono::sys_days{std::chrono::year{y} / std::chrono::month{m} / std::chrono::day{d}});
}

Bar bar(const std::string& symbol, const std::string& date, double close, const std::string& id = "ZFM6") {
    Bar b(day(date), close, close, close, close, 1000.0, symbol);
    b.instrument_id = id;
    return b;
}

Position stored(const std::string& symbol, double quantity, double realized) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(quantity);
    p.realized_pnl = Decimal(realized);
    return p;
}

// The stored rows by date, and how often each date was read.
struct Rows {
    std::map<std::string, std::vector<Position>> by_date;
    std::map<std::string, int> reads;
    StoredRowsOfDate reader() {
        return [this](const Timestamp& t) {
            const std::string d = SessionClassifier::ymd(SessionClassifier::day_of(t));
            ++reads[d];
            const auto it = by_date.find(d);
            return it == by_date.end() ? std::vector<Position>{} : it->second;
        };
    }
};

const std::vector<Bar> kZf = {bar("ZF.v.0", "2026-04-24", 108.25), bar("ZF.v.0", "2026-04-27", 108.1953125),
                              bar("ZF.v.0", "2026-04-28", 108.0546875), bar("ZF.v.0", "2026-04-29", 107.6328125)};

}  // namespace

TEST(LateBarWarning, TheRunThatFirstConsumesALateBarNamesItWithTheUnbookedAmount) {
    Rows rows;
    rows.by_date["2026-04-28"] = {stored("ZF.v.0", -2.0, 0.0), stored("KE.v.0", 1.0, -550.0)};
    rows.by_date["2026-04-27"] = {stored("ZF.v.0", -2.0, -62.5)};
    const auto late = find_late_bars(kZf, "2026-04-29", {"ZF.v.0"}, {}, rows.reader());
    ASSERT_EQ(late.size(), 1u);
    EXPECT_EQ(late[0].symbol, "ZF.v.0");
    EXPECT_EQ(late[0].date, "2026-04-28");
    EXPECT_EQ(late[0].quantity, -2.0);
    EXPECT_DOUBLE_EQ(late[0].unbooked(1000.0), 281.25);
    const std::string line = late_bar_warning_line(late[0], 1000.0);
    EXPECT_EQ(line.rfind("LATE_BAR ZF.v.0 2026-04-28: ", 0), 0u) << line;
    EXPECT_NE(line.find("so 281.25 is on no stored row"), std::string::npos) << line;
    EXPECT_NE(line.find("(-2 x (108.0546875 - 108.1953125) x 1000:"), std::string::npos) << line;
    EXPECT_NE(line.find("Remedy: re-run 2026-04-29 (the date that settles the 2026-04-28 bar) and every "
                        "later date in order"),
              std::string::npos)
        << line;
    EXPECT_EQ(rows.reads["2026-04-27"], 1) << "the walk stops at the first booked row";
}

TEST(LateBarWarning, ABookedRowIsNotALateBarAndTheNextRunDoesNotRepeatTheLine) {
    // The run after the one above: T-1 is 04-30, the bar before it is 04-29, whose row is booked.
    std::vector<Bar> bars = kZf;
    bars.push_back(bar("ZF.v.0", "2026-04-30", 107.8515625));
    Rows rows;
    rows.by_date["2026-04-29"] = {stored("ZF.v.0", -2.0, 843.75)};
    rows.by_date["2026-04-28"] = {stored("ZF.v.0", -2.0, 0.0)};
    EXPECT_TRUE(find_late_bars(bars, "2026-04-30", {"ZF.v.0"}, {}, rows.reader()).empty());
    EXPECT_EQ(rows.reads.count("2026-04-28"), 0u) << "the 04-28 row is never reached again";
}

TEST(LateBarWarning, TwoLateBarsInARowAreBothNamed) {
    Rows rows;
    rows.by_date["2026-04-28"] = {stored("ZF.v.0", -2.0, 0.0)};
    rows.by_date["2026-04-27"] = {stored("ZF.v.0", -3.0, 0.0)};
    rows.by_date["2026-04-24"] = {stored("ZF.v.0", -3.0, 0.0)};  // the first bar of the window: nothing before it
    const auto late = find_late_bars(kZf, "2026-04-29", {"ZF.v.0"}, {}, rows.reader());
    ASSERT_EQ(late.size(), 2u);
    EXPECT_EQ(late[0].date, "2026-04-28");
    EXPECT_EQ(late[1].date, "2026-04-27");
    EXPECT_DOUBLE_EQ(late[1].unbooked(1000.0), -3.0 * (108.1953125 - 108.25) * 1000.0);
}

TEST(LateBarWarning, TheSleevesOfABookAreSummedInOneLine) {
    Rows rows;
    rows.by_date["2026-04-28"] = {stored("ZF.v.0", -2.0, 0.0), stored("ZF.v.0", -1.0, 0.0)};
    const auto late = find_late_bars(kZf, "2026-04-29", {"ZF.v.0"}, {}, rows.reader());
    ASSERT_EQ(late.size(), 1u);
    EXPECT_EQ(late[0].quantity, -3.0);
}

TEST(LateBarWarning, WhatIsNotALateBar) {
    Rows zero;
    zero.by_date["2026-04-28"] = {stored("ZF.v.0", -2.0, 0.0)};
    // Not held in the stored T-1 book.
    EXPECT_TRUE(find_late_bars(kZf, "2026-04-29", {"KE.v.0"}, {}, zero.reader()).empty());
    // No consumed T-1 bar on this run (the run's T-1 is a later date).
    EXPECT_TRUE(find_late_bars(kZf, "2026-04-30", {"ZF.v.0"}, {}, zero.reader()).empty());
    // A roll this run settles late books its unbooked bars itself.
    EXPECT_TRUE(find_late_bars(kZf, "2026-04-29", {"ZF.v.0"}, {"ZF.v.0"}, zero.reader()).empty());
    // The stored row holds nothing on that date, or there is no row.
    Rows flat;
    flat.by_date["2026-04-28"] = {stored("ZF.v.0", 0.0, 0.0)};
    EXPECT_TRUE(find_late_bars(kZf, "2026-04-29", {"ZF.v.0"}, {}, flat.reader()).empty());
    Rows none;
    EXPECT_TRUE(find_late_bars(kZf, "2026-04-29", {"ZF.v.0"}, {}, none.reader()).empty());
    // The close did not move: a row that books 0 is right.
    std::vector<Bar> still = kZf;
    still[2] = bar("ZF.v.0", "2026-04-28", 108.1953125);
    EXPECT_TRUE(find_late_bars(still, "2026-04-29", {"ZF.v.0"}, {}, zero.reader()).empty());
    // A change bar books 0 by design (section 6.6): the contract differs from the bar before it.
    std::vector<Bar> rolled = kZf;
    rolled[2] = bar("ZF.v.0", "2026-04-28", 108.0546875, "ZFU6");
    EXPECT_TRUE(find_late_bars(rolled, "2026-04-29", {"ZF.v.0"}, {}, zero.reader()).empty());
}
