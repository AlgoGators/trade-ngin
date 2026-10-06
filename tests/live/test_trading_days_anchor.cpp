// tests/live/test_trading_days_anchor.cpp
//
// E2-F32 -- the annualization anchor that contradicts the book it annualizes.
//
// `trading.get_trading_days(strategy, target, portfolio)` reads
// strategy_trading_days_metadata.live_start_date and only falls back to MIN(date)
// over live_results when no row exists. So a metadata row dated AFTER the book's
// first day is worse than no row at all, and the function cannot tell -- it stops
// looking as soon as it finds one.
//
// Measured on the live database 2026-09-03:
//   EQUITY_MR_PORTFOLIO / LIVE_EQUITY_MEAN_REVERSION
//     live_start_date  2026-07-24   (seeded by hand on 2026-09-01 for a 10-day window)
//     live_results     2026-04-01 .. 2026-08-04, 126 rows
//   -> every date before 07-24 gets 1 trading day, so total_annualized_return
//      collapses onto total_cumulative_return on 115 rows; 07-27 gets 4 days and
//      a -3.5 % cumulative return annualizes to -1674 %.
//
// Pure: no database, no I/O.

#include <gtest/gtest.h>

#include <string>

#include "trade_ngin/live/trading_days_anchor.hpp"

using namespace trade_ngin;

// The live defect, stated as data.
TEST(TradingDaysAnchor, AMetadataRowLaterThanTheBookIsCaught) {
    const auto a = assess_trading_days_anchor("2026-07-24", "2026-04-01");

    ASSERT_TRUE(a.has_metadata);
    EXPECT_TRUE(a.anchor_is_late)
        << "an anchor 114 days after the book's first day is the E2-F32 shape";
    EXPECT_EQ(a.effective_anchor, "2026-04-01")
        << "annualization must anchor to the book, not to a hand-seeded row";

    // What the DB function returns today vs what it should: on 2026-07-27 the
    // stored anchor gives 4 days, which is what produced -1674 %.
    EXPECT_EQ(trading_days_from_anchor(a.metadata_anchor, "2026-07-27"), 4);
    EXPECT_EQ(trading_days_from_anchor(a.effective_anchor, "2026-07-27"), 118);

    // And on any date before the stored anchor the function floors at 1, which is
    // why 115 rows show annualized == cumulative rather than an obvious error.
    EXPECT_EQ(trading_days_from_anchor(a.metadata_anchor, "2026-05-01"), 1);
    EXPECT_EQ(trading_days_from_anchor(a.effective_anchor, "2026-05-01"), 31);
}

// The sound case must stay silent, or the check is noise on every futures run.
TEST(TradingDaysAnchor, AnAnchorOnTheBooksFirstDayIsNotFlagged) {
    // LIVE_TREND_FOLLOWING / CONSERVATIVE_PORTFOLIO as of 2026-09-03: anchor and
    // first live_results row are both 2025-10-05.
    const auto a = assess_trading_days_anchor("2025-10-05", "2025-10-05");
    EXPECT_TRUE(a.has_metadata);
    EXPECT_FALSE(a.anchor_is_late) << "equal dates are the correct, seeded shape";
    EXPECT_EQ(a.effective_anchor, "2025-10-05");
}

// An anchor EARLIER than the first result is normal -- a book seeded before its
// first run -- and annualizes conservatively. Flagging it would train operators
// to ignore the warning.
TEST(TradingDaysAnchor, AnAnchorBeforeTheBookIsAcceptedAsSeeded) {
    const auto a = assess_trading_days_anchor("2025-10-01", "2025-10-05");
    EXPECT_FALSE(a.anchor_is_late);
    EXPECT_EQ(a.effective_anchor, "2025-10-01")
        << "the seeded date wins: it says when the strategy went live, which is the "
           "quantity being annualized over";
}

// No metadata row: the DB function's own MIN(date) fallback is already right, so
// the check must not invent an override.
TEST(TradingDaysAnchor, NoMetadataRowDefersToTheBook) {
    const auto a = assess_trading_days_anchor("", "2026-04-01");
    EXPECT_FALSE(a.has_metadata);
    EXPECT_FALSE(a.anchor_is_late);
    EXPECT_EQ(a.effective_anchor, "2026-04-01");
}

// A genuine first run: a seeded anchor and no history at all. There is nothing to
// contradict, and calling that a defect would fire on every new strategy.
TEST(TradingDaysAnchor, AFirstRunWithNoHistoryTrustsTheSeededAnchor) {
    const auto a = assess_trading_days_anchor("2026-09-01", "");
    EXPECT_TRUE(a.has_metadata);
    EXPECT_FALSE(a.anchor_is_late);
    EXPECT_EQ(a.effective_anchor, "2026-09-01");
}

// Neither source: the function returns 1 and so must this, rather than a zero or
// a negative that would divide badly downstream.
TEST(TradingDaysAnchor, NothingKnownYieldsOneDay) {
    const auto a = assess_trading_days_anchor("", "");
    EXPECT_TRUE(a.effective_anchor.empty());
    EXPECT_EQ(trading_days_from_anchor(a.effective_anchor, "2026-09-03"), 1);
}

// The count must be the SAME quantity the SQL computes, or an override would
// silently redefine the denominator instead of correcting its anchor.
// SQL: GREATEST(1, (p_target_date - v_start_date) + 1), whole calendar days.
TEST(TradingDaysAnchor, TheCountReproducesTheSqlExactly) {
    EXPECT_EQ(trading_days_from_anchor("2026-09-03", "2026-09-03"), 1) << "same day is 1";
    EXPECT_EQ(trading_days_from_anchor("2026-09-02", "2026-09-03"), 2) << "inclusive of both ends";
    EXPECT_EQ(trading_days_from_anchor("2025-10-05", "2026-05-03"), 211)
        << "the futures figure migration 004's verification block pins";
    // Calendar days, so weekends and holidays count -- exactly like date subtraction.
    EXPECT_EQ(trading_days_from_anchor("2026-08-07", "2026-08-10"), 4) << "Fri to Mon";
    // Leap year handled by the same UTC arithmetic the freshness check uses.
    EXPECT_EQ(trading_days_from_anchor("2024-02-28", "2024-03-01"), 3);
    EXPECT_EQ(trading_days_from_anchor("2026-02-28", "2026-03-01"), 2);
    // A target BEFORE the anchor floors at 1, matching GREATEST(1, ...), rather
    // than going negative and inverting the sign of every annualized return.
    EXPECT_EQ(trading_days_from_anchor("2026-09-03", "2026-01-01"), 1);
    // Malformed input is the function's "I do not know" answer, not a huge count.
    EXPECT_EQ(trading_days_from_anchor("garbage", "2026-09-03"), 1);
    EXPECT_EQ(trading_days_from_anchor("2026-09-03", ""), 1);
}
