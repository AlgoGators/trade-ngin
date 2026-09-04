// B-iii regression pins -- a spinoff off a parent with NO KNOWN COST BASIS must be refused,
// not allocated from zero.
//
// THE DEFECT: apply_spinoffs guards the basis arithmetic with `basis > 0.0 ? ... : 0.0`
// (corporate_actions_lifecycle.cpp:150,154), so a parent whose average_price is 0 yields
// parent_basis_after = 0 and pool_per_share = 0, hence a child basis of 0 for every child.
// `allocation_sane` only rejects NEGATIVE or non-finite values, and zero is neither, so the
// event proceeds.
//
// Under the default liquidate_at_first_close policy the child is then sold at its first
// close against a basis of zero:
//
//     realized = quantity * (first_close - 0) == the child's ENTIRE market value
//
// booked onto the parent's day-T row, into live_results and into the equity curve in one
// step. Worse, the runner's receipt BUY is gated on `child_avg_price > 0.0`
// (live_equity_mean_reversion.cpp:2594), so the acquisition is suppressed while the
// disposal is emitted -- leaving a SELL in trading.executions for a security that was never
// bought.
//
// `average_price == 0` is not a hypothetical. It is the shape a position is PERSISTED in
// when its basis could not be resolved: AVERAGE_PRICE_LIFECYCLE rule 5 ("a closed row
// carries no basis") writes 0, and the rule-5 residual path writes basis 0 with a loud
// ERROR when `resolve_day_t_cost_basis` finds nothing ("BASIS TRACE | UNRESOLVED"). A book
// carrying such a row is exactly the book that must not have a corporate action applied to
// it -- the same reasoning as the E2-F17 basis-provenance skip, which refuses to restate a
// basis the run cannot vouch for.
//
// The refusal writes no dedup row, so the event is retried next run once the basis is
// repaired, rather than being silently consumed.

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>

#include "trade_ngin/live/corporate_actions_lifecycle.hpp"

using namespace trade_ngin;

namespace {

Position held(const std::string& symbol, double qty, double avg, double realized = 0.0) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(avg);
    p.realized_pnl = Decimal(realized);
    return p;
}

// MMM -> SOLV, the real 2024-04-01 event B-4 replayed.
SpinoffEvent mmm_solv(double first_close = 66.0) {
    SpinoffEvent ev;
    ev.parent = "MMM";
    ev.ex_date = "2024-04-01";
    ev.parent_restatement_factor = 1.043;  // the real split_factor on the MMM ex-date bar
    ev.children.push_back({"SOLV", 0.25, first_close});
    return ev;
}

}  // namespace

TEST(SpinoffZeroBasisParent, AParentWithNoKnownBasisIsRefused) {
    std::unordered_map<std::string, Position> positions;
    positions["MMM"] = held("MMM", 100.0, 0.0, /*realized=*/5.0);

    auto log = CorporateActionsLifecycle::apply_spinoffs(positions, {mmm_solv()});

    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::SKIPPED_NO_PARENT_BASIS);

    // Nothing moved on the parent.
    EXPECT_DOUBLE_EQ(positions["MMM"].quantity.as_double(), 100.0);
    EXPECT_DOUBLE_EQ(positions["MMM"].average_price.as_double(), 0.0);
    EXPECT_DOUBLE_EQ(positions["MMM"].realized_pnl.as_double(), 5.0);

    // No child was delivered, and above all no realized was fabricated.
    EXPECT_EQ(positions.count("SOLV"), 0u);
    EXPECT_TRUE(log[0].children.empty());
    EXPECT_DOUBLE_EQ(log[0].realized_delta, 0.0)
        << "a zero basis would have booked 25 x 66.00 = 1650.00 as pure realized gain";
}

TEST(SpinoffZeroBasisParent, TheRefusalIsIndependentOfTheChildPolicy) {
    for (auto policy : {SpinoffChildPolicy::HOLD, SpinoffChildPolicy::LIQUIDATE_AT_FIRST_CLOSE}) {
        std::unordered_map<std::string, Position> positions;
        positions["MMM"] = held("MMM", 100.0, 0.0);
        auto log = CorporateActionsLifecycle::apply_spinoffs(positions, {mmm_solv()}, policy);
        ASSERT_EQ(log.size(), 1u);
        EXPECT_EQ(log[0].outcome, LifecycleOutcome::SKIPPED_NO_PARENT_BASIS);
        EXPECT_EQ(positions.count("SOLV"), 0u);
    }
}

TEST(SpinoffZeroBasisParent, ANegativeBasisIsRefusedToo) {
    // Not reachable through the applier today, but a persisted negative would allocate a
    // negative pool and is refused for the same reason rather than by arithmetic accident.
    std::unordered_map<std::string, Position> positions;
    positions["MMM"] = held("MMM", 100.0, -1.0);
    auto log = CorporateActionsLifecycle::apply_spinoffs(positions, {mmm_solv()});
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::SKIPPED_NO_PARENT_BASIS);
}

TEST(SpinoffZeroBasisParent, AKnownBasisStillDeliversTheChild) {
    // The B-4 §6 replay numbers, so the refusal cannot have been bought by breaking the
    // path that works: MMM 100 sh at 106.07, SOLV 0.25 at first close 66.217880.
    std::unordered_map<std::string, Position> positions;
    positions["MMM"] = held("MMM", 100.0, 106.07);

    auto ev = mmm_solv(66.217880);
    auto log = CorporateActionsLifecycle::apply_spinoffs(positions, {ev},
                                                         SpinoffChildPolicy::HOLD);

    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::SPUN_OFF_CHILD_HELD);
    ASSERT_EQ(log[0].children.size(), 1u);
    EXPECT_DOUBLE_EQ(log[0].children[0].quantity, 25.0);
    EXPECT_GT(log[0].children[0].avg_price, 0.0);
    EXPECT_GT(positions["MMM"].average_price.as_double(), 0.0);
}
