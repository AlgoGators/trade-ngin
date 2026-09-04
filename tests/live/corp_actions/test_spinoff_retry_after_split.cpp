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
//      it delivers 33.33333333 * 0.6 = 20 PK and 11 HGV -- a third of the entitlement,
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

    // 33.33333333 * 0.6 = 19.999999998. Under a bare floor that was NINETEEN plus a
    // 0.999999998 "fraction" -- the defect one share worse still, from floating-point dust
    // rather than from anything real. BA-25's epsilon removes that artefact from the
    // measurement, so the unscaled retry now delivers a clean 20: exactly a third of the
    // sixty the holder is owed, which is the defect this test is about.
    EXPECT_DOUBLE_EQ(log[0].children[0].quantity, 20.0) << "PK: 33.33333333 * 0.6";
    EXPECT_NEAR(log[0].children[0].fractional, 0.0, 1e-6);
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

// BA-25 -- the retry must survive the ROUND TRIP THROUGH THE DATABASE, not only through
// memory.
//
// The test above compares the two paths with quantities the process computed. Production does
// not: `trading.positions.quantity` is numeric(20,6), so the retry reloads 33.333333, not the
// 33.33333333 the same-run path holds. 33.333333 x 1.8 = 59.9999994, and a bare floor turns
// that into 59 whole shares plus a 0.9999994 "fraction" -- the holder is one PK short and the
// book emits a cash-in-lieu SELL for very nearly a whole share that no broker ever paid.
// "Exactly the same book" was true in memory and false in the database.
TEST(SpinoffRetryAfterSplit, ASixDecimalRoundTripStillDeliversSixtyAndNoCashInLieu) {
    const auto frame = hlt_bar().retry_frame(/*coincident_split_already_applied=*/true);

    // The quantity as trading.positions stores and returns it: six decimal places.
    const double qty_post = static_cast<double>(Decimal(kQtyPre * kSplit));
    const double stored = std::round(qty_post * 1e6) / 1e6;
    ASSERT_NEAR(stored, 33.333333, 1e-9) << "the fixture must be the STORED quantity";
    ASSERT_LT(stored * (kRatioPK * frame.child_ratio_scale), 60.0)
        << "the exact product must sit just BELOW 60 or this test does not exercise the "
           "defect; it is " << stored * (kRatioPK * frame.child_ratio_scale);

    std::unordered_map<std::string, Position> positions;
    positions["HLT"] = held("HLT", stored, kBasisPre / kSplit);

    auto log = CorporateActionsLifecycle::apply_spinoffs(
        positions, {hlt_event(frame.child_ratio_scale)}, SpinoffChildPolicy::HOLD);
    ASSERT_EQ(log.size(), 1u);
    ASSERT_EQ(log[0].children.size(), 2u);

    EXPECT_DOUBLE_EQ(log[0].children[0].quantity, 60.0)
        << "PK came back " << log[0].children[0].quantity
        << ": the six-decimal storage precision cost the holder a share";
    EXPECT_NEAR(log[0].children[0].fractional, 0.0, 1e-9)
        << "a fraction of " << log[0].children[0].fractional
        << " would be sold as cash in lieu of very nearly a whole share";
    EXPECT_DOUBLE_EQ(log[0].children[0].cash_in_lieu, 0.0);
    EXPECT_DOUBLE_EQ(log[0].children[0].realized_delta, 0.0)
        << "no CIL means no realized on the PK leg under HOLD";

    // The delivered position, not just the log line.
    ASSERT_EQ(positions.count("PK"), 1u);
    EXPECT_DOUBLE_EQ(positions["PK"].quantity.as_double(), 60.0);
}

// The other half of the same rule: a GENUINE fraction must still be floored and still be paid
// out as cash in lieu. 1e-6 of a share is four orders of magnitude below what the column can
// represent, so the epsilon cannot swallow a real entitlement.
TEST(SpinoffRetryAfterSplit, AnExactOneThirdIsStillFlooredAndStillPaysCashInLieu) {
    const auto frame = hlt_bar().retry_frame(/*coincident_split_already_applied=*/true);
    const double stored = std::round((kQtyPre * kSplit) * 1e6) / 1e6;  // 33.333333

    std::unordered_map<std::string, Position> positions;
    positions["HLT"] = held("HLT", stored, kBasisPre / kSplit);

    auto log = CorporateActionsLifecycle::apply_spinoffs(
        positions, {hlt_event(frame.child_ratio_scale)}, SpinoffChildPolicy::HOLD);
    ASSERT_EQ(log.size(), 1u);
    ASSERT_EQ(log[0].children.size(), 2u);

    // HGV: 33.333333 x (0.33333/0.3333333333) = 33.3329997..., a third of a share out.
    const auto& hgv = log[0].children[1];
    EXPECT_EQ(hgv.symbol, "HGV");
    EXPECT_DOUBLE_EQ(hgv.quantity, 33.0) << "a real fraction must still floor";
    EXPECT_GT(hgv.fractional, 0.3);
    EXPECT_LT(hgv.fractional, 0.4);
    EXPECT_NEAR(hgv.cash_in_lieu, hgv.fractional * kCloseHGV, 1e-9);
    EXPECT_GT(hgv.cash_in_lieu, 0.0) << "the fractional share must still be paid out";

    // And the two children came out of ONE call, so the epsilon is per child, not per event.
    EXPECT_DOUBLE_EQ(log[0].children[0].quantity, 60.0);
}

// The epsilon must not round a fraction that is merely small-ish. 0.5 shares is not dust.
TEST(SpinoffRetryAfterSplit, AHalfShareEntitlementIsFlooredNotRounded) {
    std::unordered_map<std::string, Position> positions;
    positions["PAR"] = held("PAR", 7.0, 100.0);

    SpinoffEvent ev;
    ev.parent = "PAR";
    ev.ex_date = "2020-01-02";
    ev.parent_restatement_factor = 1.5;
    ev.children.push_back({"CHI", 0.5, 40.0});  // 7 * 0.5 = 3.5 exactly

    auto log = CorporateActionsLifecycle::apply_spinoffs(positions, {ev},
                                                        SpinoffChildPolicy::HOLD);
    ASSERT_EQ(log.size(), 1u);
    ASSERT_EQ(log[0].children.size(), 1u);
    EXPECT_DOUBLE_EQ(log[0].children[0].quantity, 3.0)
        << "std::round would give 4 -- half a share is an entitlement, not dust";
    EXPECT_NEAR(log[0].children[0].fractional, 0.5, 1e-12);
    EXPECT_NEAR(log[0].children[0].cash_in_lieu, 0.5 * 40.0, 1e-9);
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
