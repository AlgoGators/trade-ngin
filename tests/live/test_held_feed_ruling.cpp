// tests/live/test_held_feed_ruling.cpp
//
// T-DATED (HD 2026-10-10). Every case here EXECUTES the function the live runners call; none
// reads source text.
//
//   R1  the held-hole refusal is for a run made on the host's own date; the same date run on
//       another day is a warning. The boundaries: the run date equal to, one day before and one
//       day after the host's date, and a run that crosses local midnight before the test.
//   R2a a HELD symbol with no bar anywhere in the loaded history is a hole past every tolerance.
//   R2b a requested symbol the book does not hold stops nothing: one line names it.
//   R2c an empty feed refuses every run; a requested symbol with no bar, and a feed staler than
//       the tolerance, refuse a run with no date; on a dated run the one WARN is the held check's.

#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/live/data_freshness.hpp"
#include "trade_ngin/live/session_book_gate.hpp"

using namespace trade_ngin;

namespace {

// Local midnight of a date, as the runners parse a date argument (std::mktime).
Timestamp local_midnight(int y, int m, int d, int hour = 0, int minute = 0, int second = 0) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

SymbolDayVerdict no_bar(const std::string& symbol, int age, const std::string& last_bar) {
    SymbolDayVerdict v;
    v.symbol = symbol;
    v.date = "2026-04-25";
    v.has_bar = false;
    v.hole_age_days = age;
    v.last_bar_date = last_bar;
    return v;
}

SymbolDayVerdict printed(const std::string& symbol) {
    SymbolDayVerdict v;
    v.symbol = symbol;
    v.date = "2026-04-25";
    v.has_bar = true;
    v.verdict = SessionVerdict::SESSION;
    return v;
}

Position held(const std::string& symbol, double qty) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    return p;
}

const std::vector<HeldFeedHole> kOneHole = {{"6M.v.0", 5, "2026-04-20", 1.0}};

}  // namespace

// ---------------------------------------------------------------------------------------------
// R1: the host-date boundaries
// ---------------------------------------------------------------------------------------------

TEST(HeldFeedRuling, ARunOnTheHostsOwnDateRefusesWithOneErrorLineAHole) {
    const auto run = local_midnight(2026, 4, 26);
    const auto ruling = rule_held_feed_holes(kOneHole, 4, run, local_midnight(2026, 4, 26, 9, 30));
    EXPECT_TRUE(ruling.refuse);
    ASSERT_EQ(ruling.lines.size(), 1u);
    EXPECT_EQ(ruling.lines[0],
              "FEED_HOLE_REFUSAL held symbol 6M.v.0 (quantity 1.000000) has had no bar for 5 "
              "day(s) (last bar 2026-04-20), past live.data_staleness_tolerance_days=4. Refusing "
              "to run live: refresh the OHLCV feed, then run this date.");
}

TEST(HeldFeedRuling, TheSameDateRunOneDayLateIsAWarningNotARefusal) {
    const auto run = local_midnight(2026, 4, 26);
    const auto ruling = rule_held_feed_holes(kOneHole, 4, run, local_midnight(2026, 4, 27, 9, 30));
    EXPECT_FALSE(ruling.refuse);
    ASSERT_EQ(ruling.lines.size(), 1u);
    EXPECT_EQ(ruling.lines[0],
              "FEED_HOLE_REFUSAL held symbol 6M.v.0 (quantity 1.000000) has had no bar for 5 "
              "day(s) (last bar 2026-04-20), past live.data_staleness_tolerance_days=4. Replay of "
              "a past date: not refused.");
}

TEST(HeldFeedRuling, ADateOneDayAheadOfTheHostIsNotRefused) {
    const auto run = local_midnight(2026, 4, 26);
    EXPECT_FALSE(rule_held_feed_holes(kOneHole, 4, run, local_midnight(2026, 4, 25, 9, 30)).refuse);
}

TEST(HeldFeedRuling, TheFirstAndTheLastSecondOfTheHostsDateBothRefuse) {
    const auto run = local_midnight(2026, 4, 26);
    EXPECT_TRUE(rule_held_feed_holes(kOneHole, 4, run, local_midnight(2026, 4, 26, 0, 0, 0)).refuse);
    EXPECT_TRUE(
        rule_held_feed_holes(kOneHole, 4, run, local_midnight(2026, 4, 26, 23, 59, 59)).refuse);
    EXPECT_FALSE(
        rule_held_feed_holes(kOneHole, 4, run, local_midnight(2026, 4, 25, 23, 59, 59)).refuse);
}

TEST(HeldFeedRuling, ARunThatCrossesLocalMidnightBeforeTheTestIsRuledAsMadeOnAnotherDay) {
    // The host clock is read when the test is reached. A run started at 23:59:50 for its own
    // date that reaches the test five seconds after midnight is a run made on the next day: a
    // warning, the symbols held at their last price. Pinned as it is (HD 2026-10-10, R1).
    const auto run = local_midnight(2026, 4, 26);
    EXPECT_FALSE(rule_held_feed_holes(kOneHole, 4, run, local_midnight(2026, 4, 27, 0, 0, 5)).refuse);
}

TEST(HeldFeedRuling, NoHeldHoleRulesNothingOnAnyDate) {
    const auto run = local_midnight(2026, 4, 26);
    const auto ruling = rule_held_feed_holes({}, 4, run, run);
    EXPECT_FALSE(ruling.refuse);
    EXPECT_TRUE(ruling.lines.empty());
}

// ---------------------------------------------------------------------------------------------
// R2a, R2b: what is held, and what is not
// ---------------------------------------------------------------------------------------------

TEST(HeldFeedRuling, AHeldSymbolWithNoBarAnywhereIsAHolePastEveryTolerance) {
    T1Classification t1;
    t1.verdicts = {no_bar("GONE.v.0", -1, ""), no_bar("OLD.v.0", 7, "2026-04-18"),
                   no_bar("FRESH.v.0", 2, "2026-04-23"), printed("MES.v.0")};
    const std::unordered_map<std::string, Position> book = {
        {"GONE.v.0", held("GONE.v.0", 2.0)}, {"OLD.v.0", held("OLD.v.0", -1.0)},
        {"FRESH.v.0", held("FRESH.v.0", 1.0)}, {"MES.v.0", held("MES.v.0", 1.0)}};

    EXPECT_TRUE(any_no_bar_past_tolerance(t1, 4));
    EXPECT_TRUE(any_no_bar_past_tolerance(t1, 1000)) << "no bar anywhere has no age to tolerate";
    const auto holes = held_feed_holes_past_tolerance(t1, book, 4);
    ASSERT_EQ(holes.size(), 2u);
    EXPECT_EQ(holes[0].symbol, "GONE.v.0");
    EXPECT_EQ(holes[1].symbol, "OLD.v.0");

    const auto ruling = rule_held_feed_holes(holes, 4, local_midnight(2026, 4, 26),
                                             local_midnight(2026, 4, 26, 9, 30));
    EXPECT_TRUE(ruling.refuse);
    EXPECT_NE(ruling.lines[0].find("held symbol GONE.v.0 (quantity 2.000000) has had no bar "
                                   "anywhere in the loaded history, past"),
              std::string::npos)
        << ruling.lines[0];
}

TEST(HeldFeedRuling, EverySymbolPrintedMeansTheHeldBookIsNotEvenRead) {
    T1Classification t1;
    t1.verdicts = {printed("MES.v.0"), no_bar("SAT.v.0", 1, "2026-04-24")};
    EXPECT_FALSE(any_no_bar_past_tolerance(t1, 4));
}

TEST(HeldFeedRuling, WhatTheBookDoesNotHoldStopsNothingAndIsNamedInOneLine) {
    T1Classification t1;
    t1.verdicts = {no_bar("ZW.v.0", 6, "2026-04-19"), no_bar("GONE.v.0", -1, ""),
                   no_bar("HELD.v.0", 9, "2026-04-16"), no_bar("FLAT.v.0", 5, "2026-04-20"),
                   printed("MES.v.0")};
    const std::unordered_map<std::string, Position> book = {{"HELD.v.0", held("HELD.v.0", 1.0)},
                                                            {"FLAT.v.0", held("FLAT.v.0", 0.0)}};
    EXPECT_EQ(unheld_no_bar_line(t1, book, 4),
              "FEED_HOLE_NOT_HELD 3 requested symbol(s) the book does not hold are past "
              "live.data_staleness_tolerance_days=4 and cannot be entered today: FLAT.v.0 (no bar "
              "for 5 day(s) (last bar 2026-04-20)); GONE.v.0 (no bar anywhere in the loaded "
              "history); ZW.v.0 (no bar for 6 day(s) (last bar 2026-04-19));");
    const auto holes = held_feed_holes_past_tolerance(t1, book, 4);
    ASSERT_EQ(holes.size(), 1u);
    EXPECT_EQ(holes[0].symbol, "HELD.v.0");

    T1Classification clean;
    clean.verdicts = {printed("MES.v.0")};
    EXPECT_EQ(unheld_no_bar_line(clean, book, 4), "");
}

TEST(HeldFeedRuling, TheEquityRunnersSelectionFromItsNewestBarsFollowsTheSameRule) {
    const std::vector<std::string> requested = {"AAA", "BBB", "CCC", "DDD", "EEE"};
    const std::unordered_map<std::string, std::string> last_bar = {
        {"AAA", "2026-06-15"},   // current
        {"BBB", "2026-06-08"},   // 7 days behind, held
        {"CCC", "2026-06-11"},   // 4 days behind: inside the tolerance (a weekend and a holiday)
        {"DDD", "2026-06-01"}};  // 14 days behind, not held; EEE: no bar at all, held
    const std::unordered_map<std::string, double> held_qty = {
        {"AAA", 10.0}, {"BBB", 5.0}, {"CCC", 3.0}, {"EEE", 7.0}};
    const auto holes = holes_from_last_bars(requested, last_bar, "2026-06-15", held_qty, 4);
    ASSERT_EQ(holes.held.size(), 2u);
    EXPECT_EQ(holes.held[0].symbol, "BBB");
    EXPECT_EQ(holes.held[0].age_days, 7);
    EXPECT_EQ(holes.held[0].last_bar_date, "2026-06-08");
    EXPECT_EQ(holes.held[1].symbol, "EEE");
    EXPECT_LT(holes.held[1].age_days, 0);
    EXPECT_EQ(holes.not_held_line,
              "FEED_HOLE_NOT_HELD 1 requested symbol(s) the book does not hold are past "
              "live.data_staleness_tolerance_days=4 and cannot be entered today: DDD (no bar for "
              "14 day(s) (last bar 2026-06-01));");
    // the same ruling as the futures runners
    EXPECT_TRUE(rule_held_feed_holes(holes.held, 4, local_midnight(2026, 6, 16),
                                     local_midnight(2026, 6, 16, 9, 30))
                    .refuse);
    EXPECT_FALSE(rule_held_feed_holes(holes.held, 4, local_midnight(2026, 6, 16),
                                      local_midnight(2026, 6, 17, 9, 30))
                     .refuse);
}

// ---------------------------------------------------------------------------------------------
// R2c: the feed-freshness arms, on a dated run and on a run with no date
// ---------------------------------------------------------------------------------------------

TEST(FeedFreshnessRuling, AnEmptyFeedRefusesADatedRunAndARunWithNoDate) {
    const std::unordered_map<std::string, std::string> unparsable = {{"MES.v.0", "not-a-date"}};
    const auto f = assess_feed_freshness(unparsable, "2026-04-25");
    ASSERT_FALSE(f.any_data);
    for (bool dated : {true, false}) {
        const auto ruling = rule_feed_freshness(f, 4, "2026-04-25", dated, "Futures");
        EXPECT_TRUE(ruling.refuse) << "dated=" << dated;
        ASSERT_EQ(ruling.errors.size(), 1u);
        EXPECT_EQ(ruling.errors[0],
                  "Futures data freshness cannot be established: none of the 1 loaded symbols "
                  "carries a usable bar date as of 2026-04-25. Refusing to run live without a "
                  "feed. Refresh the OHLCV feed.");
    }
}

TEST(FeedFreshnessRuling, ARequestedSymbolWithNoBarIsLeftToTheHeldCheckOnADatedRun) {
    const std::unordered_map<std::string, std::string> bars = {{"MES.v.0", "2026-04-25"}};
    const auto f = assess_feed_freshness(bars, "2026-04-25", {"MES.v.0", "GONE.v.0"});
    ASSERT_EQ(f.absent, 1u);

    // One WARN names such a symbol on a dated run, and it is the held check's
    // (FEED_HOLE_NOT_HELD): this ruling adds none.
    const auto dated = rule_feed_freshness(f, 4, "2026-04-25", true, "Futures");
    EXPECT_FALSE(dated.refuse);
    EXPECT_TRUE(dated.warnings.empty());
    EXPECT_TRUE(dated.errors.empty());
    EXPECT_FALSE(dated.info.empty()) << "the rest of the feed is still measured";

    const auto no_date = rule_feed_freshness(f, 4, "2026-04-25", false, "Futures");
    EXPECT_TRUE(no_date.refuse);
    ASSERT_EQ(no_date.errors.size(), 1u);
    EXPECT_NE(no_date.errors[0].find("Refusing to run live with an incomplete universe."),
              std::string::npos);
}

TEST(FeedFreshnessRuling, AStaleFeedIsOneInfoLineOnADatedRunAndRefusesARunWithNoDate) {
    const std::unordered_map<std::string, std::string> bars = {{"MES.v.0", "2026-04-25"},
                                                               {"6M.v.0", "2026-04-20"}};
    const auto f = assess_feed_freshness(bars, "2026-04-25", {"MES.v.0", "6M.v.0"});
    ASSERT_EQ(f.days_behind, 5);

    const auto dated = rule_feed_freshness(f, 4, "2026-04-25", true, "Futures");
    EXPECT_FALSE(dated.refuse);
    EXPECT_TRUE(dated.warnings.empty());
    EXPECT_EQ(dated.info,
              "Futures data is stale: the stalest of 2 symbols (6M.v.0) last printed 2026-04-20, "
              "5 calendar days before 2026-04-25 (tolerance 4 days). A dated run stops only for a "
              "symbol the book holds: the held-symbol check names each one.");
    EXPECT_EQ(dated.info.find("replay"), std::string::npos)
        << "the line must be true on a run made on its own date";

    const auto no_date = rule_feed_freshness(f, 4, "2026-04-25", false, "Equity");
    EXPECT_TRUE(no_date.refuse);
    EXPECT_NE(no_date.errors[0].find("Equity data is stale"), std::string::npos);
    EXPECT_NE(no_date.errors[0].find("Refusing to run live on stale data."), std::string::npos);
}

TEST(FeedFreshnessRuling, ADatedRunsOneWarningForWhatTheBookDoesNotHoldIsTheHeldChecks) {
    // The two forms of R2b through the functions the runners call in order: a symbol stale past
    // the tolerance and a requested symbol with no bar in the window. Counted together: one WARN.
    T1Classification t1;
    t1.verdicts = {no_bar("6M.v.0", 5, "2026-04-20"), no_bar("GC.v.0", 753, "2024-03-31"),
                   printed("MES.v.0")};
    const std::unordered_map<std::string, Position> book = {{"MES.v.0", held("MES.v.0", 1.0)}};
    const std::unordered_map<std::string, std::string> bars = {{"MES.v.0", "2026-04-25"},
                                                               {"6M.v.0", "2026-04-20"}};
    const auto f = assess_feed_freshness(bars, "2026-04-25", {"MES.v.0", "6M.v.0", "GC.v.0"});
    const auto ruling = rule_feed_freshness(f, 4, "2026-04-25", true, "Futures");
    const std::string not_held = unheld_no_bar_line(t1, book, 4);
    EXPECT_EQ(ruling.warnings.size() + (not_held.empty() ? 0u : 1u), 1u);
    EXPECT_NE(not_held.find("6M.v.0 (no bar for 5 day(s) (last bar 2026-04-20))"),
              std::string::npos);
    EXPECT_NE(not_held.find("GC.v.0 (no bar for 753 day(s) (last bar 2024-03-31))"),
              std::string::npos);
}

TEST(FeedFreshnessRuling, AFeedInsideTheToleranceIsOneInfoLineOnEveryRun) {
    const std::unordered_map<std::string, std::string> bars = {{"MES.v.0", "2026-04-25"},
                                                               {"6M.v.0", "2026-04-21"}};
    const auto f = assess_feed_freshness(bars, "2026-04-25", {"MES.v.0", "6M.v.0"});
    for (bool dated : {true, false}) {
        const auto ruling = rule_feed_freshness(f, 4, "2026-04-25", dated, "Futures");
        EXPECT_FALSE(ruling.refuse);
        EXPECT_TRUE(ruling.warnings.empty());
        EXPECT_TRUE(ruling.errors.empty());
        EXPECT_EQ(ruling.info,
                  "Futures feed freshness: stalest of 2 symbols (6M.v.0) at 2026-04-21, 4 days "
                  "behind 2026-04-25 (tolerance 4).");
    }
}
