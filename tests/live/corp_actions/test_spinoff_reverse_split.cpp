// tests/live/corp_actions/test_spinoff_reverse_split.cpp
//
// E2-F48 / BA-21 -- a reverse split on a spinoff's ex-date is a reverse split.
//
// `apply_spinoffs` guarded `F > 0` where its own comment said `> 1`. A spinoff's factor is
// the step the price series takes because value LEFT the parent, so it is strictly greater
// than 1 and `1 - 1/F` -- the fraction of the cost basis that leaves with the child -- is
// strictly positive. Three real bars carry a split_factor BELOW 1 on a spinoff ex-date:
//
//   HLT  2017-01-04  0.3333333333  (1-for-3, alongside PK and HGV)
//   LDOS 2013-09-30  0.25          (1-for-4, alongside SAIC)
//   DD   2019-06-03  0.33333333    (1-for-3, alongside CTVA)
//
// Passed in as F those made `1 - 1/F` negative -- HLT's is -2.0 -- so the child was
// allocated a negative cost basis, and under the default liquidate_at_first_close policy the
// child's whole first close PLUS that negative basis was booked as realized gain on the
// parent's row. It also SUPPRESSED a real split that applied correctly before the spinoff
// path existed, so the fix was a regression against pre-B-4 behaviour on those bars.
//
// THE RULE, chosen from the two the brief offered and applied here: (a) the reverse split is
// applied as an ordinary class-1 split, and only the rest of the bar's step is routed as the
// distribution. Option (b) -- refuse the whole bar -- survives as the DEGENERATE case, where
// taking the reverse split out leaves no distribution factor at all (DD): there is then
// nothing to allocate to the child, and delivering it would fabricate the allocation.

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <unordered_map>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/corporate_actions_lifecycle.hpp"

using namespace trade_ngin;

namespace {

SpinoffBarColumns bar(double split, double div, double close) {
    SpinoffBarColumns c;
    if (split != 1.0) {
        c.has_split = true;
        c.split_factor = split;
    }
    if (div != 0.0) {
        c.has_dividend = true;
        c.dividend_cash = div;
    }
    c.close_at_ex_date = close;
    return c;
}

Position held(const std::string& symbol, double qty, double avg) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(avg);
    p.realized_pnl = Decimal(0.0);
    return p;
}

}  // namespace

TEST(SpinoffReverseSplit, TheReverseSplitIsSeparatedOutAndTheTwoFactorsRebuildTheBarsStep) {
    const auto hlt = bar(0.3333333333, 24.48, 58.0);
    ASSERT_TRUE(hlt.has_reverse_split());
    EXPECT_DOUBLE_EQ(hlt.reverse_split_factor(), 0.3333333333);

    // What is left is the distribution, and it is above 1 as a distribution must be.
    EXPECT_NEAR(hlt.spinoff_factor(), 1.0 + 24.48 / 58.0, 1e-9);
    EXPECT_TRUE(hlt.routes_a_spinoff());

    // The two multiply back to the step the price series took: nothing is created or lost by
    // splitting the bar in two.
    EXPECT_NEAR(hlt.reverse_split_factor() * hlt.spinoff_factor(), hlt.total_factor(), 1e-12);

    const auto ldos = bar(0.25, 21.3548786447, 45.52);
    ASSERT_TRUE(ldos.has_reverse_split());
    EXPECT_DOUBLE_EQ(ldos.reverse_split_factor(), 0.25);
    EXPECT_NEAR(ldos.spinoff_factor(), 1.0 + 21.3548786447 / 45.52, 1e-9);
    EXPECT_TRUE(ldos.routes_a_spinoff());
    EXPECT_NEAR(ldos.reverse_split_factor() * ldos.spinoff_factor(), ldos.total_factor(),
                1e-12);
}

TEST(SpinoffReverseSplit, ASplitFactorAboveOneIsNotTreatedAsAShareCountChange) {
    // The five bars where the vendor encodes part of the distribution in split_factor. The
    // holder's share count did not change on any of them, and the adjusted series takes the
    // whole product -- so the entire step is the distribution's.
    for (const auto& c : {bar(1.0688328345, 2.9154459753, 42.32),   // ABT / HSP
                          bar(1.019, 0.5849224898, 31.43),          // BX  / PJT
                          bar(1.065, 3.67, 52.50),                  // K   / KLG
                          bar(1.122, 5.87, 48.53),                  // MET / BHF
                          bar(1.589, 13.28, 49.93),                 // RTX / OTIS + CARR
                          bar(1.327, 0.0, 71.60)}) {                // FTV / RAL, split-encoded
        EXPECT_FALSE(c.has_reverse_split());
        EXPECT_DOUBLE_EQ(c.reverse_split_factor(), 1.0);
        EXPECT_DOUBLE_EQ(c.spinoff_factor(), c.total_factor());
        EXPECT_TRUE(c.routes_a_spinoff());
    }
}

TEST(SpinoffReverseSplit, DDIsRefusedBecauseNoDistributionWasPricedIntoTheSeries) {
    // DD 2019-06-03: split_factor 0.33333333, div_cash 0. The adjusted series takes exactly
    // 0.3332989 -- the reverse split and nothing else -- so the vendor priced NO value out of
    // DD for Corteva. Taking the split out leaves a factor of 1: nothing to allocate.
    const auto dd = bar(0.33333333, 0.0, 76.10);
    ASSERT_TRUE(dd.has_reverse_split());
    EXPECT_DOUBLE_EQ(dd.reverse_split_factor(), 0.33333333);
    EXPECT_NEAR(dd.spinoff_factor(), 1.0, 1e-12);
    EXPECT_FALSE(dd.routes_a_spinoff())
        << "with no distribution factor left, delivering CTVA gives it a zero cost basis and "
           "books its entire first close as realized gain";
    // And the split itself is unchanged: exactly what the class-1 path applied before B-4.
    EXPECT_DOUBLE_EQ(dd.total_factor(), 0.33333333);
}

TEST(SpinoffReverseSplit, ApplySpinoffsRefusesAFactorAtOrBelowOneRatherThanGoingNegative) {
    // The backstop inside apply_spinoffs, exercised directly with the value the old routing
    // would have handed it for DD: F = 0.33333333, r = 0.33333 (CTVA).
    std::unordered_map<std::string, Position> positions;
    positions["DD"] = held("DD", 100.0, 30.52);

    SpinoffEvent ev;
    ev.parent = "DD";
    ev.child = "CTVA";
    ev.ex_date = "2019-06-03";
    ev.parent_restatement_factor = 0.33333333;  // the reverse split, mis-read as the spinoff
    ev.child_ratio = 0.33333;
    ev.child_first_close = 26.0;

    auto log = CorporateActionsLifecycle::apply_spinoffs(
        positions, {ev}, SpinoffChildPolicy::LIQUIDATE_AT_FIRST_CLOSE);

    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::SKIPPED_NO_CHILD_PRICE);

    // NOTHING was applied: the parent is untouched, no child exists, no realized was booked.
    EXPECT_DOUBLE_EQ(positions["DD"].quantity.as_double(), 100.0);
    EXPECT_DOUBLE_EQ(positions["DD"].average_price.as_double(), 30.52);
    EXPECT_DOUBLE_EQ(positions["DD"].realized_pnl.as_double(), 0.0);
    EXPECT_EQ(positions.count("CTVA"), 0u);

    // What the old `F > 0` guard would have produced instead: 1 - 1/0.33333333 = -2.0, a
    // child basis of 30.52 * -2 / 0.33333 = -183.12 per share, and 33 shares sold at 26.00
    // against it -- 33 * (26.00 + 183.12) = +6,901 of realized gain out of thin air.
    const double negative_child_basis = 30.52 * (1.0 - 1.0 / 0.33333333) / 0.33333;
    EXPECT_LT(negative_child_basis, 0.0);
    EXPECT_GT(33.0 * (26.0 - negative_child_basis), 6000.0);
}

TEST(SpinoffReverseSplit, ADistributionFactorJustAboveOneIsStillApplied) {
    // The guard is `> 1`, not `>= 1` and not "comfortably above 1": a tiny distribution is
    // still a distribution and must not be silently dropped.
    std::unordered_map<std::string, Position> positions;
    positions["PAR"] = held("PAR", 100.0, 100.0);

    SpinoffEvent ev;
    ev.parent = "PAR";
    ev.child = "CHI";
    ev.ex_date = "2020-01-02";
    ev.parent_restatement_factor = 1.0001;
    ev.child_ratio = 0.01;
    ev.child_first_close = 1.0;

    auto log = CorporateActionsLifecycle::apply_spinoffs(positions, {ev},
                                                        SpinoffChildPolicy::HOLD);
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::SPUN_OFF_CHILD_HELD);
    EXPECT_GT(log[0].child_avg_price, 0.0);
    EXPECT_NEAR(positions["PAR"].average_price.as_double(), 100.0 / 1.0001, 1e-7);
}

TEST(SpinoffReverseSplit, ExactlyOneIsRefusedBecauseNothingLeftTheParent) {
    std::unordered_map<std::string, Position> positions;
    positions["PAR"] = held("PAR", 100.0, 100.0);

    SpinoffEvent ev;
    ev.parent = "PAR";
    ev.child = "CHI";
    ev.ex_date = "2020-01-02";
    ev.parent_restatement_factor = 1.0;
    ev.child_ratio = 0.5;
    ev.child_first_close = 10.0;

    auto log = CorporateActionsLifecycle::apply_spinoffs(
        positions, {ev}, SpinoffChildPolicy::LIQUIDATE_AT_FIRST_CLOSE);
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::SKIPPED_NO_CHILD_PRICE);
    EXPECT_DOUBLE_EQ(positions["PAR"].average_price.as_double(), 100.0);
    EXPECT_DOUBLE_EQ(positions["PAR"].realized_pnl.as_double(), 0.0)
        << "F == 1 allocates a ZERO basis to the child, so liquidating it would book its "
           "whole first close as gain";
}

TEST(SpinoffReverseSplit, TheDecomposedHLTBarConservesBasisAcrossBothLegs) {
    // The full HLT arithmetic, single-child form (the multi-child case is E2-F49's test).
    // 100 sh at the raw 2017-01-03 close of 27.39.
    const auto columns = bar(0.3333333333, 24.48, 58.0);
    const double F_spin = columns.spinoff_factor();
    const double F_split = columns.reverse_split_factor();
    const double q0 = 100.0, B0 = 27.39, r = 0.6;  // PK, taken alone here

    std::unordered_map<std::string, Position> positions;
    positions["HLT"] = held("HLT", q0, B0);

    SpinoffEvent ev;
    ev.parent = "HLT";
    ev.child = "PK";
    ev.ex_date = "2017-01-04";
    ev.parent_restatement_factor = F_spin;
    ev.child_ratio = r;
    ev.child_first_close = 27.0;

    auto log = CorporateActionsLifecycle::apply_spinoffs(positions, {ev},
                                                        SpinoffChildPolicy::HOLD);
    ASSERT_EQ(log.size(), 1u);
    ASSERT_EQ(log[0].outcome, LifecycleOutcome::SPUN_OFF_CHILD_HELD);

    // Distribution first, on the PRE-split share count: 0.6 x 100 = 60 PK shares.
    EXPECT_DOUBLE_EQ(positions["HLT"].quantity.as_double(), 100.0);
    EXPECT_DOUBLE_EQ(positions["PK"].quantity.as_double(), 60.0);
    EXPECT_NEAR(positions["HLT"].average_price.as_double(), B0 / F_spin, 1e-7);
    EXPECT_NEAR(positions["PK"].average_price.as_double(), B0 * (1.0 - 1.0 / F_spin) / r,
                1e-7);

    // Basis conserved across the distribution.
    EXPECT_NEAR(100.0 * (B0 / F_spin) + 60.0 * (B0 * (1.0 - 1.0 / F_spin) / r), q0 * B0, 1e-5);

    // Then the class-1 reverse split, which the runner routes to the applier: quantity and
    // basis move together, so the parent's total cost is untouched by it.
    const double q_after_split = 100.0 * F_split;
    const double B_after_split = (B0 / F_spin) / F_split;
    EXPECT_NEAR(q_after_split * B_after_split, 100.0 * (B0 / F_spin), 1e-9);

    // And the parent's basis is now in the frame its marks are in: the whole bar's step.
    EXPECT_NEAR(B_after_split, B0 / columns.total_factor(), 1e-9);
    // 27.39 / 0.474023 = 57.78, against the raw ex-date close of 58.00.
    EXPECT_NEAR(B_after_split, 57.78, 0.05);
}
