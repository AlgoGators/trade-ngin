// tests/live/corp_actions/test_spinoff_multi_child.cpp
//
// E2-F49 / BA-22 -- a spinoff can distribute more than one company, and every one of them
// has to arrive.
//
// The routing map was `spinoff_terms[{ticker, ex_date}] = {child, ratio}` -- a scalar value
// on a key that is not unique. Fifteen (parent, ex-date) pairs in
// `equities_data.corporate_action` carry more than one `spinoff` row, so the last row read
// silently overwrote every earlier one and exactly one arbitrary child was delivered:
//
//   RTX  2020-04-03   OTIS 0.5   AND  CARR 1.0
//   HLT  2017-01-04   PK   0.6   AND  HGV  0.33333
//
// The parent's basis was still divided by the FULL factor, so the value of the missing
// children left the parent and went nowhere: the book lost cost basis outright.
//
// ALLOCATION. With one child, `B(1-1/F)/r` is forced -- the whole pool goes to the only
// recipient. With several, the pool has to be split, and the split is by relative fair market
// value at each child's first close, which is both the US holder's own basis-allocation rule
// and the rule the vendor's own factor already encodes (B-4 verified MMM's div_cash 17.3875
// = 0.25 x SOLV's 69.55). The single-child case must come out UNCHANGED, and the first test
// below is the statement of that.

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
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

// RTX 2020-04-03: split_factor 1.589 AND div_cash 13.28 on a close of 49.93 -- the E2-F47
// both-columns shape -- distributing OTIS at 0.5 and CARR at 1.0. First closes on or after
// the ex-date: OTIS 47.32, CARR 16.92.
SpinoffEvent rtx_otis_carr() {
    SpinoffEvent ev;
    ev.parent = "RTX";
    ev.ex_date = "2020-04-03";
    ev.parent_restatement_factor = 1.589 * (1.0 + 13.28 / 49.93);
    ev.children.push_back({"OTIS", 0.5, 47.32});
    ev.children.push_back({"CARR", 1.0, 16.92});
    return ev;
}

}  // namespace

TEST(SpinoffMultiChild, OneChildStillGetsTheWholePoolAndTheOldFormulaExactly) {
    // The FMV allocation must reduce to B(1-1/F)/r when there is one recipient, or every
    // number B-4 hand-checked would have moved. Checked against the formula itself, not
    // against a recorded output.
    const double B = 70.0, q = 100.0, F = 1.327, r = 0.33333, P = 53.0;
    std::unordered_map<std::string, Position> positions;
    positions["FTV"] = held("FTV", q, B);

    SpinoffEvent ev;
    ev.parent = "FTV";
    ev.ex_date = "2025-06-30";
    ev.parent_restatement_factor = F;
    ev.children.push_back({"RAL", r, P});

    auto log = CorporateActionsLifecycle::apply_spinoffs(positions, {ev},
                                                        SpinoffChildPolicy::HOLD);
    ASSERT_EQ(log.size(), 1u);
    ASSERT_EQ(log[0].children.size(), 1u);
    EXPECT_NEAR(log[0].children[0].avg_price, B * (1.0 - 1.0 / F) / r, 1e-9);
    EXPECT_NEAR(log[0].children[0].avg_price, 51.748822, 1e-6);  // B-4's number, unmoved
    EXPECT_NEAR(log[0].avg_price_after, 52.750565, 1e-6);
}

TEST(SpinoffMultiChild, EveryChildIsDeliveredOnceWithItsOwnRatioAndFirstClose) {
    std::unordered_map<std::string, Position> positions;
    positions["RTX"] = held("RTX", 100.0, 86.01);  // the real 2020-04-02 close

    auto log = CorporateActionsLifecycle::apply_spinoffs(positions, {rtx_otis_carr()},
                                                        SpinoffChildPolicy::HOLD);
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::SPUN_OFF_CHILD_HELD);
    ASSERT_EQ(log[0].children.size(), 2u) << "the second child was dropped again (E2-F49)";
    EXPECT_EQ(log[0].children_joined(), "OTIS, CARR");

    // Both positions exist, each with its own share count from its own ratio.
    ASSERT_EQ(positions.count("OTIS"), 1u);
    ASSERT_EQ(positions.count("CARR"), 1u);
    EXPECT_DOUBLE_EQ(positions["OTIS"].quantity.as_double(), 50.0);
    EXPECT_DOUBLE_EQ(positions["CARR"].quantity.as_double(), 100.0);
    EXPECT_DOUBLE_EQ(positions["RTX"].quantity.as_double(), 100.0);

    // Hand-computed allocation. pool = 86.01 (1 - 1/F); W = 0.5*47.32 + 1.0*16.92 = 40.58;
    // basis_i = pool * P_i / W.
    const double F = 1.589 * (1.0 + 13.28 / 49.93);
    const double pool = 86.01 * (1.0 - 1.0 / F);
    const double W = 0.5 * 47.32 + 1.0 * 16.92;
    EXPECT_NEAR(positions["RTX"].average_price.as_double(), 86.01 / F, 1e-7);
    EXPECT_NEAR(positions["OTIS"].average_price.as_double(), pool * 47.32 / W, 1e-7);
    EXPECT_NEAR(positions["CARR"].average_price.as_double(), pool * 16.92 / W, 1e-7);
    EXPECT_NEAR(positions["RTX"].average_price.as_double(), 42.756370, 1e-6);
    EXPECT_NEAR(positions["OTIS"].average_price.as_double(), 50.437698, 1e-6);
    EXPECT_NEAR(positions["CARR"].average_price.as_double(), 18.034781, 1e-6);

    // The dearer child is allocated the larger per-share basis, which is what "relative fair
    // market value" means and the direction a swapped allocation would break.
    EXPECT_GT(positions["OTIS"].average_price.as_double(),
              positions["CARR"].average_price.as_double());
}

TEST(SpinoffMultiChild, CostBasisIsConservedOverTheParentAndEVERYChild) {
    std::unordered_map<std::string, Position> positions;
    positions["RTX"] = held("RTX", 100.0, 86.01);

    auto log = CorporateActionsLifecycle::apply_spinoffs(positions, {rtx_otis_carr()},
                                                        SpinoffChildPolicy::HOLD);
    ASSERT_EQ(log.size(), 1u);

    double book = positions["RTX"].quantity.as_double() *
                  positions["RTX"].average_price.as_double();
    for (const auto& c : log[0].children) {
        // whole shares PLUS the fraction paid out as cash in lieu -- the fraction was still
        // basis that left the parent.
        book += (c.quantity + c.fractional) * c.avg_price;
    }
    EXPECT_NEAR(book, 100.0 * 86.01, 1e-5)
        << "the distribution created or destroyed cost basis. Before E2-F49 the parent's "
           "basis was divided by the full factor while only ONE child received an "
           "allocation, so the second child's share of the pool vanished from the book";

    // Stated the other way: the children's allocations sum to exactly the pool.
    const double F = 1.589 * (1.0 + 13.28 / 49.93);
    double allocated = 0.0;
    for (const auto& c : log[0].children) allocated += c.ratio * c.avg_price;
    EXPECT_NEAR(allocated, 86.01 * (1.0 - 1.0 / F), 1e-9);
}

TEST(SpinoffMultiChild, LiquidationRealizesEachChildAgainstItsOwnAllocatedBasis) {
    std::unordered_map<std::string, Position> positions;
    positions["RTX"] = held("RTX", 100.0, 86.01);

    auto log = CorporateActionsLifecycle::apply_spinoffs(
        positions, {rtx_otis_carr()}, SpinoffChildPolicy::LIQUIDATE_AT_FIRST_CLOSE);
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::SPUN_OFF_CHILD_SOLD);
    EXPECT_EQ(positions.count("OTIS"), 0u);
    EXPECT_EQ(positions.count("CARR"), 0u);

    const double F = 1.589 * (1.0 + 13.28 / 49.93);
    const double pool = 86.01 * (1.0 - 1.0 / F);
    const double W = 0.5 * 47.32 + 1.0 * 16.92;
    const double expected = 50.0 * (47.32 - pool * 47.32 / W) +
                            100.0 * (16.92 - pool * 16.92 / W);

    EXPECT_NEAR(log[0].realized_delta, expected, 1e-6);
    EXPECT_NEAR(log[0].realized_delta, -267.363005, 1e-5);
    EXPECT_NEAR(positions["RTX"].realized_pnl.as_double(), expected, 1e-7);

    // Per child, and they sum to the total booked on the parent's row.
    ASSERT_EQ(log[0].children.size(), 2u);
    EXPECT_NEAR(log[0].children[0].realized_delta, 50.0 * (47.32 - pool * 47.32 / W), 1e-6);
    EXPECT_NEAR(log[0].children[1].realized_delta, 100.0 * (16.92 - pool * 16.92 / W), 1e-6);
    EXPECT_NEAR(log[0].children[0].realized_delta + log[0].children[1].realized_delta,
                log[0].realized_delta, 1e-9);
}

TEST(SpinoffMultiChild, ChildrenThatOpenExactlyAtTheirAllocationBookNothing) {
    // The statement that the allocation is a fair-value split and not a disguised gain,
    // extended to several children. Choose closes so that each child's first close IS its
    // allocated basis: with basis_i = pool * P_i / W, that happens when W == pool, i.e. when
    // the children's total distributed value per parent share equals the pool.
    const double B = 100.0, F = 2.0;             // pool = 50 per parent share
    const double pool = B * (1.0 - 1.0 / F);
    // Two children, ratios 0.5 and 2.0. Pick P so that 0.5*P1 + 2.0*P2 == pool with
    // P1 = 60, P2 = 10: 30 + 20 = 50. == pool.
    std::unordered_map<std::string, Position> positions;
    positions["PAR"] = held("PAR", 100.0, B);

    SpinoffEvent ev;
    ev.parent = "PAR";
    ev.ex_date = "2020-01-02";
    ev.parent_restatement_factor = F;
    ev.children.push_back({"C1", 0.5, 60.0});
    ev.children.push_back({"C2", 2.0, 10.0});
    ASSERT_NEAR(0.5 * 60.0 + 2.0 * 10.0, pool, 1e-12);

    auto log = CorporateActionsLifecycle::apply_spinoffs(
        positions, {ev}, SpinoffChildPolicy::LIQUIDATE_AT_FIRST_CLOSE);
    ASSERT_EQ(log.size(), 1u);
    ASSERT_EQ(log[0].children.size(), 2u);
    EXPECT_NEAR(log[0].children[0].avg_price, 60.0, 1e-9);
    EXPECT_NEAR(log[0].children[1].avg_price, 10.0, 1e-9);
    EXPECT_NEAR(log[0].realized_delta, 0.0, 1e-9);
    EXPECT_NEAR(positions["PAR"].realized_pnl.as_double(), 0.0, 1e-7);
}

TEST(SpinoffMultiChild, OneUnpriceableChildRefusesTheWHOLEEvent) {
    // HLT 2017-01-04 is this case on the real database: PK and HGV both have ZERO rows in
    // equities_data.ohlcv_1d. Allocating over the children that DO have a close would hand
    // the priced one the whole pool -- a basis several times its real allocation -- and drop
    // the other entirely. An allocation computed over a subset of the recipients is not an
    // allocation, so the event is refused whole and said so loudly.
    std::unordered_map<std::string, Position> positions;
    positions["HLT"] = held("HLT", 100.0, 27.39, /*realized=*/5.0);

    SpinoffEvent ev;
    ev.parent = "HLT";
    ev.ex_date = "2017-01-04";
    ev.parent_restatement_factor = 1.0 + 24.48 / 58.0;
    ev.children.push_back({"PK", 0.6, 0.0});       // no bars
    ev.children.push_back({"HGV", 0.33333, 27.0});  // priced, but its sibling is not

    auto log = CorporateActionsLifecycle::apply_spinoffs(
        positions, {ev}, SpinoffChildPolicy::LIQUIDATE_AT_FIRST_CLOSE);
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::SKIPPED_NO_CHILD_PRICE);
    EXPECT_TRUE(log[0].children.empty());

    // NOTHING moved: not the quantity, not the basis, not the realized figure, and neither
    // child exists -- including the one that could have been priced.
    EXPECT_DOUBLE_EQ(positions["HLT"].quantity.as_double(), 100.0);
    EXPECT_DOUBLE_EQ(positions["HLT"].average_price.as_double(), 27.39);
    EXPECT_DOUBLE_EQ(positions["HLT"].realized_pnl.as_double(), 5.0);
    EXPECT_EQ(positions.count("PK"), 0u);
    EXPECT_EQ(positions.count("HGV"), 0u);
}

TEST(SpinoffMultiChild, FractionalSharesAreCashedOutPerChild) {
    // 7 shares held. OTIS at 0.5 gives 3.5 -> 3 whole plus 0.5 cashed out; CARR at 1.0 gives
    // 7 exactly. The cash-in-lieu is per child and struck at that child's own first close.
    std::unordered_map<std::string, Position> positions;
    positions["RTX"] = held("RTX", 7.0, 86.01);

    auto log = CorporateActionsLifecycle::apply_spinoffs(positions, {rtx_otis_carr()},
                                                        SpinoffChildPolicy::HOLD);
    ASSERT_EQ(log.size(), 1u);
    ASSERT_EQ(log[0].children.size(), 2u);

    EXPECT_DOUBLE_EQ(log[0].children[0].quantity, 3.0);
    EXPECT_NEAR(log[0].children[0].fractional, 0.5, 1e-12);
    EXPECT_NEAR(log[0].children[0].cash_in_lieu, 0.5 * 47.32, 1e-9);
    EXPECT_DOUBLE_EQ(log[0].children[1].quantity, 7.0);
    EXPECT_DOUBLE_EQ(log[0].children[1].fractional, 0.0);
    EXPECT_DOUBLE_EQ(log[0].children[1].cash_in_lieu, 0.0);

    // Under HOLD only the fraction realizes, and only for the child that had one.
    EXPECT_NEAR(log[0].realized_delta, log[0].children[0].realized_delta, 1e-12);
    EXPECT_DOUBLE_EQ(log[0].children[1].realized_delta, 0.0);

    // Conservation still holds over the fractional shares.
    double book = positions["RTX"].quantity.as_double() *
                  positions["RTX"].average_price.as_double();
    for (const auto& c : log[0].children) book += (c.quantity + c.fractional) * c.avg_price;
    EXPECT_NEAR(book, 7.0 * 86.01, 1e-6);
}
