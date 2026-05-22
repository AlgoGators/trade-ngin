#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"
#include "trade_ngin/live/corporate_actions_audit_log.hpp"

using namespace trade_ngin;

// Ultrareview PR #38 fix verification.
// Pins the three behaviors that ultrareview flagged as real correctness or
// durability gaps and we resolved in this PR:
//   - bug_021 (cash-basis qty): the audit log records ex_date qty, not today's.
//   - bug_003 (atomic save):    the on-disk state file is written via tmp+rename.
// bug_007, merged_bug_001, bug_013, bug_037, bug_002 are wire-up concerns in
// the live equity app's main() and are exercised via integration / manual
// verification (the unit-test surface here is just the pure-logic layer).

namespace {

Position make_position(const std::string& symbol, double qty, double avg_price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(avg_price);
    return p;
}

CorpActionEvent dividend_event(const std::string& symbol,
                               const std::string& date,
                               double per_share,
                               double close_tm1,
                               double qty_at_ex = 0.0) {
    CorpActionEvent ev;
    ev.symbol = symbol;
    ev.ex_date = date;
    ev.type = CorpActionType::DIVIDEND;
    ev.value = per_share;
    ev.close_t_minus_1 = close_tm1;
    ev.qty_at_ex_date = qty_at_ex;
    return ev;
}

}  // namespace

// bug_021: cash-flow figure uses ex_date qty (not today's), so a catch-up
// run days after the ex_date still records the correct dividend cash.
//
// Setup: position is 100 today, but on ex_date the operator was holding 200.
// $0.50 dividend per share. The cash recorded should be 200*0.50 = $100,
// NOT 100*0.50 = $50.
TEST(CorpActionsUltrareviewFixes, DividendUsesQtyAtExDateForCashBasis) {
    std::unordered_map<std::string, Position> positions;
    positions["AAPL"] = make_position("AAPL", 100.0, 100.0);

    auto log = CorporateActionsApplier::apply(
        positions,
        {dividend_event("AAPL", "2024-08-12", 0.50, 100.0, /*qty_at_ex=*/200.0)});

    ASSERT_EQ(log.size(), 1u);
    // The recorded adjustment's quantity reflects ex_date holding (200), not
    // today's live position (100). avg_price math still uses the live position
    // (dividends don't change qty, only rescale avg_price into closeadj frame).
    EXPECT_DOUBLE_EQ(log[0].quantity_before, 200.0);
    EXPECT_DOUBLE_EQ(log[0].quantity_after, 200.0);
    // Position quantity in-memory is unchanged (qty doesn't change on dividend).
    EXPECT_DOUBLE_EQ(positions["AAPL"].quantity.as_double(), 100.0);
}

// bug_021 regression guard: when the live app couldn't reconstruct ex_date qty
// (first-week catch-up, missing historical row, etc.) and leaves qty_at_ex_date
// at 0.0, the applier falls back to the live position quantity -- preserving
// pre-fix behavior so no audit log diff for the common same-day path.
TEST(CorpActionsUltrareviewFixes, DividendFallsBackToCurrentQtyWhenExDateQtyUnset) {
    std::unordered_map<std::string, Position> positions;
    positions["AAPL"] = make_position("AAPL", 100.0, 100.0);

    auto log = CorporateActionsApplier::apply(
        positions,
        {dividend_event("AAPL", "2024-08-12", 0.50, 100.0, /*qty_at_ex=*/0.0)});

    ASSERT_EQ(log.size(), 1u);
    EXPECT_DOUBLE_EQ(log[0].quantity_before, 100.0);
    EXPECT_DOUBLE_EQ(log[0].quantity_after, 100.0);
}

// bug_021 cash-flow round-trip: the audit log uses the applier's recorded
// quantity_after as the dividend basis when computing total_cash for
// total_cumulative_dividend_income(). With qty_at_ex_date=200 and
// $0.50/share, total_cash should be $100, not $50.
TEST(CorpActionsUltrareviewFixes, AuditLogTotalCashHonorsExDateQty) {
    const std::string state_dir =
        (std::filesystem::temp_directory_path() /
         ("ca_ultrareview_div_basis_" + std::to_string(std::rand()))).string();

    std::unordered_map<std::string, Position> positions;
    positions["AAPL"] = make_position("AAPL", 100.0, 100.0);

    auto log_records = CorporateActionsApplier::apply(
        positions,
        {dividend_event("AAPL", "2024-08-12", 0.50, 100.0, /*qty_at_ex=*/200.0)});

    CorporateActionsAuditLog audit(state_dir);
    audit.load();
    for (const auto& r : log_records) audit.record(r);
    EXPECT_DOUBLE_EQ(audit.total_cumulative_dividend_income(), 100.0);

    std::error_code ec;
    std::filesystem::remove_all(state_dir, ec);
}

// bug_003: save() must be atomic -- write to a sibling .tmp and rename
// into place. After a successful save, the .tmp must not linger.
TEST(CorpActionsUltrareviewFixes, AtomicSaveLeavesNoTempFile) {
    const std::string state_dir =
        (std::filesystem::temp_directory_path() /
         ("ca_ultrareview_atomic_" + std::to_string(std::rand()))).string();

    {
        CorporateActionsAuditLog audit(state_dir);
        audit.load();
        PositionAdjustment adj;
        adj.symbol = "AAPL";
        adj.event_date = "2024-08-12";
        adj.type = CorpActionType::DIVIDEND;
        adj.quantity_before = 100.0;
        adj.quantity_after = 100.0;
        adj.avg_price_before = 100.0;
        adj.avg_price_after = 99.75;
        adj.event_value = 0.25;
        adj.ratio_change = 1.0025;
        audit.record(adj);
        ASSERT_TRUE(audit.save());
    }

    const std::string final_path = state_dir + "/applied_corp_actions.json";
    const std::string tmp_path = final_path + ".tmp";
    EXPECT_TRUE(std::filesystem::exists(final_path));
    EXPECT_FALSE(std::filesystem::exists(tmp_path))
        << "Atomic save must not leave a .tmp sibling after success";

    // Reload from disk and verify the JSON parsed (i.e. the file is not
    // corrupted from a half-write).
    CorporateActionsAuditLog reload(state_dir);
    ASSERT_TRUE(reload.load());
    EXPECT_DOUBLE_EQ(reload.total_cumulative_dividend_income(), 25.0);

    std::error_code ec;
    std::filesystem::remove_all(state_dir, ec);
}

// bug_003 regression: a stale .tmp from an earlier crash is harmless --
// the next save() overwrites it via the rename, leaving the final file
// valid and the .tmp gone.
TEST(CorpActionsUltrareviewFixes, AtomicSaveOverwritesStaleTempFile) {
    const std::string state_dir =
        (std::filesystem::temp_directory_path() /
         ("ca_ultrareview_stale_tmp_" + std::to_string(std::rand()))).string();
    std::filesystem::create_directories(state_dir);
    const std::string final_path = state_dir + "/applied_corp_actions.json";
    const std::string tmp_path = final_path + ".tmp";

    // Simulate stale tmp from a prior crash (corrupt half-JSON).
    {
        std::ofstream stale(tmp_path);
        stale << "{ \"applied\": [ corrupt";
    }
    ASSERT_TRUE(std::filesystem::exists(tmp_path));

    CorporateActionsAuditLog audit(state_dir);
    audit.load();
    PositionAdjustment adj;
    adj.symbol = "MSFT";
    adj.event_date = "2024-08-15";
    adj.type = CorpActionType::DIVIDEND;
    adj.quantity_before = 100.0;
    adj.quantity_after = 100.0;
    adj.avg_price_before = 200.0;
    adj.avg_price_after = 199.70;
    adj.event_value = 0.30;
    adj.ratio_change = 1.0015;
    audit.record(adj);
    ASSERT_TRUE(audit.save());

    EXPECT_TRUE(std::filesystem::exists(final_path));
    EXPECT_FALSE(std::filesystem::exists(tmp_path))
        << "Atomic save must clean up its tmp after rename";

    CorporateActionsAuditLog reload(state_dir);
    ASSERT_TRUE(reload.load());
    EXPECT_DOUBLE_EQ(reload.total_cumulative_dividend_income(), 30.0);

    std::error_code ec;
    std::filesystem::remove_all(state_dir, ec);
}
