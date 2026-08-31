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
        const std::string&, const std::string&, const std::string& strategy_name) override {
        ++load_calls;
        // Mirrors the real WHERE clause: rows are keyed by strategy_name too,
        // so a load only sees what its own name wrote.
        std::vector<AppliedCorpActionRow> mine;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (row_names[i] == strategy_name) mine.push_back(rows[i]);
        }
        return Result<std::vector<AppliedCorpActionRow>>(mine);
    }

    Result<void> store_applied_corp_actions(
        const std::string&, const std::string&, const std::string& strategy_name,
        const std::vector<AppliedCorpActionRow>& incoming) override {
        for (const auto& r : incoming) {
            bool dup = false;
            for (size_t i = 0; i < rows.size(); ++i) {
                // ON CONFLICT covers strategy_name, so the same event under a
                // different name is a distinct row, not a duplicate.
                if (rows[i].symbol == r.symbol && rows[i].ex_date == r.ex_date &&
                    rows[i].action_type == r.action_type && row_names[i] == strategy_name) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                rows.push_back(r);
                row_names.push_back(strategy_name);
            }
        }
        return Result<void>();
    }

    std::vector<AppliedCorpActionRow> rows;
    std::vector<std::string> row_names;
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

// Two strategies can share one strategy_id: the live runners build a combined
// id, so LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST carries both TREND_FOLLOWING
// and TREND_FOLLOWING_FAST rows in trading.positions today. The dedup write has
// always keyed on strategy_name; the read did not, so one strategy received the
// other's applied events, skipped its own adjustment, and kept a permanently
// wrong cost basis -- while dividend income summed across both names.
TEST(CorpActionDedupDurability, OneStrategyDoesNotInheritAnothersAppliedEvents) {
    auto db = std::make_shared<FakeDedupDatabase>();
    const auto state_dir = make_temp_state_dir("names");

    const std::string portfolio = "BASE_PORTFOLIO";
    const std::string shared_id = "LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST";

    // Strategy A applies the event and records it.
    {
        CorporateActionsAuditLog a(state_dir.string(), db, portfolio, shared_id,
                                   "TREND_FOLLOWING");
        a.load();
        a.record(make_dividend("AAPL", "2026-08-10", 100.0, 0.27));
        ASSERT_TRUE(a.save());
    }

    // Strategy B shares the id but is a different strategy holding its own
    // position. It must NOT see A's record.
    CorporateActionsAuditLog b(state_dir.string(), db, portfolio, shared_id,
                               "TREND_FOLLOWING_FAST");
    b.load();
    EXPECT_FALSE(b.is_applied("AAPL", "2026-08-10", CorpActionType::DIVIDEND))
        << "reading the dedup record without strategy_name hands one strategy "
           "another's applied events, so B skips its own adjustment and its "
           "cost basis stays wrong for good";
    EXPECT_DOUBLE_EQ(b.total_cumulative_dividend_income(), 0.0)
        << "dividend income must not sum across every name under the id";

    // And A still sees its own after B has written its own row.
    b.record(make_dividend("AAPL", "2026-08-10", 50.0, 0.27));
    ASSERT_TRUE(b.save());

    CorporateActionsAuditLog a_again(state_dir.string(), db, portfolio, shared_id,
                                     "TREND_FOLLOWING");
    a_again.load();
    EXPECT_TRUE(a_again.is_applied("AAPL", "2026-08-10", CorpActionType::DIVIDEND));
    EXPECT_DOUBLE_EQ(a_again.total_cumulative_dividend_income(), 27.0)
        << "A owns 100 shares at 0.27; B's 50-share row belongs to B alone";
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

// ---------------------------------------------------------------------------
// Fail-closed contract.
//
// load() used to return a bare bool for three different situations: DB read
// error, read-OK-but-empty, and no-state-file. The caller could not tell them
// apart, so a transient read failure produced an EMPTY applied-set and the
// runner carried on -- making every is_applied() false and re-applying every
// event in the window. record()'s ON CONFLICT protects the table, not the
// positions already double-adjusted in memory and written to
// trading.positions. E1 widened that window from 14 days to position
// inception, so the blast radius can span years.
// ---------------------------------------------------------------------------
namespace {

// Fails only the read; store still works, so a test can seed rows first and
// then prove the failing read is not mistaken for "nothing applied yet".
class ReadFailingDedupDatabase : public FakeDedupDatabase {
public:
    Result<std::vector<AppliedCorpActionRow>> load_applied_corp_actions(
        const std::string&, const std::string&, const std::string&) override {
        ++load_calls;
        return make_error<std::vector<AppliedCorpActionRow>>(
            ErrorCode::DATABASE_ERROR, "connection reset by peer", "FakeDedup");
    }
};

}  // namespace

// 1. A read failure is reported as an error, distinct from an empty record.
TEST(CorpActionFailClosed, ReadFailureIsDistinctFromEmptyRecord) {
    const auto state_dir = make_temp_state_dir("readfail");

    auto broken = std::make_shared<ReadFailingDedupDatabase>();
    CorporateActionsAuditLog failing(state_dir.string(), broken, "EQUITY_MR_PORTFOLIO",
                                     "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    auto failed = failing.load();
    ASSERT_TRUE(failed.is_error())
        << "an unreadable dedup record must surface as an error, not as false";

    auto healthy = std::make_shared<FakeDedupDatabase>();
    CorporateActionsAuditLog empty_log(state_dir.string(), healthy, "EQUITY_MR_PORTFOLIO",
                                       "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    auto first_run = empty_log.load();
    ASSERT_FALSE(first_run.is_error()) << "a genuine first run is not an error";
    EXPECT_FALSE(first_run.value()) << "and reports 'nothing loaded'";

    std::filesystem::remove_all(state_dir);
}

// 2. Genuine first run still proceeds normally.
TEST(CorpActionFailClosed, GenuineFirstRunProceeds) {
    auto db = std::make_shared<FakeDedupDatabase>();
    const auto state_dir = make_temp_state_dir("firstrun");

    CorporateActionsAuditLog log(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                 "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    auto loaded = log.load();
    ASSERT_FALSE(loaded.is_error());
    EXPECT_FALSE(loaded.value());
    EXPECT_FALSE(log.is_applied("AAPL", "2026-08-10", CorpActionType::DIVIDEND));

    std::filesystem::remove_all(state_dir);
}

// 3. Legacy file present, DB empty: import runs and the load succeeds.
TEST(CorpActionFailClosed, LegacyFileImportStillWorks) {
    auto db = std::make_shared<FakeDedupDatabase>();
    const auto state_dir = make_temp_state_dir("legacy");

    {
        CorporateActionsAuditLog file_only(state_dir.string());
        file_only.record(make_dividend("MSFT", "2026-08-20", 50.0, 0.91));
        ASSERT_TRUE(file_only.save());
    }

    CorporateActionsAuditLog log(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                 "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    auto loaded = log.load();
    ASSERT_FALSE(loaded.is_error()) << "a successful import is not a failure";
    EXPECT_TRUE(loaded.value()) << "imported rows must be visible to this run";
    EXPECT_TRUE(log.is_applied("MSFT", "2026-08-20", CorpActionType::DIVIDEND))
        << "the imported event must not be re-applied";

    std::filesystem::remove_all(state_dir);
}

// 4. The regression itself: on read failure the applier must NOT be handed an
//    empty applied-set that would re-apply an event the DB already recorded.
TEST(CorpActionFailClosed, ReadFailureDoesNotYieldAnEmptyAppliedSet) {
    const auto state_dir = make_temp_state_dir("noempty");

    // The event IS applied as far as durable state is concerned.
    auto broken = std::make_shared<ReadFailingDedupDatabase>();
    broken->rows.push_back([] {
        PostgresDatabase::AppliedCorpActionRow r;
        r.symbol = "AAPL";
        r.ex_date = "2026-08-10";
        r.action_type = "DIVIDEND";
        r.qty_held = 100.0;
        r.dividend_per_share = 0.27;
        r.total_cash = 27.0;
        return r;
    }());
    broken->row_names.push_back("EQUITY_MEAN_REVERSION");

    CorporateActionsAuditLog log(state_dir.string(), broken, "EQUITY_MR_PORTFOLIO",
                                 "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    auto loaded = log.load();

    // The in-memory set IS empty here -- that is unavoidable, the read failed.
    // The contract that protects positions is that load() reports the failure so
    // the caller aborts instead of trusting this empty set. Were it to report
    // "false / first run" (the old behaviour), the runner would proceed, find
    // is_applied() false, and re-apply a dividend already recorded.
    ASSERT_TRUE(loaded.is_error())
        << "silently returning an empty applied-set re-applies every event in the window";
    EXPECT_FALSE(log.is_applied("AAPL", "2026-08-10", CorpActionType::DIVIDEND))
        << "the empty set is exactly why the error must be propagated";

    std::filesystem::remove_all(state_dir);
}
