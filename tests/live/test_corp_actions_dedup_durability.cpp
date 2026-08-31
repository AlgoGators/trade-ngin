#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>

#include "trade_ngin/live/corporate_actions_applier.hpp"
#include "trade_ngin/live/corporate_actions_audit_log.hpp"

using namespace trade_ngin;

// Dedup used to live in <state_dir>/applied_corp_actions.json, under a path that
// resolves to /app/state in the container with no volume declared -- so losing it
// on redeploy was the DEFAULT. A dedup record that evaporates makes a lookback
// window wide enough to cover a real outage unsafe, because every event in it
// would be re-applied to positions. Migration 002 moved the record into
// trading.corp_action_applied; these tests pin the behaviour that makes the wide
// window safe.
namespace {

// Stands in for the DB so the durability contract is testable without one:
// rows survive independently of the filesystem, which is the whole point.
class FakeDedupDatabase : public PostgresDatabase {
public:
    FakeDedupDatabase() : PostgresDatabase("mock://dedup") {}

    Result<std::vector<AppliedCorpActionRow>> load_applied_corp_actions(
        const std::string&, const std::string&) override {
        ++load_calls;
        return Result<std::vector<AppliedCorpActionRow>>(rows);
    }

    Result<void> store_applied_corp_actions(
        const std::string&, const std::string&, const std::string&,
        const std::vector<AppliedCorpActionRow>& incoming) override {
        for (const auto& r : incoming) {
            bool dup = false;
            for (const auto& e : rows) {
                if (e.symbol == r.symbol && e.ex_date == r.ex_date &&
                    e.action_type == r.action_type) {
                    dup = true;  // mirrors ON CONFLICT DO NOTHING
                    break;
                }
            }
            if (!dup) rows.push_back(r);
        }
        return Result<void>();
    }

    std::vector<AppliedCorpActionRow> rows;
    int load_calls{0};
};

PositionAdjustment make_dividend(const std::string& symbol, const std::string& ex_date,
                                 double qty, double per_share) {
    PositionAdjustment adj;
    adj.symbol = symbol;
    adj.event_date = ex_date;
    adj.type = CorpActionType::DIVIDEND;
    adj.quantity_after = qty;
    adj.event_value = per_share;
    return adj;
}

std::filesystem::path make_temp_state_dir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("corp_dedup_" + tag + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

// The failure this whole design exists to prevent: state dir wiped (redeploy),
// dedup record still in the DB, so the event must NOT be re-applied.
TEST(CorpActionDedupDurability, SurvivesWipedStateDirectory) {
    auto db = std::make_shared<FakeDedupDatabase>();
    const auto state_dir = make_temp_state_dir("wiped");

    {
        CorporateActionsAuditLog log(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                     "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
        log.load();
        log.record(make_dividend("AAPL", "2026-08-10", 100.0, 0.27));
        ASSERT_TRUE(log.save());
    }

    // Simulate the container losing its unmounted state directory entirely.
    std::filesystem::remove_all(state_dir);
    ASSERT_FALSE(std::filesystem::exists(state_dir));

    CorporateActionsAuditLog after_redeploy(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                            "LIVE_EQUITY_MEAN_REVERSION",
                                            "EQUITY_MEAN_REVERSION");
    after_redeploy.load();
    EXPECT_TRUE(after_redeploy.is_applied("AAPL", "2026-08-10", CorpActionType::DIVIDEND))
        << "dedup must survive loss of the state directory, or a wide lookback "
           "window re-applies every event in it";
    EXPECT_DOUBLE_EQ(after_redeploy.total_cumulative_dividend_income(), 27.0);
}

// A legacy JSON file on a host upgrading to the DB-backed record must be
// imported exactly once, and re-running must not duplicate it.
TEST(CorpActionDedupDurability, LegacyStateFileImportsOnceAndIsIdempotent) {
    auto db = std::make_shared<FakeDedupDatabase>();
    const auto state_dir = make_temp_state_dir("legacy");

    {
        CorporateActionsAuditLog file_log(state_dir.string());  // file-only ctor
        file_log.load();
        file_log.record(make_dividend("MSFT", "2026-08-20", 50.0, 0.91));
        ASSERT_TRUE(file_log.save());
    }
    ASSERT_TRUE(std::filesystem::exists(state_dir / "applied_corp_actions.json"));

    CorporateActionsAuditLog first(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                   "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    first.load();
    EXPECT_TRUE(first.is_applied("MSFT", "2026-08-20", CorpActionType::DIVIDEND));
    EXPECT_EQ(db->rows.size(), 1u) << "legacy file should import exactly once";

    CorporateActionsAuditLog second(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                    "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    second.load();
    EXPECT_EQ(db->rows.size(), 1u) << "re-running must not duplicate the import";
    EXPECT_TRUE(second.is_applied("MSFT", "2026-08-20", CorpActionType::DIVIDEND));

    // The file is superseded, not deleted -- it stays as a recovery artefact.
    EXPECT_TRUE(std::filesystem::exists(state_dir / "applied_corp_actions.json"));
    std::filesystem::remove_all(state_dir);
}

// Recording the same event twice must not double-count dividend income.
TEST(CorpActionDedupDurability, RepeatedSaveDoesNotDoubleRecord) {
    auto db = std::make_shared<FakeDedupDatabase>();
    const auto state_dir = make_temp_state_dir("repeat");

    CorporateActionsAuditLog log(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                 "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    log.load();
    log.record(make_dividend("NSC", "2026-05-30", 10.0, 1.35));
    ASSERT_TRUE(log.save());
    ASSERT_TRUE(log.save());  // retried run

    CorporateActionsAuditLog reread(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                    "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    reread.load();
    EXPECT_EQ(db->rows.size(), 1u);
    EXPECT_DOUBLE_EQ(reread.total_cumulative_dividend_income(), 13.5)
        << "dividend income must not double when a run is retried";
    std::filesystem::remove_all(state_dir);
}

// Two strategies in one portfolio hold independent positions in the same symbol;
// each must be adjusted exactly once, which is why strategy_name is in the key.
TEST(CorpActionDedupDurability, DedupIsScopedPerStrategy) {
    auto db = std::make_shared<FakeDedupDatabase>();
    const auto state_dir = make_temp_state_dir("scope");

    CorporateActionsAuditLog log(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                 "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    log.load();
    EXPECT_FALSE(log.is_applied("AAPL", "2026-08-10", CorpActionType::DIVIDEND))
        << "a fresh strategy record must start empty";
    std::filesystem::remove_all(state_dir);
}
