// CorporateActionsAuditLog -- the dedup record that makes corp-action
// application idempotent, plus the DB/file backing behind it.
//
// Consolidated from test_corp_actions_audit_log.cpp, test_corp_actions_dedup_durability.cpp
// and the audit-log half of test_corp_actions_ultrareview_fixes.cpp.
//
// Contracts worth keeping in view: a read FAILURE must stay distinguishable
// from an empty record (conflating them yields an empty applied-set, which
// re-applies every event in the window); the record is keyed on strategy_name
// as well as strategy_id, so two strategies sharing an id cannot inherit each
// other's entries; and entries are mirrored across ticker renames, bounded by
// the alias effective date, because the vendor migrates a renamed symbol's
// whole history to its successor.

#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include "trade_ngin/live/corporate_actions_applier.hpp"
#include "trade_ngin/live/corporate_actions_audit_log.hpp"
#include <memory>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/types.hpp"

// ---------------------------------------------------------------------------
// SUBJECT: the record's basic contract -- first run is empty, a recorded
// event survives save/reload, and a second run does not re-apply it.
// from test_corp_actions_audit_log.cpp
// Wrapped in a namespace so its file-local helpers cannot collide with the
// other sections; gtest test identities are unaffected.
// ---------------------------------------------------------------------------
namespace audit_core {

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
    EXPECT_FALSE(log.load().value());
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
    EXPECT_TRUE(log2.load().value());
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

}  // namespace audit_core

// ---------------------------------------------------------------------------
// SUBJECT: dedup that survives reality -- a wiped state directory, a legacy
// file import, two strategies sharing one strategy_id, and a read failure
// that must NOT read as 'nothing applied yet'. Re-application is permanent
// corruption, so these are the highest-stakes cases in the component.
// from test_corp_actions_dedup_durability.cpp
// Wrapped in a namespace so its file-local helpers cannot collide with the
// other sections; gtest test identities are unaffected.
// ---------------------------------------------------------------------------
namespace audit_dedup {

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

    // The real load() consults ticker_aliases to bridge renames, so the mock
    // must answer too. Empty by default: tests that care set `aliases`.
    Result<std::vector<TickerAliasRow>> get_ticker_aliases() override {
        if (alias_read_fails) {
            return make_error<std::vector<TickerAliasRow>>(
                ErrorCode::DATABASE_ERROR, "simulated alias read failure", "FakeDedupDatabase");
        }
        return Result<std::vector<TickerAliasRow>>(aliases);
    }

    std::vector<AppliedCorpActionRow> rows;
    std::vector<std::string> row_names;
    std::vector<TickerAliasRow> aliases;
    bool alias_read_fails{false};
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

// A symbol renamed between runs must not have its already-applied events
// re-applied under the new ticker. The vendor migrates a renamed symbol's whole
// history to the current ticker (AA has 0 bars; HWM carries 6,703 back to 2000),
// so the event genuinely resurfaces under the new name -- only the dedup bridge
// stops it being applied a second time.
TEST(CorpActionRenameBridge, AppliedEventIsNotReAppliedUnderTheNewTicker) {
    auto db = std::make_shared<FakeDedupDatabase>();
    PostgresDatabase::TickerAliasRow alias;
    alias.historical_ticker = "AA";
    alias.current_symbol = "HWM";
    alias.effective_until = "2016-11-01";
    db->aliases.push_back(alias);

    const auto state_dir = make_temp_state_dir("rename_bridge");
    {
        CorporateActionsAuditLog log(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                     "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
        ASSERT_TRUE(log.load().is_ok());
        log.record(make_dividend("AA", "2016-05-10", 100.0, 0.27));
        ASSERT_TRUE(log.save());
    }

    // Next run: apply_renames has re-keyed the position AA -> HWM, and the bars
    // now carry that same dividend under HWM.
    CorporateActionsAuditLog after(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                  "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    ASSERT_TRUE(after.load().is_ok());
    EXPECT_TRUE(after.is_applied("HWM", "2016-05-10", CorpActionType::DIVIDEND))
        << "dividend applied under AA must be seen as applied under HWM; "
           "otherwise it is applied twice and the cost basis is permanently wrong";
    // The original key must still match, for a book that has not been re-keyed yet.
    EXPECT_TRUE(after.is_applied("AA", "2016-05-10", CorpActionType::DIVIDEND));
    // An unrelated symbol is unaffected.
    EXPECT_FALSE(after.is_applied("MSFT", "2016-05-10", CorpActionType::DIVIDEND));
}

// The alias map is what stops re-application across a rename, so an unreadable
// map must fail closed rather than silently dropping the bridge.
TEST(CorpActionRenameBridge, UnreadableAliasMapFailsClosed) {
    auto db = std::make_shared<FakeDedupDatabase>();
    db->alias_read_fails = true;
    const auto state_dir = make_temp_state_dir("alias_fail");
    CorporateActionsAuditLog log(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                 "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    EXPECT_TRUE(log.load().is_error())
        << "a missing alias map silently reopens the re-application path";
}

// effective_until is the rename DATE, so an event maps to the successor only
// when it predates the rename. Tickers are reused -- 33 historical_tickers in
// the live table carry two or more successors -- so mirroring without that
// bound both picks an arbitrary winner and can mask a genuine event on the
// unrelated company that now holds the ticker.
TEST(CorpActionRenameBridge, EventAfterTheRenameIsNotMirrored) {
    auto db = std::make_shared<FakeDedupDatabase>();
    PostgresDatabase::TickerAliasRow alias;
    alias.historical_ticker = "AA";
    alias.current_symbol = "HWM";
    alias.effective_until = "2016-11-01";
    db->aliases.push_back(alias);

    const auto state_dir = make_temp_state_dir("rename_after");
    {
        CorporateActionsAuditLog log(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                     "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
        ASSERT_TRUE(log.load().is_ok());
        // Dated AFTER the rename: at this point "AA" is whoever was reassigned
        // the ticker, a different company from the one that became HWM.
        log.record(make_dividend("AA", "2020-05-10", 100.0, 0.27));
        ASSERT_TRUE(log.save());
    }

    CorporateActionsAuditLog after(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                   "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    ASSERT_TRUE(after.load().is_ok());
    EXPECT_TRUE(after.is_applied("AA", "2020-05-10", CorpActionType::DIVIDEND));
    EXPECT_FALSE(after.is_applied("HWM", "2020-05-10", CorpActionType::DIVIDEND))
        << "an event dated after the rename belongs to the reused ticker, not to "
           "the successor; mirroring it would mask a genuine HWM event sharing "
           "that ex_date and action, skipping its adjustment entirely";
}

// A reused ticker with two successors must route each event to the company that
// held it at the time, not to whichever alias row happened to be read last.
TEST(CorpActionRenameBridge, ReusedTickerRoutesEachEventToItsOwnEra) {
    auto db = std::make_shared<FakeDedupDatabase>();
    PostgresDatabase::TickerAliasRow first;
    first.historical_ticker = "BBT";
    first.current_symbol = "BBT1";
    first.effective_until = "1998-12-10";
    PostgresDatabase::TickerAliasRow second;
    second.historical_ticker = "BBT";
    second.current_symbol = "TFC";
    second.effective_until = "2019-12-10";
    // Deliberately out of chronological order: resolution must not depend on
    // read order.
    db->aliases.push_back(second);
    db->aliases.push_back(first);

    const auto state_dir = make_temp_state_dir("rename_reuse");
    {
        CorporateActionsAuditLog log(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                     "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
        ASSERT_TRUE(log.load().is_ok());
        log.record(make_dividend("BBT", "1998-06-01", 100.0, 0.10));  // first era
        log.record(make_dividend("BBT", "2015-06-01", 100.0, 0.20));  // second era
        ASSERT_TRUE(log.save());
    }

    CorporateActionsAuditLog after(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                   "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    ASSERT_TRUE(after.load().is_ok());
    EXPECT_TRUE(after.is_applied("BBT1", "1998-06-01", CorpActionType::DIVIDEND))
        << "the 1998 event predates the first rename, so it belongs to BBT1";
    EXPECT_TRUE(after.is_applied("TFC", "2015-06-01", CorpActionType::DIVIDEND))
        << "the 2015 event falls between the two renames, so it belongs to TFC";
    // Cross-era leakage is the failure the date bound exists to prevent.
    EXPECT_FALSE(after.is_applied("BBT1", "2015-06-01", CorpActionType::DIVIDEND));
}

// Only the DB backing can bridge a rename, because the bridge reads
// ticker_aliases. A file-backed log answers on the pre-rename symbol alone, so
// the live runner refuses to adjust positions with one -- this pins the
// distinction the runner checks.
TEST(CorpActionRenameBridge, OnlyTheDatabaseBackingBridgesRenames) {
    const auto state_dir = make_temp_state_dir("bridge_capability");

    CorporateActionsAuditLog file_backed(state_dir.string());
    EXPECT_FALSE(file_backed.bridges_renames())
        << "a file-backed log has no alias source, so it cannot recognise an "
           "event across a rename and must not be used for that decision";

    auto db = std::make_shared<FakeDedupDatabase>();
    CorporateActionsAuditLog db_backed(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                       "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    EXPECT_TRUE(db_backed.bridges_renames());
}

}  // namespace audit_dedup

// ---------------------------------------------------------------------------
// SUBJECT: the cash figure recorded per event, measured at the ex-date.
// from ur_audit.cpp
// Wrapped in a namespace so its file-local helpers cannot collide with the
// other sections; gtest test identities are unaffected.
// ---------------------------------------------------------------------------
namespace audit_ex_date_cash {

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
    ev.close_at_ex_date = close_tm1;
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
    ASSERT_TRUE(reload.load().value());
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
    ASSERT_TRUE(reload.load().value());
    EXPECT_DOUBLE_EQ(reload.total_cumulative_dividend_income(), 30.0);

    std::error_code ec;
    std::filesystem::remove_all(state_dir, ec);
}

}  // namespace audit_ex_date_cash

// ──────────────────────────────────────────────────────────────────────────
// E2-F39 / BA-15 -- the legacy state-file import must key dividend detail on
// the ACTION TYPE too, not just (symbol, ex_date).
//
// The file records applied events as (symbol, ex_date, action_type) but carries
// dividend detail as (symbol, ex_date) only -- DividendEvent has no type field,
// because everything in that list IS a dividend. The import matched on the pair
// alone, so a SPLIT sharing an ex-date with a DIVIDEND inherited the dividend's
// qty_held / dividend_per_share / total_cash. The same cash then shows up twice
// in cumulative dividend income: on the dividend row that paid it and on the
// split row that paid nothing.
//
// Same-day split-and-dividend is not exotic: a company declaring a split
// commonly sets its ex-date to a dividend ex-date so the two settle together.
// ──────────────────────────────────────────────────────────────────────────

using trade_ngin::CorpActionType;
using trade_ngin::CorporateActionsAuditLog;
using DividendEvent = CorporateActionsAuditLog::DividendEvent;

TEST(LegacyImportDividendDetail, ASplitSharingAnExDateDoesNotInheritTheDividendCash) {
    const std::vector<DividendEvent> events{
        {"AAPL", "2026-08-10", /*qty*/ 250.0, /*dps*/ 0.24, /*cash*/ 60.0}};

    // The dividend gets its detail.
    const auto* div = CorporateActionsAuditLog::dividend_detail_for(
        "AAPL", "2026-08-10", CorpActionType::DIVIDEND, events);
    ASSERT_NE(div, nullptr) << "a dividend must still receive its own detail";
    EXPECT_DOUBLE_EQ(div->total_cash, 60.0);
    EXPECT_DOUBLE_EQ(div->qty_held, 250.0);
    EXPECT_DOUBLE_EQ(div->dividend_per_share, 0.24);

    // The split on the SAME symbol and SAME ex-date gets nothing.
    EXPECT_EQ(CorporateActionsAuditLog::dividend_detail_for(
                  "AAPL", "2026-08-10", CorpActionType::SPLIT, events),
              nullptr)
        << "a split pays no dividend; inheriting 60.0 here double-counts the cash";

    // Neither do the other non-paying classes.
    EXPECT_EQ(CorporateActionsAuditLog::dividend_detail_for(
                  "AAPL", "2026-08-10", CorpActionType::ADR_SPLIT, events),
              nullptr);
    EXPECT_EQ(CorporateActionsAuditLog::dividend_detail_for(
                  "AAPL", "2026-08-10", CorpActionType::TERMINATION, events),
              nullptr);
    EXPECT_EQ(CorporateActionsAuditLog::dividend_detail_for(
                  "AAPL", "2026-08-10", CorpActionType::UNKNOWN, events),
              nullptr);
}

TEST(LegacyImportDividendDetail, DetailStillMatchesOnSymbolAndExDate) {
    const std::vector<DividendEvent> events{
        {"AAPL", "2026-08-10", 250.0, 0.24, 60.0},
        {"MSFT", "2026-08-10", 80.0, 0.75, 60.0},   // same cash, different symbol
        {"AAPL", "2026-05-11", 250.0, 0.24, 60.0},  // same symbol, different date
    };

    const auto* a = CorporateActionsAuditLog::dividend_detail_for(
        "AAPL", "2026-08-10", CorpActionType::DIVIDEND, events);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->symbol, "AAPL");
    EXPECT_EQ(a->ex_date, "2026-08-10");
    EXPECT_DOUBLE_EQ(a->qty_held, 250.0);

    const auto* m = CorporateActionsAuditLog::dividend_detail_for(
        "MSFT", "2026-08-10", CorpActionType::DIVIDEND, events);
    ASSERT_NE(m, nullptr);
    EXPECT_DOUBLE_EQ(m->dividend_per_share, 0.75) << "the right row, not the first one";

    // A dividend with no detail in the file is not an error -- it simply carries none.
    EXPECT_EQ(CorporateActionsAuditLog::dividend_detail_for(
                  "TMUS", "2026-08-10", CorpActionType::DIVIDEND, events),
              nullptr);
    EXPECT_EQ(CorporateActionsAuditLog::dividend_detail_for(
                  "AAPL", "2026-08-11", CorpActionType::DIVIDEND, events),
              nullptr);
}

TEST(LegacyImportDividendDetail, AnEmptyDetailListYieldsNothingForAnyType) {
    const std::vector<DividendEvent> none;
    for (auto t : {CorpActionType::DIVIDEND, CorpActionType::SPLIT,
                   CorpActionType::ADR_SPLIT, CorpActionType::TERMINATION}) {
        EXPECT_EQ(CorporateActionsAuditLog::dividend_detail_for("AAPL", "2026-08-10", t, none),
                  nullptr);
    }
}

// ---------------------------------------------------------------------------
// E2-F23 (audit option C-prime) -- dedup rows from a LATER pass must stop the run.
//
// A live run is correct exactly when the dedup rows and the T-1 position row come
// from the SAME pass. Reset the book but not trading.corp_action_applied and it is
// not: the event is skipped as "already applied" against a T-1 row that predates
// it, T-1 is finalized in the wrong frame, and the position is marked against a
// basis that never moved. Measured: BKNG 25:1, ex-date 2026-04-06, re-run of
// 2026-04-07 over an un-reset dedup table, -4,824 of phantom unrealized P&L.
//
// Nothing downstream can catch it. The G3 basis/mark bound only inspects events
// that were APPLIED, and a deduped event never enters that list. The existing
// ex_date >= today detector cannot see it either: 2026-04-06 is safely in the
// past. `applied_at` cannot, because an earlier chain always carries an earlier
// wall clock. The RUN DATE of the writing pass is the only value that separates
// an earlier pass from a later one.
// ---------------------------------------------------------------------------

namespace audit_run_date {

using namespace trade_ngin;
using audit_dedup::FakeDedupDatabase;
using audit_dedup::make_temp_state_dir;

namespace {

PositionAdjustment split_adjustment(const std::string& symbol, const std::string& ex_date,
                                    double factor) {
    PositionAdjustment adj;
    adj.symbol = symbol;
    adj.event_date = ex_date;
    adj.type = CorpActionType::SPLIT;
    adj.event_value = factor;
    return adj;
}

}  // namespace

TEST(CorpActionRunDateGuard, ARowFromALaterPassRefusesTheRun) {
    const auto state_dir = make_temp_state_dir("rundate_later");
    auto db = std::make_shared<FakeDedupDatabase>();

    // Pass 1 runs 2026-04-07 and applies the split, stamping its own run date.
    {
        CorporateActionsAuditLog first(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                       "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
        first.set_run_date("2026-04-07");
        ASSERT_FALSE(first.load().is_error());
        first.record(split_adjustment("BKNG", "2026-04-06", 25.0));
        ASSERT_TRUE(first.save());
    }

    // The book is re-seeded and 2026-04-07 is replayed, but the dedup table was
    // not reset. This is the measured F23 case, and it must not start.
    CorporateActionsAuditLog rerun(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                   "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    rerun.set_run_date("2026-04-07");
    auto loaded = rerun.load();
    ASSERT_TRUE(loaded.is_error())
        << "a dedup row written by the run of 2026-04-07 must stop a second run of "
           "2026-04-07: the T-1 book it would be trusted against is a different pass's";
    const std::string what = loaded.error()->what();
    EXPECT_NE(what.find("BKNG"), std::string::npos) << "the message must name the stale rows";
    EXPECT_NE(what.find("2026-04-06"), std::string::npos) << "and their ex-dates";
    EXPECT_NE(what.find("2026-04-07"), std::string::npos) << "and the run date that wrote them";

    // The ex-date detector could never have caught this one.
    EXPECT_LT(std::string("2026-04-06"), std::string("2026-04-07"))
        << "the ex-date is in the past, which is why run_date is the load-bearing column";

    // And a run of a LATER date, whose T-1 book really is this pass's output, is
    // unaffected -- the guard must not block ordinary forward progress.
    CorporateActionsAuditLog next_day(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                      "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    next_day.set_run_date("2026-04-08");
    auto ok = next_day.load();
    ASSERT_FALSE(ok.is_error()) << "forward-only running must stay unblocked";
    EXPECT_TRUE(ok.value());
    EXPECT_TRUE(next_day.is_applied("BKNG", "2026-04-06", CorpActionType::SPLIT));

    std::filesystem::remove_all(state_dir);
}

TEST(CorpActionRunDateGuard, LegacyRowsWithNoRunDatePass) {
    const auto state_dir = make_temp_state_dir("rundate_legacy");
    auto db = std::make_shared<FakeDedupDatabase>();

    // A row written before migration 005: no run date, so nothing is known about
    // which pass wrote it. Refusing it would make the first run after the
    // migration unstartable, on a table that is append-only and years old.
    {
        CorporateActionsAuditLog legacy(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                        "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
        // No set_run_date: this is exactly the pre-005 write path.
        ASSERT_FALSE(legacy.load().is_error());
        legacy.record(split_adjustment("BKNG", "2026-04-06", 25.0));
        ASSERT_TRUE(legacy.save());
    }

    CorporateActionsAuditLog after(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                   "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    after.set_run_date("2026-04-07");
    auto loaded = after.load();
    ASSERT_FALSE(loaded.is_error())
        << "a NULL run_date means UNKNOWN, and unknown must not block the run";
    EXPECT_TRUE(loaded.value());
    EXPECT_TRUE(after.is_applied("BKNG", "2026-04-06", CorpActionType::SPLIT))
        << "and the legacy row still dedups, or the migration would re-apply history";

    std::filesystem::remove_all(state_dir);
}

TEST(CorpActionRunDateGuard, StampOnlyLetsASecondLogInTheSameRunProceed) {
    // The class-3 lifecycle log opens AFTER the class-1 block has committed this
    // run's rows. Enforcing there would make the run refuse its own output.
    const auto state_dir = make_temp_state_dir("rundate_stamponly");
    auto db = std::make_shared<FakeDedupDatabase>();

    CorporateActionsAuditLog class1(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                    "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    class1.set_run_date("2026-04-07");
    ASSERT_FALSE(class1.load().is_error());
    class1.record(split_adjustment("BKNG", "2026-04-06", 25.0));
    ASSERT_TRUE(class1.save());

    CorporateActionsAuditLog class3(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                    "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    class3.set_run_date("2026-04-07",
                        CorporateActionsAuditLog::RunDateCheck::StampOnly);
    auto loaded = class3.load();
    ASSERT_FALSE(loaded.is_error())
        << "the same run's own rows must not be mistaken for a later pass's";

    // Enforce on the same state is the failing case -- which is what proves the
    // mode is doing something rather than being decoration.
    CorporateActionsAuditLog enforcing(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                       "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    enforcing.set_run_date("2026-04-07");
    EXPECT_TRUE(enforcing.load().is_error());

    std::filesystem::remove_all(state_dir);
}

TEST(CorpActionRunDateGuard, AnUnstampedRunNeitherStampsNorChecks) {
    // Pre-005 behaviour is preserved exactly for any caller that sets no run date:
    // rows are written with no stamp and no load is ever refused. This is what the
    // file-backed tests and every other caller rely on.
    const auto state_dir = make_temp_state_dir("rundate_none");
    auto db = std::make_shared<FakeDedupDatabase>();

    CorporateActionsAuditLog stamped(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                     "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    stamped.set_run_date("2026-04-07");
    ASSERT_FALSE(stamped.load().is_error());
    stamped.record(split_adjustment("BKNG", "2026-04-06", 25.0));
    ASSERT_TRUE(stamped.save());

    CorporateActionsAuditLog unstamped(state_dir.string(), db, "EQUITY_MR_PORTFOLIO",
                                       "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION");
    auto loaded = unstamped.load();
    EXPECT_FALSE(loaded.is_error())
        << "a caller that sets no run date must behave exactly as it did before 005";
    EXPECT_TRUE(unstamped.is_applied("BKNG", "2026-04-06", CorpActionType::SPLIT));

    std::filesystem::remove_all(state_dir);
}

}  // namespace audit_run_date
