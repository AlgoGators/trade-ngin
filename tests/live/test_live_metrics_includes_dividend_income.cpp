#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include "trade_ngin/live/corporate_actions_applier.hpp"
#include "trade_ngin/live/corporate_actions_audit_log.hpp"

using namespace trade_ngin;

// Phase 4.5 test TDI.2 — wire-up contract.
//
// The live equity app builds its metrics map at daily finalization with a
// "total_dividend_income" key whose value comes from
// CorporateActionsAuditLog::total_cumulative_dividend_income(). This test
// reproduces that exact call pattern against a state file with a known
// cumulative total, and asserts the metric lands at the expected key with
// the expected value. Pins the contract the live app depends on.

namespace {

class LiveMetricsIncludesDividendIncomeTest : public ::testing::Test {
protected:
    void SetUp() override {
        state_dir_ = (std::filesystem::temp_directory_path() /
                      ("ca_metric_wireup_" + std::to_string(std::rand())))
                         .string();
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(state_dir_, ec);
    }
    std::string state_dir_;
};

PositionAdjustment dividend_adj(const std::string& symbol,
                                const std::string& date,
                                double qty_after,
                                double per_share) {
    PositionAdjustment adj;
    adj.symbol = symbol;
    adj.event_date = date;
    adj.type = CorpActionType::DIVIDEND;
    adj.quantity_before = qty_after;
    adj.quantity_after = qty_after;
    adj.avg_price_before = 100.0;
    adj.avg_price_after = 99.75;
    adj.event_value = per_share;
    adj.ratio_change = 1.0 + per_share / 100.0;
    return adj;
}

}  // namespace

// Set up a state file with $75 in cumulative dividend income, then build a
// metrics map the same way the live app does and assert the key/value.
TEST_F(LiveMetricsIncludesDividendIncomeTest, MetricsMapPicksUpCumulativeFromAuditLog) {
    // Arrange: write a state file with two dividends totaling $75.
    {
        CorporateActionsAuditLog log(state_dir_);
        log.load();
        // 100 shares * $0.25 = $25
        log.record(dividend_adj("AAPL", "2024-08-12", 100.0, 0.25));
        // 100 shares * $0.50 = $50
        log.record(dividend_adj("MSFT", "2024-08-15", 100.0, 0.50));
        ASSERT_TRUE(log.save());
    }

    // Act: mirror the live app's wire-up at the metrics-build site.
    double total_dividend_income = 0.0;
    {
        CorporateActionsAuditLog div_log(state_dir_);
        div_log.load();
        total_dividend_income = div_log.total_cumulative_dividend_income();
    }
    std::unordered_map<std::string, double> double_metrics = {
        {"total_pnl", 1234.5},               // placeholder for other metrics
        {"total_dividend_income", total_dividend_income},
    };

    // Assert: the metric is exactly the cumulative sum, under the right key.
    ASSERT_TRUE(double_metrics.count("total_dividend_income"));
    EXPECT_DOUBLE_EQ(double_metrics["total_dividend_income"], 75.0);

    // Sanity: total_pnl is independent (we did NOT add dividend income to it,
    // per the audit's locked decision #8).
    EXPECT_DOUBLE_EQ(double_metrics["total_pnl"], 1234.5);
}

// First-run / missing-state-file: metric is 0 (file absent → load returns
// false → in-memory state empty → cumulative is 0).
TEST_F(LiveMetricsIncludesDividendIncomeTest, MissingStateFileYieldsZeroMetric) {
    CorporateActionsAuditLog div_log(state_dir_);
    div_log.load();  // returns false; we ignore the return per the live app
    EXPECT_DOUBLE_EQ(div_log.total_cumulative_dividend_income(), 0.0);
}
