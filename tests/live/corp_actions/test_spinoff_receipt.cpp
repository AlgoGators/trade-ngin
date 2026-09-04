// tests/live/corp_actions/test_spinoff_receipt.cpp
//
// E2-F31 -- a spinoff must deliver the CHILD, not mangle the parent.
//
// Today the parent is silently wrong in three different ways depending on how the vendor
// encoded the distribution:
//
//   split_factor   FTV 2025-06-30 = 1.327   -> the applier reads a SPLIT and 100 FTV becomes
//                                              132.7 FTV. 32.7 shares that do not exist.
//   div_cash       MMM 2024-04-01 = 17.3875 -> the applier reads a DIVIDEND and books
//                                              $1,738.75 of income that never arrived, and
//                                              cuts the basis by the same ratio.
//   nothing        LEN 2025-02-07           -> the bar carries split_factor 1 and div_cash 0,
//                                              so nothing at all happens (V4-1 / E2-F41).
//
// In every one of the three the CHILD -- the entire point of the event -- is never created.
//
// The ratio is NOT inferred from the magnitude of the step (E2-F9's provenance rule). It is
// the `value` on the `spinoff` row in equities_data.corporate_action, verified 5/5 against
// the real distributions: GE/GEV 0.25, WDC/SNDK 0.33333, FTV/RAL 0.33333, LEN/MRP 0.5,
// MMM/SOLV 0.25.

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"
#include "trade_ngin/live/corporate_actions_lifecycle.hpp"

using namespace trade_ngin;

namespace {

Position held(const std::string& symbol, double qty, double avg, double realized = 0.0) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(avg);
    p.realized_pnl = Decimal(realized);
    return p;
}

// E2-F49: children are a VECTOR now. This helper keeps the single-child shape the E4 audit
// specified, so every number below is still the one B-4 hand-checked -- the relative-FMV
// allocation reduces to B(1-1/F)/r exactly when there is one child.
SpinoffEvent ftv_ral(double ratio = 0.33333, double first_close = 53.0) {
    SpinoffEvent ev;
    ev.parent = "FTV";
    ev.ex_date = "2025-06-30";
    ev.parent_restatement_factor = 1.327;  // the real split_factor on the FTV ex-date bar
    ev.children.push_back({"RAL", ratio, first_close});
    return ev;
}

}  // namespace

TEST(CorpActionSpinoff, RevivedSpinoffTermsCreateTheChildPositionWithNoCodeChange) {
    // The shape the E4 audit specified: 100 FTV @ 70, F = 1.327, r = 0.33333, child's first
    // close 53. Hand-computed, and every number below is checked against the arithmetic
    // rather than against whatever the implementation happens to produce.
    const double B = 70.0, q = 100.0, F = 1.327, r = 0.33333, c_child = 53.0;
    const double parent_basis = B / F;                    // 52.750565...
    const double child_basis = B * (1.0 - 1.0 / F) / r;   // 51.748822...
    const double child_exact = q * r;                     // 33.333
    const double child_whole = std::floor(child_exact);   // 33
    const double fraction = child_exact - child_whole;    // 0.333
    const double cil_realized = fraction * (c_child - child_basis);

    EXPECT_NEAR(parent_basis, 52.750565, 1e-6);
    EXPECT_NEAR(child_basis, 51.748822, 1e-6);

    // Tolerance note: anything read back out of a Position has been through `Decimal`, which
    // quantizes to 8 decimal places (52.75056518 for 52.750565184627). 1e-7 is that
    // quantum, not a fudge -- the LOG values below, which never touch Decimal, are checked
    // to 1e-9.
    constexpr double kDecimalQuantum = 1e-7;

    // HOLD -- the child is kept, which is what the audit's example describes.
    {
        std::unordered_map<std::string, Position> positions;
        positions["FTV"] = held("FTV", q, B);

        auto log = CorporateActionsLifecycle::apply_spinoffs(
            positions, {ftv_ral()}, SpinoffChildPolicy::HOLD);

        ASSERT_EQ(log.size(), 1u);
        EXPECT_EQ(log[0].outcome, LifecycleOutcome::SPUN_OFF_CHILD_HELD);
        EXPECT_EQ(log[0].symbol, "FTV");
        ASSERT_EQ(log[0].children.size(), 1u);
        EXPECT_EQ(log[0].children[0].symbol, "RAL");

        // The parent keeps every share. This is the whole defect: today it would hold 132.7.
        EXPECT_DOUBLE_EQ(positions["FTV"].quantity.as_double(), 100.0);
        EXPECT_NEAR(positions["FTV"].average_price.as_double(), parent_basis, kDecimalQuantum);

        ASSERT_EQ(positions.count("RAL"), 1u);
        EXPECT_DOUBLE_EQ(positions["RAL"].quantity.as_double(), 33.0);
        EXPECT_NEAR(positions["RAL"].average_price.as_double(), child_basis, kDecimalQuantum);

        // Cash in lieu of the 0.333 fractional share, struck at the child's first close and
        // realized against the CHILD's basis -- those shares were never parent shares.
        EXPECT_NEAR(log[0].children[0].fractional, 0.333, 1e-9);
        EXPECT_NEAR(log[0].children[0].cash_in_lieu, 0.333 * 53.0, 1e-9);
        EXPECT_NEAR(log[0].realized_delta, cil_realized, 1e-9);
        EXPECT_NEAR(positions["FTV"].realized_pnl.as_double(), cil_realized, kDecimalQuantum);

        // TOTAL COST BASIS IS CONSERVED. A distribution moves value between two holdings; it
        // does not create or destroy any. 100*52.750565 + 33.333*51.748822 == 100*70 exactly.
        const double book_basis = positions["FTV"].quantity.as_double() *
                                      positions["FTV"].average_price.as_double() +
                                  child_exact * child_basis;
        EXPECT_NEAR(book_basis, q * B, 1e-5)
            << "the spinoff created or destroyed cost basis";
    }

    // LIQUIDATE_AT_FIRST_CLOSE -- the DEFAULT, because RAL is not in the strategy universe
    // and a held child with no bars is an F-4 orphan: "Missing T-1 price for symbol with a
    // non-zero position", rolled back forever.
    {
        std::unordered_map<std::string, Position> positions;
        positions["FTV"] = held("FTV", q, B);

        auto log = CorporateActionsLifecycle::apply_spinoffs(positions, {ftv_ral()});

        ASSERT_EQ(log.size(), 1u);
        EXPECT_EQ(log[0].outcome, LifecycleOutcome::SPUN_OFF_CHILD_SOLD);
        EXPECT_EQ(positions.count("RAL"), 0u) << "nothing unpriceable may be left in the book";
        EXPECT_DOUBLE_EQ(positions["FTV"].quantity.as_double(), 100.0);
        EXPECT_NEAR(positions["FTV"].average_price.as_double(), parent_basis, kDecimalQuantum);

        const double liquidation = child_whole * (c_child - child_basis);
        EXPECT_NEAR(log[0].realized_delta, cil_realized + liquidation, 1e-9);
        EXPECT_NEAR(positions["FTV"].realized_pnl.as_double(), cil_realized + liquidation,
                    kDecimalQuantum);
        // 33 shares that opened 1.25 above their allocated basis. The distribution itself is
        // NOT a P&L event -- a child that opened exactly at its allocation books zero.
        EXPECT_NEAR(liquidation, 41.288876, 1e-6);
    }

    // A child that opens exactly at its allocated basis books nothing. This is the statement
    // that the allocation is a FAIR VALUE split and not a disguised gain.
    {
        std::unordered_map<std::string, Position> positions;
        positions["FTV"] = held("FTV", q, B);
        auto ev = ftv_ral(0.5, B * (1.0 - 1.0 / F) / 0.5);  // 50 whole shares, no fraction

        auto log = CorporateActionsLifecycle::apply_spinoffs(positions, {ev});
        ASSERT_EQ(log.size(), 1u);
        EXPECT_NEAR(log[0].realized_delta, 0.0, 1e-9);
        EXPECT_NEAR(positions["FTV"].realized_pnl.as_double(), 0.0, kDecimalQuantum);
    }

    // The dividend-encoded shape, which is the OTHER half of the defect. MMM 2024-04-01:
    // div_cash 17.3875 against a raw ex-date close of 94.02, spinning off SOLV at 0.25.
    // F is the same restatement the price series applies, 1 + d/c -- not a made-up number.
    {
        const double B_mmm = 106.07, q_mmm = 100.0;
        const double F_mmm = 1.0 + 17.3875 / 94.02;
        std::unordered_map<std::string, Position> positions;
        positions["MMM"] = held("MMM", q_mmm, B_mmm);

        SpinoffEvent ev;
        ev.parent = "MMM";
        ev.ex_date = "2024-04-01";
        ev.parent_restatement_factor = F_mmm;
        ev.children.push_back({"SOLV", 0.25, 69.10});  // SOLV's real close on the ex-date

        auto log = CorporateActionsLifecycle::apply_spinoffs(positions, {ev});
        ASSERT_EQ(log.size(), 1u);
        EXPECT_EQ(log[0].outcome, LifecycleOutcome::SPUN_OFF_CHILD_SOLD);
        EXPECT_DOUBLE_EQ(positions["MMM"].quantity.as_double(), 100.0);
        EXPECT_NEAR(positions["MMM"].average_price.as_double(), 89.515530, 1e-6);
        ASSERT_EQ(log[0].children.size(), 1u);
        EXPECT_NEAR(log[0].children[0].avg_price, 66.217880, 1e-6);
        EXPECT_DOUBLE_EQ(log[0].children[0].quantity, 25.0);
        EXPECT_DOUBLE_EQ(log[0].children[0].fractional, 0.0);   // 100 * 0.25 is exact
        EXPECT_DOUBLE_EQ(log[0].children[0].cash_in_lieu, 0.0);
        EXPECT_NEAR(log[0].realized_delta, 72.052992, 1e-6);
    }
}

TEST(CorpActionSpinoff, AChildWithNoCloseIsRefusedRatherThanGuessed) {
    // RAL and MRP -- both real 2025 spinoff children -- have ZERO rows in
    // equities_data.ohlcv_1d. With no close there is no cash-in-lieu price, no liquidation
    // price and no mark, and manufacturing one from the parent's basis would fabricate the
    // single number a broker statement is compared against. Refuse the whole event.
    std::unordered_map<std::string, Position> positions;
    positions["FTV"] = held("FTV", 100.0, 70.0, /*realized=*/12.5);

    auto ev = ftv_ral(0.33333, 0.0);

    auto log = CorporateActionsLifecycle::apply_spinoffs(positions, {ev});

    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::SKIPPED_NO_CHILD_PRICE);
    // NOTHING moved: not the quantity, not the basis, not the realized figure.
    EXPECT_DOUBLE_EQ(positions["FTV"].quantity.as_double(), 100.0);
    EXPECT_DOUBLE_EQ(positions["FTV"].average_price.as_double(), 70.0);
    EXPECT_DOUBLE_EQ(positions["FTV"].realized_pnl.as_double(), 12.5);
    EXPECT_EQ(positions.count("RAL"), 0u);

    // Same refusal for unusable terms, which is what a missing `spinoff` row looks like.
    for (auto bad : {0.0, -1.0}) {
        std::unordered_map<std::string, Position> p2;
        p2["FTV"] = held("FTV", 100.0, 70.0);
        auto e2 = ftv_ral(bad, 53.0);
        auto l2 = CorporateActionsLifecycle::apply_spinoffs(p2, {e2});
        ASSERT_EQ(l2.size(), 1u);
        EXPECT_EQ(l2[0].outcome, LifecycleOutcome::SKIPPED_NO_CHILD_PRICE);
        EXPECT_DOUBLE_EQ(p2["FTV"].average_price.as_double(), 70.0);
    }

    // An unheld parent is a no-op with no log entry, like every other lifecycle handler.
    std::unordered_map<std::string, Position> other;
    other["AAPL"] = held("AAPL", 10.0, 200.0);
    auto none = CorporateActionsLifecycle::apply_spinoffs(other, {ftv_ral()});
    EXPECT_TRUE(none.empty());
    EXPECT_EQ(other.size(), 1u);
}

TEST(CorpActionSpinoff, TheApplierRefusesASpinoffAndTheTypeRoundTrips) {
    // SPINOFF exists in CorpActionType only so the parent and the child can be recorded in
    // trading.corp_action_applied and deduped. The applier must never touch one: `value` on
    // a spinoff is the CHILD RATIO, so restating a parent by 0.25 as though it were a split
    // factor would cut 100 shares to 25.
    std::unordered_map<std::string, Position> positions;
    positions["MMM"] = held("MMM", 100.0, 106.07);

    CorpActionEvent ev;
    ev.symbol = "MMM";
    ev.ex_date = "2024-04-01";
    ev.type = CorpActionType::SPINOFF;
    ev.value = 0.25;
    ev.basis_provenance = CorpActionEvent::BasisProvenance::FORMED_ON_OR_BEFORE_EX_DATE;

    auto adjustments = CorporateActionsApplier::apply(positions, {ev});
    EXPECT_TRUE(adjustments.empty());
    EXPECT_DOUBLE_EQ(positions["MMM"].quantity.as_double(), 100.0);
    EXPECT_DOUBLE_EQ(positions["MMM"].average_price.as_double(), 106.07);

    // The dedup key persists the type as a STRING; if it does not round-trip, the recorded
    // spinoff is not found next run and the event applies a second time.
    const char* s = CorporateActionsApplier::type_to_string(CorpActionType::SPINOFF);
    EXPECT_STREQ(s, "SPINOFF");
    EXPECT_EQ(CorporateActionsApplier::type_from_type_string(s), CorpActionType::SPINOFF);

    // Adding the value must not disturb anything already written to the table.
    for (auto t : {CorpActionType::SPLIT, CorpActionType::ADR_SPLIT, CorpActionType::DIVIDEND,
                   CorpActionType::TERMINATION, CorpActionType::UNKNOWN}) {
        EXPECT_EQ(CorporateActionsApplier::type_from_type_string(
                      CorporateActionsApplier::type_to_string(t)),
                  t);
    }

    // And "spinoff" must still NOT parse from the VENDOR label into an applier action --
    // that is the routing decision the runner makes, with the terms row in hand.
    EXPECT_EQ(CorporateActionsApplier::type_from_action_string("spinoff"),
              CorpActionType::UNKNOWN);
    EXPECT_EQ(CorporateActionsApplier::type_from_action_string("spinoffdividend"),
              CorpActionType::UNKNOWN);

    // The policy string parses, and unknown text takes the SAFE default rather than HOLD.
    EXPECT_EQ(spinoff_child_policy_from_string("hold"), SpinoffChildPolicy::HOLD);
    EXPECT_EQ(spinoff_child_policy_from_string("liquidate_at_first_close"),
              SpinoffChildPolicy::LIQUIDATE_AT_FIRST_CLOSE);
    EXPECT_EQ(spinoff_child_policy_from_string("typo"),
              SpinoffChildPolicy::LIQUIDATE_AT_FIRST_CLOSE);
    EXPECT_EQ(spinoff_child_policy_from_string(""),
              SpinoffChildPolicy::LIQUIDATE_AT_FIRST_CLOSE);
    EXPECT_STREQ(spinoff_child_policy_to_string(SpinoffChildPolicy::LIQUIDATE_AT_FIRST_CLOSE),
                 "liquidate_at_first_close");

    // THE DEFECT, stated so it cannot be lost: fed to the applier as the SPLIT the vendor
    // encoded it as, FTV's 1.327 mints 32.7 shares that do not exist.
    std::unordered_map<std::string, Position> mangled;
    mangled["FTV"] = held("FTV", 100.0, 70.0);
    CorpActionEvent as_split;
    as_split.symbol = "FTV";
    as_split.ex_date = "2025-06-30";
    as_split.type = CorpActionType::SPLIT;
    as_split.value = 1.327;
    as_split.basis_provenance = CorpActionEvent::BasisProvenance::FORMED_ON_OR_BEFORE_EX_DATE;
    auto bad = CorporateActionsApplier::apply(mangled, {as_split});
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_NEAR(mangled["FTV"].quantity.as_double(), 132.7, 1e-9)
        << "this is E2-F31: the runner must route the spinoff away from the applier";
}
