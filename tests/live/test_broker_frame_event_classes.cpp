// B-vii regression pins -- expected_pnl_gap must fold only SHARE-COUNT events into the
// split correction.
//
// THE DEFECT: the reverse walk classifies every event as "dividend or split":
//
//     if (!is_dividend(ev)) { splits_after *= ev.ratio; continue; }
//
// `is_dividend` is `action_type == "DIVIDEND"`, so SPINOFF and TERMINATION rows -- which
// basis_chain() returns alongside dividends and splits -- are treated as share-count
// splits. A spinoff restates the cost basis but leaves the parent's share count ALONE
// (corporate_actions_lifecycle.cpp: "a spinoff never changes the PARENT share count"), so
// folding its ratio into `splits_after` divides every EARLIER dividend's per-share cash by
// a factor that never touched the share count.
//
// Real chain: MMM pays a dividend on 2024-02-16 and spins off SOLV on 2024-04-01. The
// February dividend was paid on exactly the share count held today -- no split intervened --
// but the walk charges it at d/1.10 and understates the gap by ~$13.73 on 100 shares.
//
// Second defect on the same line: the ratio-validity check
//
//     if (!ev.ratio_known || !(ev.ratio > 0.0) ...) return 0.0;
//
// runs BEFORE the event class is known, so a TERMINATION row -- which carries a NULL
// basis_ratio, because a termination does not restate a basis -- short-circuits the entire
// chain to 0.0. A closed-out position in the history therefore silences the reconciliation
// for the whole symbol.
//
// THE RULE: only SPLIT and ADR_SPLIT change the share count. Everything that is neither a
// dividend nor a share-count split is irrelevant to this arithmetic and is skipped without
// its ratio being validated.
//
// NOTE: expected_pnl_gap has no production caller today (C-5 §9-D) -- it is reachable only
// from tests and the F-8 reconciliation tooling. These pins are what will make it correct
// when it is wired.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "trade_ngin/live/broker_frame.hpp"

using namespace trade_ngin;
using namespace trade_ngin::broker_frame;

namespace {

AppliedEvent dividend(const std::string& ex_date, double dps, double ratio) {
    AppliedEvent e;
    e.symbol = "MMM";
    e.ex_date = ex_date;
    e.action_type = "DIVIDEND";
    e.dividend_per_share = dps;
    e.ratio = ratio;
    e.ratio_known = true;
    return e;
}

AppliedEvent other(const std::string& ex_date, const std::string& type, double ratio,
                   bool ratio_known = true) {
    AppliedEvent e;
    e.symbol = "MMM";
    e.ex_date = ex_date;
    e.action_type = type;
    e.ratio = ratio;
    e.ratio_known = ratio_known;
    return e;
}

constexpr double kQty = 100.0;
constexpr double kRawBasis = 106.07;

}  // namespace

TEST(BrokerFrameEventClasses, ASpinoffDoesNotDiscountAnEarlierDividend) {
    // The real MMM chain: dividend 2024-02-16 (d = 1.51, r = 1.00524), spinoff 2024-04-01
    // with a basis ratio of 1.10. No split ever occurred, so the February dividend was paid
    // at today's share count and must be charged at its full 1.51.
    const std::vector<AppliedEvent> with_spinoff = {
        dividend("2024-02-16", 1.51, 1.00524),
        other("2024-04-01", "SPINOFF", 1.10),
    };
    const std::vector<AppliedEvent> dividend_only = {dividend("2024-02-16", 1.51, 1.00524)};

    const double gap_with = expected_pnl_gap(kQty, kRawBasis, with_spinoff);
    const double gap_without = expected_pnl_gap(kQty, kRawBasis, dividend_only);

    EXPECT_NEAR(gap_with, gap_without, 1e-9)
        << "a spinoff leaves the parent's share count alone, so it cannot change how much "
           "cash an earlier dividend paid per share held today";

    // And concretely: charging at 1.51/1.10 instead of 1.51 understates the cash term by
    // 100 * (1.51 - 1.51/1.10) = 13.7272...
    EXPECT_NEAR(gap_with - (kQty * kRawBasis * (1.0 - 1.0 / 1.00524) - kQty * 1.51), 0.0, 1e-9);
}

TEST(BrokerFrameEventClasses, ARealSplitStillDiscountsAnEarlierDividend) {
    // The BA-23 behaviour must survive: a dividend paid BEFORE a 4:1 split was paid on a
    // quarter of today's shares.
    const std::vector<AppliedEvent> chain = {
        dividend("2024-02-16", 1.51, 1.00524),
        other("2024-03-01", "SPLIT", 4.0),
    };
    const double gap = expected_pnl_gap(kQty, kRawBasis, chain);
    const double expected =
        kQty * kRawBasis * (1.0 - 1.0 / 1.00524) - kQty * (1.51 / 4.0);
    EXPECT_NEAR(gap, expected, 1e-9);
}

TEST(BrokerFrameEventClasses, AnAdrSplitIsAShareCountChangeToo) {
    const std::vector<AppliedEvent> chain = {
        dividend("2024-02-16", 1.51, 1.00524),
        other("2024-03-01", "ADR_SPLIT", 2.0),
    };
    const double gap = expected_pnl_gap(kQty, kRawBasis, chain);
    const double expected =
        kQty * kRawBasis * (1.0 - 1.0 / 1.00524) - kQty * (1.51 / 2.0);
    EXPECT_NEAR(gap, expected, 1e-9);
}

TEST(BrokerFrameEventClasses, ATerminationWithNoRatioDoesNotSilenceTheChain) {
    // A termination does not restate a basis, so its basis_ratio is NULL. Validating that
    // ratio before classifying the row returned 0.0 for the whole symbol -- the
    // reconciliation went quiet exactly when a position had been closed out.
    const std::vector<AppliedEvent> chain = {
        dividend("2024-02-16", 1.51, 1.00524),
        other("2024-05-01", "TERMINATION", 0.0, /*ratio_known=*/false),
    };
    const double gap = expected_pnl_gap(kQty, kRawBasis, chain);
    const double expected = kQty * kRawBasis * (1.0 - 1.0 / 1.00524) - kQty * 1.51;

    EXPECT_NE(gap, 0.0) << "a termination row must not zero the whole chain";
    EXPECT_NEAR(gap, expected, 1e-9);
}

TEST(BrokerFrameEventClasses, AnUnknownDividendRatioStillSilencesTheChain) {
    // The BA-23 / migration-006 rule is unchanged for the rows that DO restate a basis: a
    // pre-006 dividend row with a NULL ratio makes the chain uninvertible and must say so.
    const std::vector<AppliedEvent> chain = {
        dividend("2024-02-16", 1.51, 1.00524),
        other("2024-03-01", "SPLIT", 0.0, /*ratio_known=*/false),
    };
    EXPECT_DOUBLE_EQ(expected_pnl_gap(kQty, kRawBasis, chain), 0.0);
}
