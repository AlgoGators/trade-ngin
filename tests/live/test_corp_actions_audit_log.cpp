#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include "trade_ngin/live/corporate_actions_applier.hpp"
#include "trade_ngin/live/corporate_actions_audit_log.hpp"

using namespace trade_ngin;

// Phase 4 audit test T4.4 — idempotency via the on-disk dedup state file.

namespace {

class CorpActionsAuditLogTest : public ::testing::Test {
protected:
    void SetUp() override {
        state_dir_ = (std::filesystem::temp_directory_path() /
                      ("ca_audit_log_" + std::to_string(std::rand())))
                         .string();
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(state_dir_, ec);
    }
    std::string state_dir_;
};

PositionAdjustment make_split_adj(const std::string& symbol,
                                  const std::string& date) {
    PositionAdjustment adj;
    adj.symbol = symbol;
    adj.event_date = date;
    adj.type = CorpActionType::SPLIT;
    adj.quantity_before = 100.0;
    adj.quantity_after = 400.0;
    adj.avg_price_before = 400.0;
    adj.avg_price_after = 100.0;
    adj.event_value = 4.0;
    adj.ratio_change = 4.0;
    return adj;
}

PositionAdjustment make_div_adj(const std::string& symbol,
                                const std::string& date,
                                double qty_held,
                                double per_share) {
    PositionAdjustment adj;
    adj.symbol = symbol;
    adj.event_date = date;
    adj.type = CorpActionType::DIVIDEND;
    adj.quantity_before = qty_held;
    adj.quantity_after = qty_held;  // dividend doesn't change qty
    adj.avg_price_before = 100.0;
    adj.avg_price_after = 99.75;
    adj.event_value = per_share;
    adj.ratio_change = 1.0 + per_share / 100.0;
    return adj;
}

}  // namespace

// First-run: load returns false, in-memory state is empty.
TEST_F(CorpActionsAuditLogTest, FirstRunLoadReturnsFalseEmpty) {
    CorporateActionsAuditLog log(state_dir_);
    EXPECT_FALSE(log.load());
    EXPECT_FALSE(log.is_applied("AAPL", "2020-08-31", CorpActionType::SPLIT));
}

// Record, save, reload, verify the dedup record persists.
TEST_F(CorpActionsAuditLogTest, RecordSaveReloadDedupesFutureRuns) {
    {
        CorporateActionsAuditLog log(state_dir_);
        log.load();
        log.record(make_split_adj("AAPL", "2020-08-31"));
        EXPECT_TRUE(log.save());
    }
    // Fresh instance loads from disk.
    CorporateActionsAuditLog log2(state_dir_);
    EXPECT_TRUE(log2.load());
    EXPECT_TRUE(log2.is_applied("AAPL", "2020-08-31", CorpActionType::SPLIT));
    EXPECT_FALSE(log2.is_applied("AAPL", "2020-08-31", CorpActionType::DIVIDEND));
    EXPECT_FALSE(log2.is_applied("NVDA", "2024-06-10", CorpActionType::SPLIT));
}

// Dividend records also capture cash-flow detail in the dividend_events
// section of the JSON file -- this is the audit-trail substitute for the
// deferred trading.dividend_ledger table.
TEST_F(CorpActionsAuditLogTest, DividendCashFlowCapturedInState) {
    {
        CorporateActionsAuditLog log(state_dir_);
        log.load();
        log.record(make_div_adj("AAPL", "2024-08-12", 100.0, 0.25));
        ASSERT_TRUE(log.save());
    }
    // Read the file directly and confirm dividend_events has the entry.
    std::ifstream f(state_dir_ + "/applied_corp_actions.json");
    ASSERT_TRUE(f.is_open());
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("dividend_events"), std::string::npos);
    EXPECT_NE(content.find("AAPL"), std::string::npos);
    EXPECT_NE(content.find("2024-08-12"), std::string::npos);
    // total_cash = qty_held (100) * per_share (0.25) = 25.0
    EXPECT_NE(content.find("25.0"), std::string::npos);
}

// Idempotency: applying the same event twice via the audit-log dedup
// pattern must result in only one record in state. Mimics what the live
// equity app's filter_unapplied + record loop produces across two daily runs.
TEST_F(CorpActionsAuditLogTest, IdempotencyAcrossTwoRuns) {
    const auto adj = make_split_adj("NVDA", "2024-06-10");

    // Run 1: not yet applied -> record + save.
    {
        CorporateActionsAuditLog log(state_dir_);
        log.load();
        ASSERT_FALSE(log.is_applied(adj.symbol, adj.event_date, adj.type));
        log.record(adj);
        ASSERT_TRUE(log.save());
    }
    // Run 2: load -> already applied -> caller would skip.
    CorporateActionsAuditLog log2(state_dir_);
    log2.load();
    EXPECT_TRUE(log2.is_applied(adj.symbol, adj.event_date, adj.type));
    // Caller filter_unapplied would drop this event; positions stay unchanged.
}
