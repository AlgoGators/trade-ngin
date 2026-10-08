#include <gtest/gtest.h>
#include <cmath>
#include <string>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/market_data_utils.hpp"

// Regression test for audit finding §1.11: equity Bars must carry a continuous
// split/dividend-adjusted series, because strategy signals break across
// corporate actions otherwise.
//
// This used to be a source-text guard asserting the SQL contained
// `(closeadj / close)`. That contract is gone: the loader is now per-bar-native
// (Phase 4.2) — it reads RAW prices plus div_cash/split_factor and computes the
// backward cumulative adjustment itself, rather than trusting the vendor's
// derived adj_*/closeadj columns (whose refresh job stalled on 2026-08-06 and
// left them stale). The guard is now behavioural: exercise the real adjustment
// function against fixtures taken from production data.
//
// Fixtures below are real rows from equities_data.ohlcv_1d (raw close,
// div_cash, split_factor) paired with the vendor's own adjusted_close over a
// window where the vendor's restating job was still healthy (<= 2026-08-05).
// Our computed series must reproduce the vendor's to ~1e-6 relative, up to the
// anchor convention: we anchor factor = 1 on the last bar of the window, so we
// compare ratios (each bar's adjusted price relative to the window's last),
// which is anchor-invariant and exactly what returns depend on.

namespace {

using trade_ngin::market_data_utils::AdjustmentBar;
using trade_ngin::market_data_utils::compute_backward_adjustment_factors;

struct FixtureRow {
    double close;           // raw close
    double div_cash;        // dividend going ex on this bar
    double split_factor;    // split taking effect on this bar
    double vendor_adjusted; // vendor's adjusted_close for the same bar
};

// AAPL 2026-05-05 .. 2026-05-15: a $0.27 dividend goes ex on 05-11.
const std::vector<FixtureRow> kAaplDividend = {
    {284.18, 0.0, 1.0, 283.9180829493},
    {287.51, 0.0, 1.0, 287.2450138249},
    {287.44, 0.0, 1.0, 287.1750783410},
    {293.32, 0.0, 1.0, 293.0496589862},
    {292.68, 0.27, 1.0, 292.68},
    {294.80, 0.0, 1.0, 294.80},
    {298.87, 0.0, 1.0, 298.87},
    {298.21, 0.0, 1.0, 298.21},
    {300.23, 0.0, 1.0, 300.23},
};

// GE 2022-12-27 .. 2023-01-10: the GE HealthCare spin-off lands on 2023-01-04
// as split_factor 1.281 (in-kind distributions are encoded as split factors).
const std::vector<FixtureRow> kGeSpinoff = {
    {82.84, 0.0, 1.0, 50.6861148435},
    {81.97, 0.0, 1.0, 50.1538005036},
    {83.75, 0.0, 1.0, 51.2429034058},
    {83.79, 0.0, 1.0, 51.2673776284},
    {84.98, 0.0, 1.0, 51.9954857484},
    {70.20, 0.0, 1.281, 55.0218457344},
    {71.29, 0.0, 1.0, 55.8761735385},
    {71.94, 0.0, 1.0, 56.3856350731},
    {72.67, 0.0, 1.0, 56.9577995658},
    {75.27, 0.0, 1.0, 58.9956457041},
};

// MNST 2026-08-06 .. 2026-08-14: 2:1 split on 08-11. The vendor's adjusted
// column was NOT restated for this event (its refresh job died on 08-06), so
// only the raw/primitive side is fixtured here — this case exists to prove the
// per-bar path still produces a continuous series where the vendor's does not.
const std::vector<FixtureRow> kMnstSplitUnrestated = {
    {94.16, 0.0, 1.0, 94.16},
    {91.43, 0.0, 1.0, 91.43},
    {45.53, 0.0, 2.0, 45.53},
    {45.98, 0.0, 1.0, 45.98},
    {46.68, 0.0, 1.0, 46.68},
    {46.82, 0.0, 1.0, 46.82},
};

std::vector<AdjustmentBar> to_bars(const std::vector<FixtureRow>& rows) {
    std::vector<AdjustmentBar> bars;
    bars.reserve(rows.size());
    for (const auto& r : rows) {
        bars.push_back(AdjustmentBar{r.close, r.div_cash, r.split_factor});
    }
    return bars;
}

// Compare our adjusted series against the vendor's, normalised by the last bar
// (anchor-invariant). Returns the max relative deviation.
double max_relative_deviation(const std::vector<FixtureRow>& rows) {
    auto factors = compute_backward_adjustment_factors(to_bars(rows));
    const double ours_last = rows.back().close * factors.back();
    const double vendor_last = rows.back().vendor_adjusted;
    double worst = 0.0;
    for (size_t i = 0; i < rows.size(); ++i) {
        double ours = (rows[i].close * factors[i]) / ours_last;
        double vendor = rows[i].vendor_adjusted / vendor_last;
        worst = std::max(worst, std::abs(ours - vendor) / std::abs(vendor));
    }
    return worst;
}

}  // namespace

TEST(EquityPerBarAdjustment, ReproducesVendorSeriesAcrossDividend) {
    EXPECT_LT(max_relative_deviation(kAaplDividend), 1e-6)
        << "Per-bar dividend adjustment diverges from the vendor's adjusted "
           "series. A dividend must scale all prior bars by "
           "close/(close+div_cash).";
}

TEST(EquityPerBarAdjustment, ReproducesVendorSeriesAcrossSpinoffSplitFactor) {
    EXPECT_LT(max_relative_deviation(kGeSpinoff), 1e-6)
        << "Per-bar split/spin-off adjustment diverges from the vendor's "
           "adjusted series. In-kind distributions arrive as split_factor and "
           "must divide all prior bars.";
}

TEST(EquityPerBarAdjustment, ProducesContinuousSeriesWhereVendorColumnIsStale) {
    // The vendor's own adjusted column shows a fake -50% day here.
    const auto& rows = kMnstSplitUnrestated;
    double vendor_jump = std::abs(rows[2].vendor_adjusted - rows[1].vendor_adjusted) /
                         rows[1].vendor_adjusted;
    EXPECT_GT(vendor_jump, 0.4) << "Fixture no longer captures the stale-vendor case";

    auto factors = compute_backward_adjustment_factors(to_bars(rows));
    double ours_prev = rows[1].close * factors[1];
    double ours_split_day = rows[2].close * factors[2];
    double ours_jump = std::abs(ours_split_day - ours_prev) / ours_prev;
    EXPECT_LT(ours_jump, 0.05)
        << "Per-bar adjustment must remove the 2:1 split discontinuity even "
           "though the vendor's adjusted column never restated it.";
}

TEST(EquityPerBarAdjustment, AnchorsFactorOneOnNewestBar) {
    auto factors = compute_backward_adjustment_factors(to_bars(kAaplDividend));
    ASSERT_FALSE(factors.empty());
    EXPECT_DOUBLE_EQ(factors.back(), 1.0)
        << "Newest bar must be unadjusted so latest prices equal traded prices.";
}

TEST(EquityPerBarAdjustment, HandlesDegenerateInputs) {
    std::vector<AdjustmentBar> bars = {
        {0.0, 0.0, 1.0},    // zero close: neutral step
        {100.0, 0.0, 0.0},  // zero split factor: treated as 1.0
        {101.0, 0.0, 1.0},
    };
    auto factors = compute_backward_adjustment_factors(bars);
    ASSERT_EQ(factors.size(), 3u);
    for (double f : factors) {
        EXPECT_TRUE(std::isfinite(f)) << "Degenerate bars must not yield inf/NaN factors";
        EXPECT_GT(f, 0.0);
    }
    EXPECT_TRUE(compute_backward_adjustment_factors({}).empty());
}

TEST(EquityPerBarAdjustment, QuerySelectsPrimitivesNotVendorAdjustedColumns) {
    const std::string columns =
        trade_ngin::market_data_utils::get_market_data_columns(trade_ngin::AssetClass::EQUITIES);
    EXPECT_NE(columns.find("div_cash"), std::string::npos);
    EXPECT_NE(columns.find("split_factor"), std::string::npos);
    EXPECT_EQ(columns.find("adjusted_close"), std::string::npos)
        << "Equity reads must not depend on the vendor's derived adjusted "
           "columns (their refresh job can stall; primitives cannot).";
    EXPECT_EQ(columns.find("closeadj"), std::string::npos)
        << "closeadj lives only in the legacy sharadar table now.";

    const std::string query = trade_ngin::market_data_utils::build_equity_adjusted_query(
        "equities_data.ohlcv_1d", /*with_symbol_filter=*/true);
    EXPECT_NE(query.find("ROWS BETWEEN 1 FOLLOWING AND UNBOUNDED FOLLOWING"), std::string::npos)
        << "Backward cumulative adjustment window missing from equity query.";
    EXPECT_NE(query.find("symbol = ANY($3)"), std::string::npos);
    EXPECT_NE(query.find("open * f AS open"), std::string::npos);
}
