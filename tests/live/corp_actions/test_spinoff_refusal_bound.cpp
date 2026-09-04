// tests/live/corp_actions/test_spinoff_refusal_bound.cpp
//
// BA-24, both halves.
//
// (a) A REFUSED SPINOFF PARENT escaped the basis-vs-mark bound entirely. The runner runs one
//     bound over events that APPLIED (G1) and a second over class-1 events it REFUSED (G3,
//     E2-F17). E2-F31 suppresses the class-1 event for a spinoff bar, so a refused spinoff
//     parent is in neither list -- and it is the one shape whose basis is KNOWN to be out of
//     frame, because the runner says so itself in the refusal WARN: "carries a PRE-spinoff
//     cost basis against a POST-spinoff price series". The only case with a certain
//     unrestated basis was the only case nothing measured.
//
// (b) `child_first_close` was keyed on the CHILD ALONE and filled with the first positive
//     close in a series that starts at the BATCH's earliest ex-date. A child whose own
//     ex-date is later than that gets a close from before its own distribution, and both the
//     basis allocated to it and the realized booked when it is liquidated are struck at that
//     pre-event price. The number is a real close of the right symbol, so nothing downstream
//     can see it is the wrong one.
//
// The selection is now a pure function and is tested directly; the bound is inside the
// runner's main() and is asserted in the source, the approach
// tests/live/test_day_t_write_ordering.cpp takes for the same reason.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "trade_ngin/live/live_daily_cycle.hpp"

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

// One catch-up batch: a parent spins off on 2026-04-06, another on 2026-04-20, and the second
// child was ALREADY LISTED and printing all along. The range read starts at 2026-04-06.
const std::map<std::string, double>& already_listed_child_series() {
    static const std::map<std::string, double> s = {
        {"2026-04-06", 11.00}, {"2026-04-07", 11.50}, {"2026-04-08", 12.00},
        {"2026-04-17", 13.00}, {"2026-04-20", 27.04}, {"2026-04-21", 26.00},
    };
    return s;
}

}  // namespace

TEST(SpinoffRefusalBound, AChildIsPricedFromItsOwnExDateNotTheBatchsEarliest) {
    const auto& series = already_listed_child_series();

    // Its own ex-date. 27.04 is the distribution-day close.
    const auto own = LiveDailyCycle::first_close_on_or_after(series, "2026-04-20");
    EXPECT_EQ(own.first, "2026-04-20");
    EXPECT_DOUBLE_EQ(own.second, 27.04);

    // The batch's earliest ex-date -- what the old keying produced. A real close of the right
    // symbol, from two weeks before the event, less than half the right price.
    const auto batch = LiveDailyCycle::first_close_on_or_after(series, "2026-04-06");
    EXPECT_EQ(batch.first, "2026-04-06");
    EXPECT_DOUBLE_EQ(batch.second, 11.00);

    EXPECT_NE(own.second, batch.second)
        << "if these agree the fixture no longer exercises the defect";
}

TEST(SpinoffRefusalBound, TheExDateItselfCountsAndAGapWalksForward) {
    const auto& series = already_listed_child_series();
    // Inclusive on the ex-date.
    EXPECT_EQ(LiveDailyCycle::first_close_on_or_after(series, "2026-04-08").first, "2026-04-08");
    // A suspended or holiday ex-date walks forward to the next real bar, which is what
    // "first REAL close on or after" means.
    const auto after_gap = LiveDailyCycle::first_close_on_or_after(series, "2026-04-09");
    EXPECT_EQ(after_gap.first, "2026-04-17");
    EXPECT_DOUBLE_EQ(after_gap.second, 13.00);
}

TEST(SpinoffRefusalBound, NoUsableBarYieldsNothingRatherThanAGuess) {
    const auto& series = already_listed_child_series();
    // Past the end of the series: RAL and MRP are this shape (zero rows at all).
    const auto none = LiveDailyCycle::first_close_on_or_after(series, "2026-05-01");
    EXPECT_TRUE(none.first.empty());
    EXPECT_DOUBLE_EQ(none.second, 0.0);
    // An empty series likewise.
    EXPECT_TRUE(LiveDailyCycle::first_close_on_or_after({}, "2026-04-20").first.empty());
}

TEST(SpinoffRefusalBound, ZeroAndNonFiniteClosesAreSkippedNotDelivered) {
    // A 0 is what the loader leaves when it has nothing. Pricing a distribution at 0 gives
    // the child a zero basis and books its whole value as realized gain (the E2-F48 shape).
    const std::map<std::string, double> holes = {
        {"2026-04-20", 0.0}, {"2026-04-21", -1.0}, {"2026-04-22", 26.00}};
    const auto hit = LiveDailyCycle::first_close_on_or_after(holes, "2026-04-20");
    EXPECT_EQ(hit.first, "2026-04-22");
    EXPECT_DOUBLE_EQ(hit.second, 26.00);

    const std::map<std::string, double> all_holes = {{"2026-04-20", 0.0}};
    EXPECT_TRUE(LiveDailyCycle::first_close_on_or_after(all_holes, "2026-04-20").first.empty());
}

TEST(SpinoffRefusalBound, TheRunnerKeysTheChildCloseOnItsOwnExDate) {
    auto path = find_repo_file("apps/strategies/live_equity_mean_reversion.cpp");
    if (path.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();

    // The map is keyed on the pair, and the lookup passes the ROW's ex-date.
    EXPECT_NE(src.find("child_first_close;  // (child, ex_date) -> (date used, close)"),
              std::string::npos)
        << "child_first_close is keyed on the child alone again (BA-24)";
    EXPECT_NE(src.find("child_first_close.find({t.first, row.date_str})"), std::string::npos)
        << "the child close is looked up without its ex-date again (BA-24)";
    EXPECT_NE(src.find("LiveDailyCycle::first_close_on_or_after("), std::string::npos);
}

TEST(SpinoffRefusalBound, TheRefusedSpinoffParentIsInsideTheBasisVsMarkBound) {
    auto path = find_repo_file("apps/strategies/live_equity_mean_reversion.cpp");
    if (path.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();

    // The G3 bound iterates ONE list built from both refusal paths, and the spinoff half is
    // in it. Before BA-24 the loop ran over `events` alone, which a suppressed spinoff parent
    // is never in.
    const auto guard = src.find("E2-F17 / G3 -- THE SAME BOUND, ON THE EVENTS WE REFUSED.");
    ASSERT_NE(guard, std::string::npos) << "the G3 bound is gone entirely";
    const auto guard_end = src.find("E2-F19 cleanup", guard);
    ASSERT_NE(guard_end, std::string::npos);
    const std::string block = src.substr(guard, guard_end - guard);

    EXPECT_NE(block.find("for (const auto& adj : spinoff_log)"), std::string::npos)
        << "the bound does not inspect refused spinoffs, so the one shape with a KNOWN "
           "unrestated basis is the one shape nothing measures (BA-24)";
    EXPECT_NE(block.find("SPINOFF to {"), std::string::npos)
        << "a refused spinoff must be NAMED in the guard's message, not folded into a "
           "class-1 label it does not have";
    EXPECT_NE(block.find("SPUN_OFF_CHILD_HELD"), std::string::npos)
        << "an APPLIED spinoff must be excluded from the refusal set";
}
