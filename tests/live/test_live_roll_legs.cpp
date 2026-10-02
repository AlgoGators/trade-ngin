// tests/live/test_live_roll_legs.cpp
//
// LOOP_SPEC v6.1 section 6.5 in the live runners (T-ROLLX-FIX commit 3; code review D3, X-2 / L-09):
// a run dated R consumes every bar dated before R. It books the legs of every roll a consumed bar
// CONFIRMED after the previous run's T-1 (live_legs_since: the previous run's date less one day) up
// to its own T-1 (rolls_confirmed_in), so each roll is booked once: on the first run that consumes
// its confirming bar, whether that bar is the run's T-1 or an earlier bar of a catch-up after a
// missed run. The legs' ids carry the CONFIRMING bar's date.
//
// The runs below are simulated as the runner drives them: each run's consumed window, its T-1, and
// the previous stored run's date.

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/live/live_roll_legs.hpp"

using namespace trade_ngin;

namespace {

Timestamp at(const std::string& ymd) {
    std::tm tm{};
    tm.tm_year = std::stoi(ymd.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(ymd.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(ymd.substr(8, 2));
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

Bar bar(const std::string& symbol, const std::string& ymd, double close, const std::string& id) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = at(ymd);
    b.open = Decimal(close);
    b.high = Decimal(close * 1.01);
    b.low = Decimal(close * 0.99);
    b.close = Decimal(close);
    b.volume = 100000.0;
    b.instrument_id = id;
    return b;
}

// Weekday bars 2026-04-20 (Mon) .. 2026-05-01 (Fri); no weekend bars.
//   ZC: H -> K, change bar Thu 04-23, CONFIRMED Fri 04-24 (the weekend follows);
//       K -> N, change bar Wed 04-29, CONFIRMED Thu 04-30.
//   ZW: H -> K, change bar Tue 04-28, CONFIRMED Wed 04-29.
const std::vector<std::string> kDays = {"2026-04-20", "2026-04-21", "2026-04-22", "2026-04-23",
                                        "2026-04-24", "2026-04-27", "2026-04-28", "2026-04-29",
                                        "2026-04-30", "2026-05-01"};

std::vector<Bar> window() {
    std::vector<Bar> out;
    for (size_t i = 0; i < kDays.size(); ++i) {
        const std::string& d = kDays[i];
        const std::string zc = d < "2026-04-23" ? "ZCH6" : d < "2026-04-29" ? "ZCK6" : "ZCN6";
        out.push_back(bar("ZC", d, 400.0 + i, zc));
        out.push_back(bar("ZW", d, 500.0 + 2.0 * i, d < "2026-04-28" ? "ZWH6" : "ZWK6"));
    }
    return out;
}

// The runner's span for a run dated `run_date`: the bars dated before it, its T-1 (the last of them).
std::vector<ConfirmedRoll> run_on(const std::string& run_date, const std::string& previous_run_date) {
    std::vector<Bar> consumed;
    std::string t1;
    for (const auto& b : window()) {
        const std::string d = core::format_utc_date(b.timestamp);
        if (d >= run_date) continue;
        consumed.push_back(b);
        if (d > t1) t1 = d;
    }
    return rolls_confirmed_in(consumed, live_legs_since(previous_run_date, t1), t1);
}

}  // namespace

// Daily runs with Thu 04-30 MISSED: every roll is booked exactly once, the Friday confirm on the
// Monday run across the weekend, both rolls confirmed inside the catch-up on the Friday run after
// the missed day (ZW's confirming bar is not that run's T-1: a T-1-only gate never booked it).
TEST(LiveRollLegs, EveryRollIsBookedOnceAcrossAWeekendAndAMissedRun) {
    const std::vector<std::string> runs = {"2026-04-21", "2026-04-22", "2026-04-23", "2026-04-24",
                                           "2026-04-27", "2026-04-28", "2026-04-29",
                                           /* 2026-04-30 missed */ "2026-05-01"};
    std::map<std::pair<std::string, std::string>, std::vector<std::string>> booked_on;
    std::string previous;
    for (const auto& r : runs) {
        for (const auto& roll : run_on(r, previous)) booked_on[{roll.symbol, roll.confirm_date}].push_back(r);
        previous = r;
    }
    using Key = std::pair<std::string, std::string>;
    const std::map<Key, std::vector<std::string>> want = {
        {{"ZC", "2026-04-24"}, {"2026-04-27"}},
        {{"ZC", "2026-04-30"}, {"2026-05-01"}},
        {{"ZW", "2026-04-29"}, {"2026-05-01"}}};
    EXPECT_EQ(booked_on, want);
}

// The legs' prices, contracts and the id date: the closing leg at the last close before the change
// bar in the outgoing contract, the opening leg at the change bar's close in the incoming one; the
// id carries the CONFIRMING bar's date (D3), not the run's.
TEST(LiveRollLegs, TheCatchUpRunPricesEachRollOnItsOwnBarsAndDatesItsIdsByTheConfirmingBar) {
    const auto rolls = run_on("2026-05-01", "2026-04-29");
    ASSERT_EQ(rolls.size(), 2u);
    const auto& zc = rolls[0].symbol == "ZC" ? rolls[0] : rolls[1];
    const auto& zw = rolls[0].symbol == "ZW" ? rolls[0] : rolls[1];
    EXPECT_EQ(zc.outgoing_id, "ZCK6");
    EXPECT_EQ(zc.incoming_id, "ZCN6");
    EXPECT_DOUBLE_EQ(zc.closing_price, 406.0);  // Tue 04-28, the last ZCK6 close
    EXPECT_DOUBLE_EQ(zc.opening_price, 407.0);  // Wed 04-29, the change bar
    EXPECT_EQ(zw.outgoing_id, "ZWH6");
    EXPECT_EQ(zw.incoming_id, "ZWK6");
    EXPECT_DOUBLE_EQ(zw.closing_price, 510.0);  // Mon 04-27
    EXPECT_DOUBLE_EQ(zw.opening_price, 512.0);  // Tue 04-28
    EXPECT_EQ(zw.confirm_date, "2026-04-29");
    EXPECT_EQ("EXEC_ZW_" + compact_date(zw.confirm_date) + "_RC", "EXEC_ZW_20260429_RC");
}

// A re-run of a date books that date's rolls again (its first run's rows are swept by type before
// the re-run stores its own), and a change bar alone (Thu 04-23 as the T-1) books nothing.
TEST(LiveRollLegs, AReRunBooksTheSameRollsAndAChangeBarAloneBooksNone) {
    const auto first = run_on("2026-04-27", "2026-04-24");
    const auto again = run_on("2026-04-27", "2026-04-24");
    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].confirm_date, first[0].confirm_date);
    EXPECT_TRUE(run_on("2026-04-24", "2026-04-23").empty());
}

// The first run of a book (no previous row) books the rolls its T-1 bar confirms and no earlier one.
TEST(LiveRollLegs, TheFirstRunBooksOnlyItsT1Confirms) {
    const auto rolls = run_on("2026-05-01", "");
    ASSERT_EQ(rolls.size(), 1u);
    EXPECT_EQ(rolls[0].symbol, "ZC");
    EXPECT_EQ(rolls[0].confirm_date, "2026-04-30");
    EXPECT_EQ(live_legs_since("", "2026-04-30"), "2026-04-29");
    EXPECT_EQ(live_legs_since("2026-04-27", "2026-04-24"), "2026-04-26");
}
