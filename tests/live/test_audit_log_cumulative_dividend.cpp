#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <string>
#include "trade_ngin/live/corporate_actions_applier.hpp"
#include "trade_ngin/live/corporate_actions_audit_log.hpp"

using namespace trade_ngin;

// Phase 4.5 test TDI.1 — total_cumulative_dividend_income() accessor.
// Sums total_cash across all recorded DIVIDEND events; SPLITs are not
// included. Persisted to disk via save() and reloadable via load().

namespace {

class AuditLogCumulativeDividendTest : public ::testing::Test {
protected:
    void SetUp() override {
        state_dir_ = (std::filesystem::temp_directory_path() /
                      ("ca_cum_div_" + std::to_string(std::rand())))
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

PositionAdjustment split_adj(const std::string& symbol,
                             const std::string& date,
                             double factor) {
    PositionAdjustment adj;
    adj.symbol = symbol;
    adj.event_date = date;
    adj.type = CorpActionType::SPLIT;
    adj.quantity_before = 100.0;
    adj.quantity_after = 100.0 * factor;
    adj.avg_price_before = 400.0;
    adj.avg_price_after = 400.0 / factor;
    adj.event_value = factor;
    adj.ratio_change = factor;
    return adj;
}

}  // namespace

// Empty log returns 0.
TEST_F(AuditLogCumulativeDividendTest, EmptyLogReturnsZero) {
    CorporateActionsAuditLog log(state_dir_);
    log.load();
    EXPECT_DOUBLE_EQ(log.total_cumulative_dividend_income(), 0.0);
}

// Three dividends sum correctly.
TEST_F(AuditLogCumulativeDividendTest, ThreeDividendsSumTotalCash) {
    CorporateActionsAuditLog log(state_dir_);
    log.load();
    // qty 100 * $0.25 = $25
    log.record(dividend_adj("AAPL", "2024-08-12", 100.0, 0.25));
    // qty 100 * $0.30 = $30
    log.record(dividend_adj("MSFT", "2024-08-15", 100.0, 0.30));
    // qty 100 * $0.50 = $50
    log.record(dividend_adj("NSC", "2024-09-01", 100.0, 0.50));

    EXPECT_DOUBLE_EQ(log.total_cumulative_dividend_income(), 105.0);
}

// Splits are NOT included in the cumulative sum.
TEST_F(AuditLogCumulativeDividendTest, SplitsDoNotContribute) {
    CorporateActionsAuditLog log(state_dir_);
    log.load();
    log.record(dividend_adj("AAPL", "2024-08-12", 100.0, 0.25));   // $25
    log.record(split_adj("NVDA", "2024-06-10", 10.0));             // $0 (split)
    log.record(dividend_adj("MSFT", "2024-08-15", 100.0, 0.30));   // $30
    log.record(split_adj("TSLA", "2022-08-25", 3.0));              // $0 (split)
    EXPECT_DOUBLE_EQ(log.total_cumulative_dividend_income(), 55.0);
}

// Sum survives save/reload round-trip.
TEST_F(AuditLogCumulativeDividendTest, SumPersistsAcrossReload) {
    {
        CorporateActionsAuditLog log(state_dir_);
        log.load();
        log.record(dividend_adj("AAPL", "2024-08-12", 100.0, 0.25));
        log.record(dividend_adj("MSFT", "2024-08-15", 100.0, 0.30));
        ASSERT_TRUE(log.save());
    }
    CorporateActionsAuditLog log2(state_dir_);
    ASSERT_TRUE(log2.load());
    EXPECT_DOUBLE_EQ(log2.total_cumulative_dividend_income(), 55.0);
}

// Fractional quantities and fractional dividends (after stacked dividends
// scale avg_price, qty_held is what's reported pre-rescale -- but the
// accessor just sums whatever is in the file).
TEST_F(AuditLogCumulativeDividendTest, FractionalSumsExactly) {
    CorporateActionsAuditLog log(state_dir_);
    log.load();
    // 50 shares * $0.1234 = $6.17
    log.record(dividend_adj("FOO", "2024-01-01", 50.0, 0.1234));
    // 25 shares * $0.0500 = $1.25
    log.record(dividend_adj("BAR", "2024-02-01", 25.0, 0.05));
    // Total: 6.17 + 1.25 = 7.42
    EXPECT_NEAR(log.total_cumulative_dividend_income(), 7.42, 1e-9);
}
