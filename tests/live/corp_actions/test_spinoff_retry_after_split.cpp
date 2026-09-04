// tests/live/corp_actions/test_spinoff_retry_after_split.cpp
//
// E2-F50 -- the retry of a refused spinoff must not distribute on the post-split share count.
//
// THE SEQUENCE. A spinoff bar carrying a coincident reverse split (HLT 2017-01-04: split_factor
// 0.3333333333, div_cash 24.48, children PK 0.6 and HGV 0.33333) is decomposed by E2-F48: the
// reverse split goes to the class-1 applier, the rest is the distribution. When the children
// have no price series -- which is the case for PK and HGV in this database -- the distribution
// is REFUSED and writes no dedup row, so it retries on every later run. The reverse split is
// NOT refused with it: it applies, and it IS dedup'd.
//
//   run N    : distribution refused (no child closes); SPLIT applied, dedup row written.
//              the book is now 33.333333 HLT at basis 82.17, not 100 at 27.39.
//   later    : PK and HGV bars appear.
//   run N+k  : the distribution retries -- against the already-split book.
//
// TWO THINGS BREAK if the retry does not notice.
//   1. `corporate_action.spinoff.value` is child shares per PRE-split parent share (HLT's
//      0.6 PK is 200 M PK shares over 330 M pre-split HLT shares). Applied to 33.333333 shares
//      it delivers floor(33.33333333 * 0.6) = 19 PK and 11 HGV -- a third of the entitlement,
//      with no error anywhere.
//   2. `pending_class1_split_factor` would still be set from the bar, so the G1 basis-vs-mark
//      bound would divide an already-post-split basis by 0.3333 a second time: a 3x error
//      inside the guard whose job is to catch 3x errors.
//
// THE FIX is exact rather than approximate. q_post = q_pre * F_split and r_post = r / F_split,
// so q_post * r_post == q_pre * r; and B_post = B_pre / F_split makes the distributed pool and
// the relative-FMV weights scale by the same factor, which cancels. The retry therefore lands
// on exactly the book a same-run apply would have produced -- which is what the last test here
// asserts, side by side.

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <unordered_map>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/corporate_actions_lifecycle.hpp"

using namespace trade_ngin;

namespace {

// HLT 2017-01-04, the real bar.
constexpr double kSplit = 0.3333333333;
constexpr double kDiv = 24.48;
constexpr double kCloseEx = 58.00;
constexpr double kRatioPK = 0.6;
constexpr double kRatioHGV = 0.33333;
constexpr double kBasisPre = 27.39;   // the real 2017-01-03 close
constexpr double kQtyPre = 100.0;
// PK and HGV have no bars in this database, so the retry can only be exercised with closes
// supplied by hand. They are inputs to the arithmetic, not claims about the market.
constexpr double kClosePK = 27.00;
constexpr double kCloseHGV = 32.00;

SpinoffBarColumns hlt_bar() {
    SpinoffBarColumns c;
    c.has_split = true;
    c.split_factor = kSplit;
    c.has_dividend = true;
    c.dividend_cash = kDiv;
    c.close_at_ex_date = kCloseEx;
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

SpinoffEvent hlt_event(double ratio_scale) {
    SpinoffEvent ev;
    ev.parent = "HLT";
    ev.ex_date = "2017-01-04";
    ev.parent_restatement_factor = hlt_bar().spinoff_factor();
    ev.children.push_back({"PK", kRatioPK * ratio_scale, kClosePK});
    ev.children.push_back({"HGV", kRatioHGV * ratio_scale, kCloseHGV});
    return ev;
}

}  // namespace

TEST(SpinoffRetryAfterSplit, SameRunLeavesTheRatiosAloneAndTellsTheBoundASplitIsPending) {
    const auto f = hlt_bar().retry_frame(/*coincident_split_already_applied=*/false);
    EXPECT_FALSE(f.split_already_applied);
    EXPECT_DOUBLE_EQ(f.child_ratio_scale, 1.0)
        << "on the same-run path the book is still PRE-split, so the vendor's ratios apply "
           "as written";
    EXPECT_DOUBLE_EQ(f.pending_split_factor, kSplit)
        << "class 1 applies the reverse split after the routing block, and the G1 bound has "
           "to divide by it before comparing basis to mark (E2-F48)";
}

TEST(SpinoffRetryAfterSplit, AnAlreadyAppliedSplitScalesTheRatiosAndClearsThePendingFactor) {
    const auto f = hlt_bar().retry_frame(/*coincident_split_already_applied=*/true);
    EXPECT_TRUE(f.split_already_applied);
    EXPECT_NEAR(f.child_ratio_scale, 1.0 / kSplit, 1e-12);
    EXPECT_NEAR(f.child_ratio_scale, 3.0, 1e-9);
    EXPECT_DOUBLE_EQ(f.pending_split_factor, 1.0)
        << "nothing is pending -- the split is already in the basis, and telling the bound "
           "otherwise makes it divide by 0.3333 twice";
}

TEST(SpinoffRetryAfterSplit, ABarWithNoReverseSplitIsUnaffectedEitherWay) {
    // RTX 2020-04-03: split_factor 1.589 is part of the distribution, not a share-count
    // change, so it never reaches class 1 and can never have been "already applied".
    SpinoffBarColumns rtx;
    rtx.has_split = true;
    rtx.split_factor = 1.589;
    rtx.has_dividend = true;
    rtx.dividend_cash = 13.28;
    rtx.close_at_ex_date = 49.93;
    for (bool applied : {false, true}) {
        const auto f = rtx.retry_frame(applied);
        EXPECT_FALSE(f.split_already_applied);
        EXPECT_DOUBLE_EQ(f.child_ratio_scale, 1.0);
        EXPECT_DOUBLE_EQ(f.pending_split_factor, 1.0);
    }
}

TEST(SpinoffRetryAfterSplit, TheUnscaledRetryIsTheDefectAndDeliversAThirdOfTheEntitlement) {
    // What the code did before E2-F50: the vendor's ratios against the post-split book.
    const double qty_post = kQtyPre * kSplit;              // 33.333333
    const double basis_post = kBasisPre / kSplit;          // 82.17
    std::unordered_map<std::string, Position> positions;
    positions["HLT"] = held("HLT", qty_post, basis_post);

    auto log = CorporateActionsLifecycle::apply_spinoffs(
        positions, {hlt_event(/*ratio_scale=*/1.0)}, SpinoffChildPolicy::HOLD);
    ASSERT_EQ(log.size(), 1u);
    ASSERT_EQ(log[0].children.size(), 2u);

    // 33.33333333 * 0.6 = 19.999999998, so the floor is NINETEEN, not twenty -- the vendor's
    // 10-digit split factor makes the defect a share worse than the round arithmetic suggests,
    // and the missing share is paid out as cash in lieu of a 0.999999998 fraction.
    EXPECT_DOUBLE_EQ(log[0].children[0].quantity, 19.0) << "PK: floor(33.33333333 * 0.6)";
    EXPECT_NEAR(log[0].children[0].fractional, 0.999999998, 1e-8);
    EXPECT_DOUBLE_EQ(log[0].children[1].quantity, 11.0) << "HGV: floor(33.33333333 * 0.33333)";
    // A third of what the holder is owed, and nothing anywhere says so.
    EXPECT_NEAR(log[0].children[0].quantity + log[0].children[0].fractional,
                kQtyPre * kRatioPK / 3.0, 1e-6);
    EXPECT_NEAR(log[0].children[1].quantity + log[0].children[1].fractional,
                kQtyPre * kRatioHGV / 3.0, 1e-6);
}

TEST(SpinoffRetryAfterSplit, TheScaledRetryDeliversTheFullEntitlementOnHLTsNumbers) {
    const auto frame = hlt_bar().retry_frame(/*coincident_split_already_applied=*/true);
    const double qty_post = kQtyPre * kSplit;
    const double basis_post = kBasisPre / kSplit;
    std::unordered_map<std::string, Position> positions;
    positions["HLT"] = held("HLT", qty_post, basis_post);

    auto log = CorporateActionsLifecycle::apply_spinoffs(
        positions, {hlt_event(frame.child_ratio_scale)}, SpinoffChildPolicy::HOLD);
    ASSERT_EQ(log.size(), 1u);
    ASSERT_EQ(log[0].children.size(), 2u);

    // 60 PK and 33.33 HGV per 100 PRE-split shares -- the brief's acceptance.
    EXPECT_NEAR(log[0].children[0].quantity + log[0].children[0].fractional,
                kQtyPre * kRatioPK, 1e-9);
    EXPECT_DOUBLE_EQ(log[0].children[0].quantity, 60.0);
    EXPECT_NEAR(log[0].children[1].quantity + log[0].children[1].fractional,
                kQtyPre * kRatioHGV, 1e-9);
    EXPECT_DOUBLE_EQ(log[0].children[1].quantity, 33.0);
    EXPECT_NEAR(log[0].children[1].fractional, 0.333, 1e-9);

    // The parent keeps its post-split share count; only the basis moves, by the distribution
    // factor alone (the split is already in it).
    EXPECT_NEAR(positions["HLT"].quantity.as_double(), qty_post, 1e-12);
    EXPECT_NEAR(positions["HLT"].average_price.as_double(),
                basis_post / hlt_bar().spinoff_factor(), 1e-7);
    // Which is the same frame the price series is in: B_pre / total_factor.
    EXPECT_NEAR(positions["HLT"].average_price.as_double(),
                kBasisPre / hlt_bar().total_factor(), 1e-7);
}

TEST(SpinoffRetryAfterSplit, TheRetryLandsOnEXACTLYTheBookASameRunApplyWouldHave) {
    // The statement that makes the fix a fix rather than a patch: run the two paths side by
    // side and compare every number. Path A -- the distribution applies on the PRE-split book,
    // then class 1 splits. Path B -- class 1 split first (the refusal), then the scaled retry.
    std::unordered_map<std::string, Position> a;
    a["HLT"] = held("HLT", kQtyPre, kBasisPre);
    auto log_a = CorporateActionsLifecycle::apply_spinoffs(
        a, {hlt_event(/*ratio_scale=*/1.0)}, SpinoffChildPolicy::HOLD);
    // ... then the class-1 reverse split, which the applier does as qty *= F, basis /= F.
    const double a_parent_qty = a["HLT"].quantity.as_double() * kSplit;
    const double a_parent_basis = a["HLT"].average_price.as_double() / kSplit;

    std::unordered_map<std::string, Position> b;
    b["HLT"] = held("HLT", kQtyPre * kSplit, kBasisPre / kSplit);
    const auto frame = hlt_bar().retry_frame(true);
    auto log_b = CorporateActionsLifecycle::apply_spinoffs(
        b, {hlt_event(frame.child_ratio_scale)}, SpinoffChildPolicy::HOLD);

    EXPECT_NEAR(b["HLT"].quantity.as_double(), a_parent_qty, 1e-9);
    EXPECT_NEAR(b["HLT"].average_price.as_double(), a_parent_basis, 1e-6);

    ASSERT_EQ(log_a[0].children.size(), log_b[0].children.size());
    for (size_t i = 0; i < log_a[0].children.size(); ++i) {
        const auto& ca = log_a[0].children[i];
        const auto& cb = log_b[0].children[i];
        EXPECT_EQ(ca.symbol, cb.symbol);
        EXPECT_DOUBLE_EQ(ca.quantity, cb.quantity) << ca.symbol;
        EXPECT_NEAR(ca.fractional, cb.fractional, 1e-9) << ca.symbol;
        EXPECT_NEAR(ca.avg_price, cb.avg_price, 1e-9)
            << ca.symbol << ": the allocated basis must not depend on which order the split "
                            "and the distribution were applied in";
        EXPECT_NEAR(ca.cash_in_lieu, cb.cash_in_lieu, 1e-9) << ca.symbol;
    }
    EXPECT_NEAR(log_a[0].realized_delta, log_b[0].realized_delta, 1e-9);

    // And cost basis is conserved on the retry path, over the parent and both children.
    double book = b["HLT"].quantity.as_double() * b["HLT"].average_price.as_double();
    for (const auto& c : log_b[0].children) book += (c.quantity + c.fractional) * c.avg_price;
    EXPECT_NEAR(book, kQtyPre * kBasisPre, 1e-5);
}
