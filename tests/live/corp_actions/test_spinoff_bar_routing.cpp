// tests/live/corp_actions/test_spinoff_bar_routing.cpp
//
// E2-F47 / BA-20 -- a spinoff ex-date bar carrying BOTH class-1 columns is ONE event.
//
// PostgresDatabase::get_per_bar_corporate_actions emits a bar's `split_factor` and its
// `div_cash` as two separate CorpActionRows sharing a (ticker, ex_date). The spinoff routing
// matched each row against the terms key on its own, so a bar with both produced TWO
// SpinoffEvents: the child was delivered twice and its realized P&L booked twice. Seven real
// bars in this database carry both, and every one of them is a spinoff ex-date:
//
//   HLT  2017-01-04   split 0.3333333333   div 24.48          close 58.00
//   K    2023-10-02   split 1.065          div  3.67          close 52.50
//   MET  2017-08-07   split 1.122          div  5.87          close 48.53
//   LDOS 2013-09-30   split 0.25           div 21.3548786447  close 45.52
//   RTX  2020-04-03   split 1.589          div 13.28          close 49.93
//   ABT  2004-05-03   split 1.0688328345   div  2.9154459753  close 42.32
//   BX   2015-10-01   split 1.019          div  0.5849224898  close 31.43
//
// The parent's restatement factor therefore cannot be one column: it is the PRODUCT the
// class-1 path would have applied, split_factor x (1 + div_cash/close). That is not an
// assumption -- it is the step the vendor's own adjusted_close series takes across the bar,
// which is what the second test below checks against measured values.

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

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

}  // namespace

TEST(SpinoffBarRouting, BothColumnsMultiplyIntoOneFactorRatherThanRoutingTwice) {
    // HLT 2017-01-04, the bar that hits every one of E2-F47, E2-F48 and E2-F49 at once.
    const auto hlt = bar(0.3333333333, 24.48, 58.0);
    ASSERT_TRUE(hlt.carries_both_columns());

    EXPECT_NEAR(hlt.split_step(), 0.3333333333, 1e-12);
    EXPECT_NEAR(hlt.dividend_factor(), 1.0 + 24.48 / 58.0, 1e-12);
    EXPECT_NEAR(hlt.total_factor(), 0.3333333333 * (1.0 + 24.48 / 58.0), 1e-12);

    // Neither column on its own is the answer, which is the whole defect: routing the split
    // row alone gives 0.3333 and routing the dividend row alone gives 1.4221, and the two
    // together were applied as two separate distributions.
    EXPECT_NE(hlt.total_factor(), hlt.split_step());
    EXPECT_NE(hlt.total_factor(), hlt.dividend_factor());
}

TEST(SpinoffBarRouting, TheProductIsTheStepTheAdjustedSeriesActuallyTook) {
    // measured = (adjusted_close/close) on the ex-date bar divided by the same ratio on the
    // bar before it -- read out of equities_data.ohlcv_1d on 2026-09-03. Tolerance 2e-4
    // relative: adjusted_close is stored rounded to 10 significant digits and the raw closes
    // to the cent, so the two sides cannot agree closer than that.
    struct Case {
        const char* symbol;
        double split;
        double div;
        double close;
        double measured;
    };
    const std::vector<Case> cases = {
        {"ABT", 1.0688328345, 2.9154459753, 42.32, 1.142569},
        {"BX", 1.019, 0.5849224898, 31.43, 1.0379078},
        {"K", 1.065, 3.67, 52.50, 1.1395329},
        {"MET", 1.122, 5.87, 48.53, 1.2576932},
        {"RTX", 1.589, 13.28, 49.93, 2.0116284},
        {"LDOS", 0.25, 21.3548786447, 45.52, 0.3672580},
        {"HLT", 0.3333333333, 24.48, 58.00, 0.4739751},
        {"DD", 0.33333333, 0.0, 76.10, 0.3332989},  // split-only: the product IS the split
    };

    for (const auto& c : cases) {
        const auto columns = bar(c.split, c.div, c.close);
        EXPECT_NEAR(columns.total_factor(), c.measured, std::abs(c.measured) * 2e-4)
            << c.symbol << ": the product of the bar's class-1 columns must equal the step "
                           "the vendor's adjusted series took across the bar";
    }
}

TEST(SpinoffBarRouting, ASingleColumnBarIsUnchangedByTheProductRule) {
    // FTV 2025-06-30 (split-encoded) and MMM 2024-04-01 (dividend-encoded) are the two shapes
    // B-4 replayed. Neither carries both columns, and the product must reproduce exactly what
    // the single-column path computed, or E2-F47's fix would have moved them.
    const auto ftv = bar(1.327, 0.0, 71.60);
    EXPECT_FALSE(ftv.carries_both_columns());
    EXPECT_DOUBLE_EQ(ftv.total_factor(), 1.327);

    const auto mmm = bar(1.0, 17.3875, 94.02);
    EXPECT_FALSE(mmm.carries_both_columns());
    EXPECT_DOUBLE_EQ(mmm.total_factor(), 1.0 + 17.3875 / 94.02);
    // The number B-4's MMM replay produced and hand-checked, to the digit it logged.
    EXPECT_NEAR(mmm.total_factor(), 1.1849340565837057, 1e-12);
}

TEST(SpinoffBarRouting, AnUnusableColumnContributesExactlyOneAndNeverZero) {
    // A missing or non-positive close cannot make the dividend factor 0 or infinite: the
    // parent's basis is DIVIDED by this, so a zero would produce an infinite basis and a
    // negative one would flip its sign. An unusable column contributes the identity and the
    // event is then refused downstream for the reason that made it unusable.
    EXPECT_DOUBLE_EQ(bar(1.0, 24.48, 0.0).total_factor(), 1.0);
    EXPECT_DOUBLE_EQ(bar(1.0, 24.48, -1.0).total_factor(), 1.0);
    EXPECT_DOUBLE_EQ(bar(1.0, -5.0, 58.0).total_factor(), 1.0);

    SpinoffBarColumns nan_split;
    nan_split.has_split = true;
    nan_split.split_factor = std::nan("");
    EXPECT_DOUBLE_EQ(nan_split.total_factor(), 1.0);

    SpinoffBarColumns zero_split;
    zero_split.has_split = true;
    zero_split.split_factor = 0.0;
    EXPECT_DOUBLE_EQ(zero_split.total_factor(), 1.0);
}
