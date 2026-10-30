// The backtest's per-bar realized FLOW comes from the fill-maintained record, is computed
// before the flat-row skip, and keeps the close-day row, so the rows match the live definition:
//
//   (a) The row snapshot the coordinator iterates (`get_strategy_positions()`, the TARGET copy
//       PortfolioManager::current_positions holds) is taken BEFORE on_execution is fed, so its
//       `pos.realized_pnl` on bar D carries realized through D-1's fills only. The flow reads
//       the fill-maintained cumulative, so a sale on D lands on D's row, not D+1's.
//
//   (b) On a full close the flow is computed before the flat-row skip, so the exit's realized
//       is emitted on the closing bar; skipped first, it would be STRANDED until the symbol is
//       re-entered, or LOST if the symbol never trades again.
//
//   (c) reset_portfolio_state() clears `last_cumulative_realized_`, so a second portfolio
//       backtest in one process does not open with the previous book's cumulative.
//
// No consumer reads the column, so backtest.results, equity_curve and the metrics do not
// depend on it: this is stored-row correctness, not P&L.
//
// The rule lives in BacktestPnLManager next to unrealized_for_accounting because that is
// where the other per-row accounting rule and its rationale already live, and because the
// coordinator's loop cannot be exercised without a database.
//
// The wiring -- that the coordinator passes the FILL-maintained cumulative and calls this
// before the flat-row skip -- is not expressible in a unit test (it is a call-site
// argument); a stored run checks it by re-deriving every (date, symbol) flow from that date's
// executions against the running basis.

#include <gtest/gtest.h>

#include "trade_ngin/backtest/backtest_pnl_manager.hpp"

using trade_ngin::backtest::BacktestPnLManager;

// ---------------------------------------------------------------------------
// The live is_dead_row rule, restated for the backtest.
// LiveDailyCycle::is_dead_row: a row dies only when it
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
    // The invariant a stored run is checked against: sum of the stored flows over dates
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

// ---------------------------------------------------------------------------
// The ledger advances only on a bar whose row is actually WRITTEN.
//
// The flow is computed before the flat-row check, but three paths after it return without
// writing anything:
//
//     if (curr_it == current_close_prices.end()) continue;          // no bar for this symbol
//     if (!pnl_manager_->has_previous_close(symbol)) { ...; continue; }
//     if (pnl_result.valid) { ... }                                 // else: nothing written
//
// A fill on a bar the symbol has no close for is DEFERRED: it surfaces on the next row that is
// written. Advancing the ledger on such a bar would turn that deferral into a LOSS: the ledger
// would say the realized has been
// reported, and no row would carry it; Σ over rows would under-state the position's realized.
//
// The rule this pins: `last` is the cumulative AS OF THE LAST ROW WRITTEN, not as of the last
// bar seen. A bar that writes nothing must leave it alone, and the next written row carries
// the whole span.
// ---------------------------------------------------------------------------

TEST(BacktestRealizedFlow, ASkippedBarDefersItsFlowInsteadOfLosingIt) {
    double last = 0.0;

    // Bar D: fills realize +200, but the symbol has no close on this bar, so the coordinator
    // writes no row. The ledger must NOT be advanced -- modelled here by not calling.
    // (If it were advanced, `last` would become 200 and the +200 would never reach a row.)

    // Bar D+1: the symbol prints again. Cumulative is now 250 (D's 200 plus 50 more) and the
    // row IS written. It must carry the whole 250.
    auto written = BacktestPnLManager::realized_row_for_bar(10.0, 250.0, last);
    EXPECT_DOUBLE_EQ(written.flow, 250.0)
        << "the skipped bar's 200 must surface on the next written row, not vanish";
    EXPECT_TRUE(written.keep);
    EXPECT_DOUBLE_EQ(last, 250.0) << "the ledger advances only now, on the row that was written";
}

TEST(BacktestRealizedFlow, HadTheSkippedBarAdvancedTheLedgerTheFlowWouldBeLost) {
    // The wrong ordering, stated as arithmetic: advance on the bar that wrote nothing, then
    // report only the remainder on the next row.
    double last = 0.0;
    BacktestPnLManager::realized_row_for_bar(10.0, 200.0, last);  // the bar that wrote NOTHING
    auto next = BacktestPnLManager::realized_row_for_bar(10.0, 250.0, last);
    EXPECT_DOUBLE_EQ(next.flow, 50.0);
    EXPECT_NE(next.flow, 250.0) << "200 of realized has no row to live on -- this is E2-F59";
}

TEST(BacktestRealizedFlow, FlowsStillSumWhenBarsAreSkipped) {
    // The same invariant, with two unwritten bars in the middle.
    double last = 0.0;
    double summed = 0.0;

    // D0 written, D1 and D2 skipped (no close), D3 written, D4 close-out written.
    struct Bar { double qty; double cumulative; bool written; };
    const Bar bars[] = {
        {10.0, 100.0, true},    // +100
        {10.0, 175.0, false},   // no close -- no row
        {10.0, 220.0, false},   // no close -- no row
        {10.0, 300.0, true},    // carries 175+45+80 == 200 since the last written row
        {0.0,  360.0, true},    // close-out carries the final 60
    };
    for (const auto& b : bars) {
        if (!b.written) continue;  // the coordinator must not touch the ledger here
        auto r = BacktestPnLManager::realized_row_for_bar(b.qty, b.cumulative, last);
        if (r.keep) summed += r.flow;
    }
    EXPECT_DOUBLE_EQ(summed, 360.0) << "Σ over written rows must equal the fills' total";
}

TEST(BacktestRealizedFlow, PeekDoesNotAdvanceTheLedger) {
    // The API shape that keeps a bar that writes nothing from advancing the ledger: a conditional
    // write site must peek, and commit only once it has written. Peeking twice must give the same
    // answer.
    double last = 0.0;
    auto a = BacktestPnLManager::realized_row_peek(10.0, 250.0, last);
    auto b = BacktestPnLManager::realized_row_peek(10.0, 250.0, last);
    EXPECT_DOUBLE_EQ(a.flow, 250.0);
    EXPECT_DOUBLE_EQ(b.flow, 250.0) << "peek must be pure";
    EXPECT_DOUBLE_EQ(last, 0.0) << "peek must not advance the ledger";

    BacktestPnLManager::commit_realized_row(250.0, last);
    EXPECT_DOUBLE_EQ(last, 250.0);
    EXPECT_DOUBLE_EQ(BacktestPnLManager::realized_row_peek(10.0, 250.0, last).flow, 0.0);
}
