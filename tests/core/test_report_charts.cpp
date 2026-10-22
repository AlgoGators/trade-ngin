// tests/core/test_report_charts.cpp
//
// T-EMAIL P2-2 (HD ruling Q2): a chart that is not in the report is named in its place with the
// reason, and the log gets one line with the counts.

#include <gtest/gtest.h>

#include "trade_ngin/core/report_charts.hpp"

using namespace trade_ngin::core;

TEST(ChartLedger, ADrawnChartPrintsNothingInItsPlace) {
    ChartLedger ledger;
    EXPECT_EQ(ledger.record("Equity Curve", true, ChartStatus::Drawn, "", "no data to plot"), "");
    EXPECT_EQ(ledger.summary(), "charts: 1 expected, 1 drawn");
}

TEST(ChartLedger, AFailedChartIsNamedWithTheReasonTheProcessGave) {
    ChartLedger ledger;
    const std::string note = ledger.record("Equity Curve", false, ChartStatus::Failed,
                                           "the chart program did not finish", "no data to plot");
    EXPECT_NE(note.find("Equity Curve: not shown (the chart program did not finish)"),
              std::string::npos);
}

TEST(ChartLedger, AChartWithNothingToPlotIsNamedWithItsOwnReason) {
    ChartLedger ledger;
    const std::string note = ledger.record("Portfolio Composition", false,
                                           ChartStatus::NothingToPlot, "ignored", "no positions");
    EXPECT_NE(note.find("Portfolio Composition: not shown (no positions)"), std::string::npos);
    EXPECT_EQ(note.find("ignored"), std::string::npos);
}

TEST(ChartLedger, TheLogLineCountsExpectedAndDrawnAndNamesWhatIsMissing) {
    ChartLedger ledger;
    ledger.record("Equity Curve", true, ChartStatus::Drawn, "", "");
    ledger.record("Daily PnL", false, ChartStatus::Failed, "the chart program failed", "");
    ledger.record("Portfolio Composition", false, ChartStatus::NothingToPlot, "", "no positions");
    EXPECT_EQ(ledger.expected(), 3);
    EXPECT_EQ(ledger.drawn(), 1);
    EXPECT_EQ(ledger.summary(),
              "charts: 3 expected, 1 drawn; not shown: Daily PnL (failed: the chart program "
              "failed), Portfolio Composition (no positions)");
}
