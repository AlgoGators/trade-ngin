// tests/core/test_stat_quality.cpp
//
// The STAT_QUALITY log lines (T-8D adversary F10, T-8D-2 R82), the text itself: the line of a
// live finalize, the line of one backtest date, and a run's record and summary lines. Every test
// here calls the functions that format the lines; the lines as a run emits them are read from
// its captured log in tests/live/test_live_pnl_manager.cpp and
// tests/backtest/test_backtest_statistics_grid.cpp.

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "trade_ngin/core/stat_quality.hpp"

using namespace trade_ngin;

TEST(StatQuality, AFinalizeLineGivesTheHeldCountTheNoBarCountAndTheListInSymbolOrder) {
    EXPECT_EQ(stat_quality::finalize_line(21, {"ZW.v.0", "ZL.v.0", "ZS.v.0", "ZM.v.0"}),
              "STAT_QUALITY finalize held=21 no_bar=4: ZL.v.0, ZM.v.0, ZS.v.0, ZW.v.0");
    EXPECT_EQ(stat_quality::finalize_line(3, {"KE.v.0"}), "STAT_QUALITY finalize held=3 no_bar=1: KE.v.0");
}

TEST(StatQuality, AFinalizeWithEveryHeldSymbolPrintingSaysZeroAndListsNothing) {
    EXPECT_EQ(stat_quality::finalize_line(15, {}), "STAT_QUALITY finalize held=15 no_bar=0");
    EXPECT_EQ(stat_quality::finalize_line(0, {}), "STAT_QUALITY finalize held=0 no_bar=0");
}

TEST(StatQuality, ADayLineGivesTheDateTheCountsTheGridVerdictAndTheList) {
    const stat_quality::Day day{"2024-09-02", 21, {"ZS.v.0", "ZC.v.0"}, 2};
    EXPECT_EQ(stat_quality::day_line(day, true),
              "STAT_QUALITY date=2024-09-02 held=21 no_bar=2 grid=yes: ZC.v.0, ZS.v.0");
    EXPECT_EQ(stat_quality::day_line(day, false),
              "STAT_QUALITY date=2024-09-02 held=21 no_bar=2 grid=no: ZC.v.0, ZS.v.0");
}

// Five dates: one holds nothing (not a held day), two hold a book that prints in full, two hold a
// symbol with no bar. ZS is held by two sleeves on the second of them: one symbol, two rows.
TEST(StatQuality, ARunRecordCountsHeldDaysNoBarDaysGridDaysRowsAndDistinctSymbols) {
    stat_quality::RunRecord record;
    record.note_day("2024-08-29", 0, {}, 0);
    record.note_day("2024-08-30", 3, {}, 0);
    record.note_day("2024-09-01", 3, {"ZS.v.0", "ZC.v.0"}, 2);
    record.note_day("2024-09-02", 3, {"ZS.v.0"}, 2);
    record.note_day("2024-09-03", 2, {}, 0);

    EXPECT_EQ(record.held_days(), 4u);
    ASSERT_EQ(record.days().size(), 2u);

    // The grid holds the Monday and not the Sunday.
    const auto lines = record.lines([](const std::string& date) { return date == "2024-09-02"; });
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0],
              "STAT_QUALITY run no_bar_days=2 of 4 held days; grid_days=1 rows=4 symbols=2: the "
              "stored dates on which a held symbol printed no bar (held, marked at last close), "
              "and how many of them are dates of the statistics grid");
    EXPECT_EQ(lines[1], "STAT_QUALITY date=2024-09-01 held=3 no_bar=2 grid=no: ZC.v.0, ZS.v.0");
    EXPECT_EQ(lines[2], "STAT_QUALITY date=2024-09-02 held=3 no_bar=1 grid=yes: ZS.v.0");
}

TEST(StatQuality, ARunWithNoSuchDateEmitsTheRunLineAloneAndAClearedRecordIsEmpty) {
    stat_quality::RunRecord record;
    record.note_day("2026-08-05", 12, {}, 0);
    record.note_day("2026-08-06", 12, {}, 0);
    auto lines = record.lines([](const std::string&) { return true; });
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].rfind("STAT_QUALITY run no_bar_days=0 of 2 held days; grid_days=0 rows=0 symbols=0:", 0),
              0u);

    record.note_day("2026-08-07", 12, {"AAPL"}, 1);
    record.clear();
    lines = record.lines([](const std::string&) { return true; });
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].rfind("STAT_QUALITY run no_bar_days=0 of 0 held days;", 0), 0u);
}
