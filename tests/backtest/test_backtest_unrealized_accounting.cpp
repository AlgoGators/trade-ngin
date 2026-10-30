// Backtest unrealized P&L is gated on the settlement model (PnLAccountingMethod) and carries
// point_value.
//
// On a futures backtest row `average_price` IS the prior close: TrendFollowingStrategy resets it
// to current_price after settling and get_target_positions() stamps price_history.back(), while
// the coordinator feeds on_data the T-1 bars. So an ungated
//
//     updated_pos.unrealized_pnl = Decimal((current_close - avg_price) * qty);
//
// reduces algebraically to the realized formula and records the same settled move twice; without
// point_value it also divides it by the contract multiplier (unrealized/realized is then EXACTLY
// 1/point_value on every symbol: MYM 2.0 (pv 0.5), MNQ 0.5 (pv 2), MES 0.2 (pv 5), ZN 0.001
// (pv 1000), 6A 0.00001 (pv 100000)).
//
// Live and backtest agree: LivePnLManager::UnrealizedPolicy applies the same rule on the live
// side, where a futures row's unrealized is 0.

#include <gtest/gtest.h>

#include "trade_ngin/backtest/backtest_pnl_manager.hpp"
#include "trade_ngin/strategy/types.hpp"

using namespace trade_ngin;
using trade_ngin::backtest::BacktestPnLManager;

namespace {

// Real numbers from the MYM.v.0 row on 2026-08-06 in run
// TREND_FOLLOWING_20260901_060030_828, so the fixture cannot drift from what production
// actually produced.
constexpr double kQty        = 3.0;
constexpr double kPrevClose  = 54583.0;   // close 2026-08-05 -- what average_price holds
constexpr double kClose      = 53998.0;   // close 2026-08-06
constexpr double kPointValue = 0.5;       // MYM contract size

// (53998 - 54583) * 3 * 0.5 == -877.50, the value stored in realized_pnl.
constexpr double kSettledMove = (kClose - kPrevClose) * kQty * kPointValue;

// What the ungated expression gives: the same move, with point_value dropped. -1755.00.
constexpr double kBuggyValue = (kClose - kPrevClose) * kQty;

}  // namespace

// ---------------------------------------------------------------------------
// Futures invariant. This is the one that must never move.
// ---------------------------------------------------------------------------

TEST(BacktestUnrealizedAccounting, RealizedOnlyYieldsZeroUnrealized) {
    const double u = BacktestPnLManager::unrealized_for_accounting(
        PnLAccountingMethod::REALIZED_ONLY, kQty, kPrevClose, kClose, kPointValue);

    EXPECT_DOUBLE_EQ(u, 0.0)
        << "A REALIZED_ONLY (futures) position recorded a non-zero unrealized P&L. Under "
           "daily settlement average_price IS the prior settlement close, so any value here "
           "is the settled move already booked in realized_pnl -- the same day counted "
           "twice (E2-F2).";
}

// The ungated expression's value, pinned so it cannot come back unnoticed.
TEST(BacktestUnrealizedAccounting, RealizedOnlyDoesNotReproduceTheUngatedExpression) {
    const double u = BacktestPnLManager::unrealized_for_accounting(
        PnLAccountingMethod::REALIZED_ONLY, kQty, kPrevClose, kClose, kPointValue);

    EXPECT_NE(u, kBuggyValue)
        << "The ungated pre-fix expression has returned. This is the literal value the "
           "defect wrote to MYM.v.0 on 2026-08-06 (-1755.00): the settled move (-877.50) "
           "recorded a second time and divided by the 0.5 contract multiplier.";
}

// average_price must be ignored entirely under REALIZED_ONLY -- not merely happen to be
// close to the mark. A futures row whose basis has drifted must still report 0.
TEST(BacktestUnrealizedAccounting, RealizedOnlyIgnoresAveragePriceEvenWhenItDiffersWidely) {
    const double u = BacktestPnLManager::unrealized_for_accounting(
        PnLAccountingMethod::REALIZED_ONLY, kQty, /*average_price=*/1.0, kClose, kPointValue);

    EXPECT_DOUBLE_EQ(u, 0.0)
        << "REALIZED_ONLY consulted average_price. The futures result must be 0 by the "
           "settlement identity regardless of what the basis holds.";
}

// ---------------------------------------------------------------------------
// Equity behaviour: a genuine mark against a cost basis, dollarised.
// ---------------------------------------------------------------------------

TEST(BacktestUnrealizedAccounting, MixedMeasuresAgainstTheCostBasis) {
    // 10 shares bought at 100.00, marked at 105.50, equities point_value 1.0.
    const double u = BacktestPnLManager::unrealized_for_accounting(
        PnLAccountingMethod::MIXED, 10.0, 100.0, 105.50, 1.0);

    EXPECT_DOUBLE_EQ(u, 55.0)
        << "MIXED (equity) accounting did not measure the position against its cost basis.";
}

TEST(BacktestUnrealizedAccounting, UnrealizedOnlyBehavesLikeMixed) {
    const double u = BacktestPnLManager::unrealized_for_accounting(
        PnLAccountingMethod::UNREALIZED_ONLY, 10.0, 100.0, 105.50, 1.0);

    EXPECT_DOUBLE_EQ(u, 55.0)
        << "UNREALIZED_ONLY must mark against the cost basis exactly as MIXED does; only "
           "REALIZED_ONLY is the settled case.";
}

// point_value is part of the expression. This fails if anyone drops the multiplier.
TEST(BacktestUnrealizedAccounting, PointValueIsAppliedNotAssumedToBeOne) {
    const double u = BacktestPnLManager::unrealized_for_accounting(
        PnLAccountingMethod::MIXED, kQty, kPrevClose, kClose, kPointValue);

    EXPECT_DOUBLE_EQ(u, kSettledMove)
        << "point_value was not applied. With the multiplier dropped this returns "
        << kBuggyValue << " instead of " << kSettledMove << " -- exactly the 1/point_value "
           "ratio the defect produced across every futures symbol.";
}

TEST(BacktestUnrealizedAccounting, ShortPositionMarksWithTheCorrectSign) {
    // Short 10 at 100.00, mark falls to 95.00 -> +50 unrealized.
    const double u = BacktestPnLManager::unrealized_for_accounting(
        PnLAccountingMethod::MIXED, -10.0, 100.0, 95.0, 1.0);

    EXPECT_DOUBLE_EQ(u, 50.0) << "A short position did not gain when the mark fell.";
}

// ---------------------------------------------------------------------------
// "No basis known" cases. 0.0 means "cannot measure", not "no gain".
// ---------------------------------------------------------------------------

TEST(BacktestUnrealizedAccounting, NoCostBasisYieldsZeroRatherThanMarkAsBasis) {
    EXPECT_DOUBLE_EQ(
        BacktestPnLManager::unrealized_for_accounting(PnLAccountingMethod::MIXED, 10.0, 0.0,
                                                      105.50, 1.0),
        0.0)
        << "An unset average_price must yield 0, never a mark measured against zero. "
           "Substituting a mark for a basis is the root defect mapped in "
           "docs/AVERAGE_PRICE_LIFECYCLE.md.";

    EXPECT_DOUBLE_EQ(
        BacktestPnLManager::unrealized_for_accounting(PnLAccountingMethod::MIXED, 10.0, -1.0,
                                                      105.50, 1.0),
        0.0)
        << "A negative average_price must be treated as 'no basis known'.";
}

TEST(BacktestUnrealizedAccounting, ZeroQuantityYieldsZero) {
    EXPECT_DOUBLE_EQ(
        BacktestPnLManager::unrealized_for_accounting(PnLAccountingMethod::MIXED, 0.0, 100.0,
                                                      105.50, 1.0),
        0.0)
        << "A flat position must carry no unrealized P&L.";
}
