// E2-F54 regression pins -- the backtest's per-bar realized FLOW must come from the
// fill-maintained record, must be computed before the flat-row skip, and must keep the
// close-day row.
//
// THE DEFECT (independent audit 2026-09-04, mechanics verified on a stored run):
//
//   (a) The row snapshot the coordinator iterates is `get_strategy_positions()` ==
//       PortfolioManager::current_positions == the TARGET copy taken at
//       portfolio_manager.cpp:230/:439, which is produced BEFORE on_execution is fed
//       (backtest_coordinator.cpp:669-672). So `pos.realized_pnl` on bar D carries realized
//       through D-1's fills only: a sale on D lands on D+1's row.
//
//   (b) On a full close `std::abs(qty) < 1e-8` continues (backtest_coordinator.cpp:743)
//       BEFORE the flow code (:785-791), so `last_cumulative_realized_` is never advanced
//       for the closing bar and the exit's realized is never emitted. It is not merely
//       late -- it is STRANDED until the symbol is re-entered (measured: ABT closes 09-11,
//       its realized surfaces on the 12-17 row) or LOST if the symbol never trades again.
//
//   (c) `last_cumulative_realized_` is not cleared in reset_portfolio_state(), so a second
//       portfolio backtest in one process opens with the previous book's cumulative and
//       writes a wrong first-bar flow.
//
// Present since 165ff068 (2026-05-22); 6401e2ed inherited it and its commit message claims
// the rows "match the live definition", which is what this pins. No consumer reads the
// column, so backtest.results, equity_curve and the metrics are unaffected -- this is a
// stored-row correctness defect, not a P&L defect.
//
// The rule lives in BacktestPnLManager next to unrealized_for_accounting because that is
// where the other per-row accounting rule and its rationale already live, and because the
// coordinator's loop cannot be exercised without a database.
//
// The wiring -- that the coordinator passes the FILL-maintained cumulative and calls this
// before the flat-row skip -- is not expressible in a unit test (it is a call-site
// argument). Run R1 is its evidence: every (date, symbol) flow re-derived from that date's
// executions against the running basis.

#include <gtest/gtest.h>

#include "trade_ngin/backtest/backtest_pnl_manager.hpp"

using trade_ngin::backtest::BacktestPnLManager;

// ---------------------------------------------------------------------------
// The live is_dead_row rule, restated for the backtest.
// AVERAGE_PRICE_LIFECYCLE step 10 / LiveDailyCycle::is_dead_row: a row dies only when it
// has NEITHER quantity NOR realized. A closed position that realized something keeps its
// row so the exit's P&L has somewhere to live.
// ---------------------------------------------------------------------------

TEST(BacktestRealizedFlow, AFlatRowWithNoRealizedIsDead) {
    EXPECT_TRUE(BacktestPnLManager::is_dead_row(0.0, 0.0));
}

TEST(BacktestRealizedFlow, AFlatRowThatRealizedSomethingIsNotDead) {
    // This is the close-day row. Dropping it strands the exit's realized.
    EXPECT_FALSE(BacktestPnLManager::is_dead_row(0.0, 200.0));
    EXPECT_FALSE(BacktestPnLManager::is_dead_row(0.0, -200.0));
}

TEST(BacktestRealizedFlow, AHeldRowIsNeverDead) {
    EXPECT_FALSE(BacktestPnLManager::is_dead_row(40.0, 0.0));
    EXPECT_FALSE(BacktestPnLManager::is_dead_row(-40.0, 0.0));
}

// ---------------------------------------------------------------------------
// The flow itself.
// ---------------------------------------------------------------------------

TEST(BacktestRealizedFlow, TheSaleLandsOnItsOwnBarNotTheNext) {
    // buy 40 @ 100 on D-3 (realizes nothing), sell 40 @ 105 on D (realizes +200).
    double last = 0.0;

    // D-3 .. D-1: the fill-maintained cumulative is still 0.
    auto d_minus_3 = BacktestPnLManager::realized_row_for_bar(40.0, 0.0, last);
    EXPECT_DOUBLE_EQ(d_minus_3.flow, 0.0);
    EXPECT_TRUE(d_minus_3.keep) << "a held row is always written";

    // D: on_execution has booked +200, so the fill-maintained cumulative is 200 and the
    // quantity is now flat. The flow must be +200 ON THIS BAR.
    auto d = BacktestPnLManager::realized_row_for_bar(0.0, 200.0, last);
    EXPECT_DOUBLE_EQ(d.flow, 200.0) << "the exit's realized belongs to the bar it happened on";
    EXPECT_TRUE(d.keep) << "the close-day row must survive -- this is where +200 lives";

    // D+1: nothing more realized, still flat -> dead row, and NOT a repeat of +200.
    auto d_plus_1 = BacktestPnLManager::realized_row_for_bar(0.0, 200.0, last);
    EXPECT_DOUBLE_EQ(d_plus_1.flow, 0.0) << "the running total must not be re-emitted";
    EXPECT_FALSE(d_plus_1.keep) << "flat and nothing realized -> no row";
}

TEST(BacktestRealizedFlow, APartialSaleLandsOnItsOwnDay) {
    double last = 0.0;
    // hold 100, sell 40 on D realizing +200: still held, flow +200 on D.
    auto d = BacktestPnLManager::realized_row_for_bar(60.0, 200.0, last);
    EXPECT_DOUBLE_EQ(d.flow, 200.0);
    EXPECT_TRUE(d.keep);
    // D+1 sells the rest realizing a further +90 -> cumulative 290, flow 90.
    auto d1 = BacktestPnLManager::realized_row_for_bar(0.0, 290.0, last);
    EXPECT_DOUBLE_EQ(d1.flow, 90.0);
    EXPECT_TRUE(d1.keep);
}

TEST(BacktestRealizedFlow, FlowsSumToTheStrategyCumulative) {
    // The invariant R1 checks against the database: sum of the stored flows over dates
    // equals the strategy's final cumulative for that symbol, including a symbol that is
    // flat at the end.
    double last = 0.0;
    const double cumulative_by_bar[] = {0.0, 0.0, 200.0, 200.0, 290.0, 290.0};
    double summed = 0.0;
    const double qty_by_bar[] = {40.0, 40.0, 0.0, 0.0, 0.0, 0.0};
    for (int i = 0; i < 6; ++i) {
        auto r = BacktestPnLManager::realized_row_for_bar(qty_by_bar[i], cumulative_by_bar[i], last);
        if (r.keep) summed += r.flow;
    }
    EXPECT_DOUBLE_EQ(summed, 290.0) << "flows must reconstruct the cumulative exactly";
}

TEST(BacktestRealizedFlow, ALossIsAFlowLikeAnyOther) {
    double last = 0.0;
    auto d = BacktestPnLManager::realized_row_for_bar(0.0, -125.5, last);
    EXPECT_DOUBLE_EQ(d.flow, -125.5);
    EXPECT_TRUE(d.keep);
}

// ---------------------------------------------------------------------------
// (c) reset semantics. reset_portfolio_state() must clear the ledger; if it does not, the
// next portfolio in the same process opens with this one's cumulative and its first bar
// reports the difference instead of the whole figure.
// ---------------------------------------------------------------------------

TEST(BacktestRealizedFlow, AClearedLedgerReportsTheWholeCumulativeNotAnIncrement) {
    double last = 0.0;
    BacktestPnLManager::realized_row_for_bar(0.0, 200.0, last);
    EXPECT_DOUBLE_EQ(last, 200.0);

    // What reset_portfolio_state() must do.
    last = 0.0;

    auto first_bar_of_the_next_run =
        BacktestPnLManager::realized_row_for_bar(0.0, 75.0, last);
    EXPECT_DOUBLE_EQ(first_bar_of_the_next_run.flow, 75.0)
        << "an uncleared ledger would report 75 - 200 = -125 here";
}
