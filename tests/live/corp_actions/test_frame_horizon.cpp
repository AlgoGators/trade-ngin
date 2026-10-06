// E2-F15 regression pins — a corp-action restatement must not put the position and the price
// series in different unit frames.
//
// THE FAILURE, from a real run (2026-09-01 10:09, R4 window, log r4d/2026-04-06.log):
//
//   Applied SPLIT for BKNG (ex_date 2026-04-06): qty 10 -> 250, avg_price 4194.31 -> 167.7724
//   Generated execution: BKNG SELL 248.799399 at 4194.31        <-- PRE-split price
//   Day T position for BKNG: qty=1.200601 realized_pnl=1001584.464342
//
// $1,001,800 gross realized P&L on a $100,000 book. `4194.31 / 25 = 167.7724` exactly: the
// price was 25x too large because the position moved into post-split units and the price
// series did not.
//
// WHY IT HAPPENED. Two windows, each individually correct:
//   * the corp-action fetch reads split_factor from the ex-date bar ROW, so it spans day D --
//     legitimate, split ratios are announced weeks ahead and using one is not lookahead;
//   * the bar load ends at the last completed session on a replay (`end_date = now - 24h`),
//     and build_equity_adjusted_query back-adjusts a bar by the steps of every LATER bar, so
//     with the ex-date bar outside the window there is no step and the closes stay raw.
// Nothing rescaled the prices to match the restated basis.
//
// THE FIX is a horizon gate, not a rescale: only apply an event the price series already
// covers. Rescaling the price maps instead would corrupt finalize_previous_day, which marks
// `previous_positions_pre_action` and MUST stay pre-event -- that would trade a $1M error for
// a $40k one.
//
// WHY THESE TESTS EXIST AT ALL. Every invariant the E2 fix wave added PASSES on the bad
// number at HEAD: L5 rows-sum-to-aggregate, `total_pnl = (realized - costs) + unrealized`,
// `equity = initial + total_pnl`, and the daily-flow identity. They are all INTERNAL
// consistency checks and the wrong number is consistently wrong everywhere. The first test
// below is the missing statement -- the only one that compares a restatement against
// something outside itself.

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <unordered_map>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"

using namespace trade_ngin;

namespace {

// Real numbers from the BKNG failure, so the fixture cannot drift from what production did.
constexpr double kQtyBefore = 10.0;
constexpr double kBasisBefore = 4194.31;     // 2026-04-02 close, pre-split
constexpr double kSplitRatio = 25.0;         // ex-date 2026-04-06
constexpr double kBasisAfter = 167.7724;     // 4194.31 / 25, exactly
constexpr const char* kExDate = "2026-04-06";
constexpr const char* kLastLoadedBar = "2026-04-02";   // the horizon on a replay for 04-06

Position held(double qty, double avg) {
    Position p;
    p.symbol = "BKNG";
    p.quantity = Quantity(qty);
    p.average_price = Decimal(avg);
    return p;
}

CorpActionEvent split_event(double ratio) {
    CorpActionEvent ev;
    ev.symbol = "BKNG";
    ev.ex_date = kExDate;
    ev.type = CorpActionType::SPLIT;
    ev.value = ratio;
    return ev;
}

// The runner's gate, in one line, so the test pins the RULE rather than the runner's plumbing:
// an event may be applied only if the symbol's loaded price series already reaches its ex-date.
bool horizon_allows(const std::string& ex_date, const std::string& last_bar_date) {
    return !last_bar_date.empty() && ex_date <= last_bar_date;
}

// E2-F16's companion rule, likewise stated as the RULE rather than the plumbing. Day T-1 is
// normally finalized from the PRE-corp-action snapshot (8a1a96ef), so a day is not restated
// before its own ex-date. That holds only while the ex-date is still in the future relative to
// T-1. Once the horizon gate defers an event, the event can arrive on a run whose T-1 is ALREADY
// on or after the ex-date -- and then the T-1 close is post-event while the pre-action basis is
// pre-event. Finalize from the restated book in exactly that case.
bool finalize_from_restated_book(const std::string& applied_ex_date, const std::string& t1_date) {
    return !applied_ex_date.empty() && applied_ex_date <= t1_date;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. THE MISSING INVARIANT. This is the check whose absence let $1M through.
// ---------------------------------------------------------------------------

TEST(CorpActionFrameHorizon, BasisToMarkRatioIsInvariantAcrossAClassOneApply) {
    std::unordered_map<std::string, Position> positions{{"BKNG", held(kQtyBefore, kBasisBefore)}};

    // The mark the runner would price against. On the failing run this stayed PRE-split
    // because the ex-date bar was outside the loaded window.
    const double mark_before = kBasisBefore;

    auto adjustments = CorporateActionsApplier::apply(positions, {split_event(kSplitRatio)});
    ASSERT_EQ(adjustments.size(), 1u);
    const auto& adj = adjustments.front();

    EXPECT_NEAR(adj.avg_price_after, kBasisAfter, 1e-9);
    EXPECT_NEAR(static_cast<double>(positions["BKNG"].quantity), 250.0, 1e-9);

    // If the price series is in the same frame as the basis, it has been rescaled by the SAME
    // factor, so basis/mark is unchanged. Applying the event while the mark is still pre-event
    // breaks exactly this.
    const double mark_after_if_unrescaled = mark_before;
    const double ratio_before = adj.avg_price_before / mark_before;
    const double ratio_after = adj.avg_price_after / mark_after_if_unrescaled;

    EXPECT_NEAR(ratio_after / ratio_before, 1.0 / kSplitRatio, 1e-9)
        << "Sanity: leaving the mark pre-event must break the invariant by exactly the split "
           "ratio. If this fails the fixture no longer reproduces the defect.";

    // And with the mark in the same frame, the invariant holds — which is what the horizon
    // gate guarantees by refusing to apply until the series covers the ex-date.
    const double mark_after_correct = mark_before / kSplitRatio;
    EXPECT_NEAR(adj.avg_price_after / mark_after_correct, ratio_before, 1e-9)
        << "Basis and mark are in different unit frames after the restatement. This is the "
           "condition that produced BKNG SELL 248.799399 at 4194.31 -- $1,001,800 of realized "
           "P&L on a $100,000 book.";
}

// Same invariant, the other direction. A reverse split inverts the sign of the error: the
// basis becomes LARGER than the mark, `pnl_pct = (price - basis)/basis` reads about -90%, and
// the mean-reversion stop-loss fires and liquidates the position on the ex-date.
// 69 reverse-split bars exist in equities_data.ohlcv_1d.
TEST(CorpActionFrameHorizon, InvariantAlsoHoldsForAReverseSplit) {
    const double ratio = 0.1;  // 1-for-10
    std::unordered_map<std::string, Position> positions{{"BKNG", held(100.0, 20.0)}};
    const double mark_before = 20.0;

    auto adjustments = CorporateActionsApplier::apply(positions, {[&] {
                                                          auto e = split_event(ratio);
                                                          return e;
                                                      }()});
    ASSERT_EQ(adjustments.size(), 1u);
    const auto& adj = adjustments.front();

    EXPECT_NEAR(adj.avg_price_after, 200.0, 1e-9) << "1-for-10 must multiply the basis by 10.";
    EXPECT_NEAR(static_cast<double>(positions["BKNG"].quantity), 10.0, 1e-9);

    // Un-rescaled mark: basis 200 vs mark 20 -> pnl_pct = (20-200)/200 = -90%, tripping a 5%
    // stop that should never have fired.
    const double pnl_pct_unrescaled = (mark_before - adj.avg_price_after) / adj.avg_price_after;
    EXPECT_LT(pnl_pct_unrescaled, -0.5)
        << "Sanity: the un-rescaled frame must look like a catastrophic loss. If it does not, "
           "this fixture no longer reproduces the reverse-split stop-loss inversion.";

    // Same frame: no P&L at all, which is correct — a reverse split moves no value.
    const double mark_after_correct = mark_before / ratio;
    EXPECT_NEAR((mark_after_correct - adj.avg_price_after) / adj.avg_price_after, 0.0, 1e-9);
}

// Dividends restate the basis by `1 + d/close` rather than a share ratio. Smaller (0.06-0.71%
// on the configured universe) but ~50,953 dividend bars exist against 771 split bars, so this
// is the flavour that actually fires — roughly every 11 trading days per held name. And unlike
// the mark, the realized error never self-heals: daily_realized_pnl is deliberately absent
// from the Day T-1 UPDATE, so a mispriced ex-date fill welds into every cumulative figure.
TEST(CorpActionFrameHorizon, InvariantAlsoHoldsForADividend) {
    const double close_at_ex = 89.27;   // ABT, 2026-07-15
    const double div = 0.63;
    const double ratio = 1.0 + div / close_at_ex;

    std::unordered_map<std::string, Position> positions{{"ABT", held(88.0, close_at_ex)}};
    CorpActionEvent ev;
    ev.symbol = "ABT";
    ev.ex_date = "2026-07-15";
    ev.type = CorpActionType::DIVIDEND;
    ev.value = div;
    ev.close_at_ex_date = close_at_ex;

    auto adjustments = CorporateActionsApplier::apply(positions, {ev});
    ASSERT_EQ(adjustments.size(), 1u);
    const auto& adj = adjustments.front();

    EXPECT_NEAR(adj.ratio_change, ratio, 1e-9);
    EXPECT_NEAR(adj.avg_price_after, close_at_ex / ratio, 1e-9);

    // Same shape as the split: with the mark left in the pre-event frame the reported gain is
    // about the dividend cash, booked as a phantom price move.
    const double phantom = 88.0 * (close_at_ex - adj.avg_price_after);
    EXPECT_NEAR(phantom, 88.0 * div / ratio, 1e-6)
        << "The un-rescaled frame should book roughly the dividend cash as a price gain.";
}

// ---------------------------------------------------------------------------
// 2. THE GATE. Defer while the series is behind; apply unchanged once it catches up.
// ---------------------------------------------------------------------------

TEST(CorpActionFrameHorizon, EventPastTheNewestLoadedBarIsDeferred) {
    EXPECT_FALSE(horizon_allows(kExDate, kLastLoadedBar))
        << "An ex-date beyond the newest loaded bar must be deferred. This is the BKNG case: "
           "the corp-action fetch spans 2026-04-06 but the bar load ends 2026-04-02.";

    // A held symbol with no bars at all in the window is the same refusal, and also surfaces
    // a symbol that is held but has been dropped from the configured universe.
    EXPECT_FALSE(horizon_allows(kExDate, ""));
}

TEST(CorpActionFrameHorizon, EventCoveredByTheSeriesAppliesUnchanged) {
    // True-live, or the next replay run: the ex-date bar is loaded, back-adjustment has run,
    // and the whole series is post-event. The gate must be a no-op here.
    EXPECT_TRUE(horizon_allows(kExDate, kExDate));
    EXPECT_TRUE(horizon_allows(kExDate, "2026-04-07"));

    std::unordered_map<std::string, Position> positions{{"BKNG", held(kQtyBefore, kBasisBefore)}};
    auto adjustments = CorporateActionsApplier::apply(positions, {split_event(kSplitRatio)});

    ASSERT_EQ(adjustments.size(), 1u)
        << "With the horizon satisfied the event must still apply exactly as before -- the gate "
           "must not change true-live behaviour.";
    EXPECT_NEAR(static_cast<double>(positions["BKNG"].quantity), 250.0, 1e-9);
    EXPECT_NEAR(static_cast<double>(positions["BKNG"].average_price), kBasisAfter, 1e-9);
}

// Deferral must be recoverable, not a silent drop. The runner writes no dedup row for a
// deferred event, so the next run reconsiders it — the same recovery the dividend-denominator
// skip already relies on.
TEST(CorpActionFrameHorizon, DeferredEventAppliesOnTheNextRunAndTheInvariantThenHolds) {
    std::unordered_map<std::string, Position> positions{{"BKNG", held(kQtyBefore, kBasisBefore)}};

    // Run 1: horizon behind -> nothing applied, position untouched.
    ASSERT_FALSE(horizon_allows(kExDate, kLastLoadedBar));
    EXPECT_NEAR(static_cast<double>(positions["BKNG"].quantity), kQtyBefore, 1e-9);
    EXPECT_NEAR(static_cast<double>(positions["BKNG"].average_price), kBasisBefore, 1e-9);

    // Run 2: the ex-date bar is now loaded and the series is adjusted.
    ASSERT_TRUE(horizon_allows(kExDate, "2026-04-07"));
    auto adjustments = CorporateActionsApplier::apply(positions, {split_event(kSplitRatio)});
    ASSERT_EQ(adjustments.size(), 1u);

    const double mark_now = 176.19;  // real 2026-04-06 close, post-split
    const double basis = static_cast<double>(positions["BKNG"].average_price);
    EXPECT_NEAR(basis, kBasisAfter, 1e-9);

    // Basis and mark are now in one frame, so the stop-loss input is sane instead of +2400%.
    const double pnl_pct = (mark_now - basis) / basis;
    EXPECT_GT(pnl_pct, 0.0);
    EXPECT_LT(pnl_pct, 0.25)
        << "Post-fix the mark and basis must be comparable. Pre-fix this read about +2400%, "
           "which is why the stop-loss could never fire on a forward split.";
}


// ---------------------------------------------------------------------------
// 4. E2-F16 -- the defect the horizon gate CREATED, and its fix.
//
// Deferral is correct, but it invents a state 8a1a96ef never had to consider: an event applying
// on run D for an ex-date of D-1. The pre-action snapshot is then a frame behind the T-1 close,
// and the finalizer marks a pre-split basis against a post-split price. Measured on a real run:
// the 2026-04-06 row took -4824.16 of phantom unrealized and the equity curve dropped to
// 95,080.29. It does NOT self-correct -- running 04-08 and 04-09 left the row untouched.
// ---------------------------------------------------------------------------

TEST(CorpActionFrameHorizon, DeferredEventCatchingUpFinalizesFromTheRestatedBook) {
    // Run on 04-07: the split for ex-date 04-06 was deferred yesterday and applies now, so T-1
    // (04-06) is already post-event.
    EXPECT_TRUE(finalize_from_restated_book(kExDate, "2026-04-06"))
        << "ex_date == T-1: the deferred event has caught up and the T-1 close is post-event.";
    EXPECT_TRUE(finalize_from_restated_book(kExDate, "2026-04-07"))
        << "ex_date behind T-1: a longer gap, same conclusion.";

    // The ordinary case must be untouched: an event whose ex-date is still ahead of T-1 is
    // exactly what 8a1a96ef's pre-action snapshot exists for.
    EXPECT_FALSE(finalize_from_restated_book(kExDate, "2026-04-02"))
        << "ex_date after T-1 must keep the PRE-action snapshot, or a day is restated before "
           "its own ex-date -- the regression 8a1a96ef fixed.";

    // No class-1 event applied at all: every symbol keeps the pre-action snapshot.
    EXPECT_FALSE(finalize_from_restated_book("", "2026-04-06"))
        << "With no applied event the predicate must be false for every symbol, so the futures "
           "path and every ordinary equity day are byte-for-byte unchanged.";
}

TEST(CorpActionFrameHorizon, RestatedBookAndPreActionBookDisagreeByExactlyTheSplitRatio) {
    // The real 2026-04-06 close, post-split, and the residual position after the 04-06 sell.
    constexpr double kT1ClosePostSplit = 176.19;
    constexpr double kQtyHeldPreSplit = 1.200601;
    const double qty_held_post_split = kQtyHeldPreSplit * kSplitRatio;   // 30.015025

    // WRONG -- pre-action book (pre-split basis) marked against the post-split T-1 close.
    const double wrong = kQtyHeldPreSplit * (kT1ClosePostSplit - kBasisBefore);
    EXPECT_NEAR(wrong, -4824.16, 0.01)
        << "This is the number the 2026-04-06 row actually carried before the fix.";

    // RIGHT -- restated book, both sides post-split.
    const double right = qty_held_post_split * (kT1ClosePostSplit - kBasisAfter);
    EXPECT_NEAR(right, 252.6545, 1e-3);

    // ...and it must equal the same economics computed entirely in PRE-split units, which is
    // the independent derivation: the engine never sees this expression.
    const double pre_split_units =
        kQtyHeldPreSplit * (kT1ClosePostSplit * kSplitRatio - kBasisBefore);
    EXPECT_NEAR(right, pre_split_units, 1e-6)
        << "Restating the book must be economically neutral. If these ever disagree, the "
           "applier's rescale convention and the finalizer have drifted apart again.";

    // The defect was not a rounding matter: it is the whole book, with the sign flipped.
    EXPECT_GT(std::abs(wrong - right), 5000.0);
    EXPECT_LT(wrong, 0.0);
    EXPECT_GT(right, 0.0);
}

TEST(CorpActionFrameHorizon, RestatedFinalizationIsNeutralForAReverseSplitAndADividend) {
    // Reverse split, real DD numbers: 1-for-3 on 2026-06-24, close 46.67 -> 137.82.
    {
        constexpr double kRatio = 1.0 / 3.0;      // split_factor 0.3333333333
        constexpr double kQtyBeforeRs = 30.0;
        constexpr double kBasisBeforeRs = 46.67;
        constexpr double kT1Close = 137.82;
        const double qty_after = kQtyBeforeRs * kRatio;
        const double basis_after = kBasisBeforeRs / kRatio;

        const double restated = qty_after * (kT1Close - basis_after);
        const double pre_event_units = kQtyBeforeRs * (kT1Close * kRatio - kBasisBeforeRs);
        EXPECT_NEAR(restated, pre_event_units, 1e-6);
    }

    // Dividend, real ABT numbers: 0.63 on 2026-04-15 against a 101.56 close.
    {
        constexpr double kDiv = 0.63;
        constexpr double kCloseAtEx = 101.56;
        const double ratio = 1.0 + kDiv / kCloseAtEx;
        constexpr double kQty = 57.412048;
        constexpr double kBasisBeforeDiv = 95.47;
        constexpr double kT1Close = 96.81;
        const double basis_after = kBasisBeforeDiv / ratio;

        // Quantity is unchanged by a dividend, so neutrality is a statement about the basis
        // and the mark moving together.
        const double restated = kQty * (kT1Close - basis_after);
        const double pre_event_units = kQty * (kT1Close * ratio - kBasisBeforeDiv) / ratio;
        EXPECT_NEAR(restated, pre_event_units, 1e-6);
    }
}
