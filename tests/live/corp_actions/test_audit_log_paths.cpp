// CorporateActionsAuditLog: the paths test_audit_log.cpp does not reach.
// The state file's malformed entries and write failures; the DB-backed save's
// failure and retry; what record() puts in the row it persists; basis_chain();
// the stale-row message bound; rename chains and ignored alias rows; and the
// legacy-file import when its write fails.

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"
#include "trade_ngin/live/corporate_actions_audit_log.hpp"

using namespace trade_ngin;
namespace fs = std::filesystem;

namespace {

using Row = PostgresDatabase::AppliedCorpActionRow;
using TickerAliasRow = PostgresDatabase::TickerAliasRow;

class ScriptedDedupDb : public PostgresDatabase {
public:
    ScriptedDedupDb() : PostgresDatabase("mock://dedup_paths") {}

    Result<std::vector<Row>> load_applied_corp_actions(const std::string&, const std::string&,
                                                       const std::string&) override {
        ++load_calls;
        if (fail_load_on_call == load_calls) {
            return make_error<std::vector<Row>>(ErrorCode::DATABASE_ERROR, "scripted read failure",
                                                "ScriptedDedupDb");
        }
        return Result<std::vector<Row>>(rows);
    }

    Result<void> store_applied_corp_actions(const std::string&, const std::string&,
                                            const std::string&,
                                            const std::vector<Row>& incoming) override {
        ++store_calls;
        if (fail_store) {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "scripted write failure",
                                    "ScriptedDedupDb");
        }
        rows.insert(rows.end(), incoming.begin(), incoming.end());
        return Result<void>();
    }

    Result<std::vector<TickerAliasRow>> get_ticker_aliases() override {
        return Result<std::vector<TickerAliasRow>>(aliases);
    }

    std::vector<Row> rows;
    std::vector<TickerAliasRow> aliases;
    bool fail_store{false};
    int fail_load_on_call{0};  // 1-based; 0 never fails
    int load_calls{0};
    int store_calls{0};
};

class AuditLogPathsTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("ca_paths_" + std::to_string(::getpid()) + "_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        db_ = std::make_shared<ScriptedDedupDb>();
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    CorporateActionsAuditLog db_log() {
        return CorporateActionsAuditLog(dir_.string(), db_, "PORT", "STRAT", "NAME");
    }

    void write_state_file(const std::string& body) {
        std::ofstream f(dir_ / "applied_corp_actions.json");
        f << body;
    }

    fs::path dir_;
    std::shared_ptr<ScriptedDedupDb> db_;
};

PositionAdjustment adj(const std::string& symbol, const std::string& date, CorpActionType type,
                       double ratio, double qty_after = 0.0, double value = 0.0) {
    PositionAdjustment a;
    a.symbol = symbol;
    a.event_date = date;
    a.type = type;
    a.ratio_change = ratio;
    a.quantity_after = qty_after;
    a.event_value = value;
    return a;
}

Row row(const std::string& symbol, const std::string& ex_date, const std::string& type,
        const std::string& run_date = "") {
    Row r;
    r.symbol = symbol;
    r.ex_date = ex_date;
    r.action_type = type;
    r.run_date = run_date;
    return r;
}

TickerAliasRow alias(const std::string& historical, const std::string& current,
                     const std::string& until) {
    TickerAliasRow a;
    a.historical_ticker = historical;
    a.current_symbol = current;
    a.effective_until = until;
    return a;
}

}  // namespace

// ---------------------------------------------------------------------------
// File backing
// ---------------------------------------------------------------------------

TEST_F(AuditLogPathsTest, FileEntriesMissingAKeyAreSkipped) {
    write_state_file(R"({
      "applied": [
        {"symbol": "AAPL", "ex_date": "2026-01-02", "action": "SPLIT"},
        {"symbol": "MSFT", "ex_date": "2026-01-02"},
        {"symbol": "NVDA", "action": "SPLIT"},
        {"ex_date": "2026-01-02", "action": "SPLIT"}
      ],
      "dividend_events": [
        {"symbol": "KO", "ex_date": "2026-01-05", "total_cash": 12.5},
        {"symbol": "PEP"}
      ]
    })");
    CorporateActionsAuditLog log(dir_.string());
    auto r = log.load();
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value());
    EXPECT_TRUE(log.is_applied("AAPL", "2026-01-02", CorpActionType::SPLIT));
    for (auto type : {CorpActionType::SPLIT, CorpActionType::ADR_SPLIT, CorpActionType::DIVIDEND,
                      CorpActionType::TERMINATION, CorpActionType::SPINOFF,
                      CorpActionType::UNKNOWN}) {
        EXPECT_FALSE(log.is_applied("MSFT", "2026-01-02", type));
        EXPECT_FALSE(log.is_applied("NVDA", "", type));
        EXPECT_FALSE(log.is_applied("", "2026-01-02", type));
    }
    // A dividend entry with missing fields reads them as zero; the cash sums.
    EXPECT_DOUBLE_EQ(log.total_cumulative_dividend_income(), 12.5);
}

TEST_F(AuditLogPathsTest, UnparseableFileIsReadAsAFirstRun) {
    write_state_file("{ \"applied\": [ {\"symbol\": ");
    CorporateActionsAuditLog log(dir_.string());
    auto r = log.load();
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(r.value());
    EXPECT_DOUBLE_EQ(log.total_cumulative_dividend_income(), 0.0);
}

TEST_F(AuditLogPathsTest, SaveFailsWhenTheStateDirCannotBeCreated) {
    // A regular file where the state directory should be.
    const fs::path blocker = dir_ / "not_a_dir";
    std::ofstream(blocker) << "x";
    CorporateActionsAuditLog log((blocker / "state").string());
    log.record(adj("AAPL", "2026-01-02", CorpActionType::SPLIT, 4.0));
    EXPECT_FALSE(log.save());
}

TEST_F(AuditLogPathsTest, SaveFailsWhenTheTempFileCannotBeOpened) {
    fs::create_directories(dir_ / "applied_corp_actions.json.tmp");  // a directory, not a file
    CorporateActionsAuditLog log(dir_.string());
    log.record(adj("AAPL", "2026-01-02", CorpActionType::SPLIT, 4.0));
    EXPECT_FALSE(log.save());
    EXPECT_FALSE(fs::exists(dir_ / "applied_corp_actions.json"));
}

TEST_F(AuditLogPathsTest, FailedRenameRemovesTheTempFileAndKeepsTheOldState) {
    // The target is a non-empty directory, so the atomic rename fails.
    fs::create_directories(dir_ / "applied_corp_actions.json" / "occupied");
    CorporateActionsAuditLog log(dir_.string());
    log.record(adj("AAPL", "2026-01-02", CorpActionType::SPLIT, 4.0));
    EXPECT_FALSE(log.save());
    EXPECT_FALSE(fs::exists(dir_ / "applied_corp_actions.json.tmp"));
    EXPECT_TRUE(fs::is_directory(dir_ / "applied_corp_actions.json" / "occupied"));
}

// ---------------------------------------------------------------------------
// DB backing: what record() persists, and save()'s failure and retry
// ---------------------------------------------------------------------------

TEST_F(AuditLogPathsTest, RecordedRowsCarryRunDateRatioAndDividendCash) {
    auto log = db_log();
    ASSERT_TRUE(log.load().is_ok());
    log.set_run_date("2026-03-02");
    log.record(adj("AAPL", "2026-03-02", CorpActionType::SPLIT, 4.0, 400.0, 4.0));
    log.record(adj("KO", "2026-03-02", CorpActionType::DIVIDEND, 1.01, 50.0, 0.5));
    log.record(adj("XYZ", "2026-03-02", CorpActionType::TERMINATION, 0.0));
    ASSERT_TRUE(log.save());

    ASSERT_EQ(db_->rows.size(), 3u);
    const Row& split = db_->rows[0];
    EXPECT_EQ(split.action_type, CorporateActionsApplier::type_to_string(CorpActionType::SPLIT));
    EXPECT_EQ(split.run_date, "2026-03-02");
    EXPECT_TRUE(split.basis_ratio_known);
    EXPECT_DOUBLE_EQ(split.basis_ratio, 4.0);
    EXPECT_EQ(split.total_cash, 0.0);

    const Row& div = db_->rows[1];
    EXPECT_DOUBLE_EQ(div.qty_held, 50.0);
    EXPECT_DOUBLE_EQ(div.dividend_per_share, 0.5);
    EXPECT_DOUBLE_EQ(div.total_cash, 25.0);
    EXPECT_TRUE(div.basis_ratio_known);

    // A termination restates no basis: no ratio, so nothing can chain through it.
    const Row& term = db_->rows[2];
    EXPECT_FALSE(term.basis_ratio_known);

    EXPECT_DOUBLE_EQ(log.total_cumulative_dividend_income(), 25.0);
}

TEST_F(AuditLogPathsTest, FailedDbSaveKeepsThePendingRowsForTheRetry) {
    auto log = db_log();
    ASSERT_TRUE(log.load().is_ok());
    log.record(adj("AAPL", "2026-03-02", CorpActionType::SPLIT, 4.0));

    db_->fail_store = true;
    EXPECT_FALSE(log.save());
    EXPECT_TRUE(db_->rows.empty());

    db_->fail_store = false;
    EXPECT_TRUE(log.save());
    ASSERT_EQ(db_->rows.size(), 1u);

    // Nothing left pending: a third save makes no write.
    const int calls = db_->store_calls;
    EXPECT_TRUE(log.save());
    EXPECT_EQ(db_->store_calls, calls);
}

TEST_F(AuditLogPathsTest, BasisChainIsPerSymbolInExDateOrderAcrossLoadedAndNewRows) {
    Row old_split = row("AAPL", "2020-08-31", "SPLIT");
    old_split.basis_ratio = 4.0;
    old_split.basis_ratio_known = true;
    db_->rows = {old_split, row("MSFT", "2021-01-01", "SPLIT")};

    auto log = db_log();
    ASSERT_TRUE(log.load().is_ok());
    log.record(adj("AAPL", "2026-03-02", CorpActionType::DIVIDEND, 1.002, 10.0, 0.25));
    log.record(adj("AAPL", "2014-06-09", CorpActionType::SPLIT, 7.0));

    auto chain = log.basis_chain("AAPL");
    ASSERT_EQ(chain.size(), 3u);
    EXPECT_EQ(chain[0].ex_date, "2014-06-09");
    EXPECT_DOUBLE_EQ(chain[0].ratio, 7.0);
    EXPECT_EQ(chain[1].ex_date, "2020-08-31");
    EXPECT_DOUBLE_EQ(chain[1].ratio, 4.0);
    EXPECT_EQ(chain[2].ex_date, "2026-03-02");
    EXPECT_DOUBLE_EQ(chain[2].dividend_per_share, 0.25);
    for (const auto& ev : chain) {
        EXPECT_EQ(ev.symbol, "AAPL");
        EXPECT_TRUE(ev.ratio_known);
    }
    EXPECT_TRUE(log.basis_chain("TSLA").empty());
}

TEST_F(AuditLogPathsTest, AFileBackedLogKeepsNoBasisChain) {
    CorporateActionsAuditLog log(dir_.string());
    log.record(adj("AAPL", "2026-03-02", CorpActionType::SPLIT, 4.0));
    EXPECT_TRUE(log.is_applied("AAPL", "2026-03-02", CorpActionType::SPLIT));
    EXPECT_TRUE(log.basis_chain("AAPL").empty());
}

// ---------------------------------------------------------------------------
// The run-date guard's message
// ---------------------------------------------------------------------------

TEST_F(AuditLogPathsTest, StaleRowMessageListsTwentyRowsAndCountsTheRest) {
    for (int i = 0; i < 25; ++i) {
        db_->rows.push_back(row("S" + std::to_string(i), "2026-02-01", "SPLIT", "2026-03-05"));
    }
    db_->rows.push_back(row("OLD", "2026-02-01", "SPLIT", "2026-03-01"));  // earlier pass: fine
    auto log = db_log();
    log.set_run_date("2026-03-04");
    auto r = log.load();
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_ARGUMENT);
    const std::string msg = r.error()->what();
    EXPECT_NE(msg.find("25 corp-action dedup row(s)"), std::string::npos) << msg;
    EXPECT_NE(msg.find("S19 SPLIT ex 2026-02-01 written by the run of 2026-03-05"),
              std::string::npos);
    EXPECT_EQ(msg.find("S20 SPLIT"), std::string::npos);
    EXPECT_NE(msg.find("and 5 more"), std::string::npos);
    EXPECT_EQ(msg.find("OLD"), std::string::npos);
}

TEST_F(AuditLogPathsTest, ARowFromThisSameRunDateAlsoRefuses) {
    // "On or after": a row stamped with today's date came from another pass today.
    db_->rows = {row("AAPL", "2026-02-01", "SPLIT", "2026-03-04")};
    auto log = db_log();
    log.set_run_date("2026-03-04");
    EXPECT_TRUE(log.load().is_error());
}

// ---------------------------------------------------------------------------
// The rename bridge
// ---------------------------------------------------------------------------

TEST_F(AuditLogPathsTest, RenameChainsAreFollowedHopByHop) {
    // A -> B until 2020-01-01, B -> C until 2023-01-01. A 2019 event on A is
    // mirrored under B and C; a 2021 event on B under C only.
    db_->rows = {row("A", "2019-05-01", "SPLIT"), row("B", "2021-05-01", "SPLIT")};
    db_->aliases = {alias("A", "B", "2020-01-01"), alias("B", "C", "2023-01-01")};
    auto log = db_log();
    ASSERT_TRUE(log.load().is_ok());
    EXPECT_TRUE(log.is_applied("B", "2019-05-01", CorpActionType::SPLIT));
    EXPECT_TRUE(log.is_applied("C", "2019-05-01", CorpActionType::SPLIT));
    EXPECT_TRUE(log.is_applied("C", "2021-05-01", CorpActionType::SPLIT));
    EXPECT_FALSE(log.is_applied("A", "2021-05-01", CorpActionType::SPLIT));
}

TEST_F(AuditLogPathsTest, ACyclicAliasMapTerminates) {
    db_->rows = {row("A", "2019-05-01", "SPLIT")};
    db_->aliases = {alias("A", "B", "2020-01-01"), alias("B", "A", "2020-01-01")};
    auto log = db_log();
    ASSERT_TRUE(log.load().is_ok());
    EXPECT_TRUE(log.is_applied("A", "2019-05-01", CorpActionType::SPLIT));
    EXPECT_TRUE(log.is_applied("B", "2019-05-01", CorpActionType::SPLIT));
}

TEST_F(AuditLogPathsTest, IncompleteOrSelfAliasRowsAreIgnored) {
    db_->rows = {row("A", "2019-05-01", "SPLIT")};
    db_->aliases = {alias("A", "", "2020-01-01"), alias("", "B", "2020-01-01"),
                    alias("A", "C", ""), alias("A", "A", "2020-01-01")};
    auto log = db_log();
    ASSERT_TRUE(log.load().is_ok());
    EXPECT_TRUE(log.is_applied("A", "2019-05-01", CorpActionType::SPLIT));
    EXPECT_FALSE(log.is_applied("B", "2019-05-01", CorpActionType::SPLIT));
    EXPECT_FALSE(log.is_applied("C", "2019-05-01", CorpActionType::SPLIT));
}

TEST_F(AuditLogPathsTest, IncompleteOrSelfAliasRowsDoNotShadowAValidRename) {
    // A -> C until 2021-01-01 is the one valid rename. A row with no successor or a
    // self-map dated earlier must not stop the lookup before it reaches C.
    db_->rows = {row("A", "2019-05-01", "SPLIT")};
    db_->aliases = {alias("A", "", "2020-01-01"), alias("A", "A", "2020-06-01"),
                    alias("A", "C", "2021-01-01")};
    auto log = db_log();
    ASSERT_TRUE(log.load().is_ok());
    EXPECT_TRUE(log.is_applied("C", "2019-05-01", CorpActionType::SPLIT));
}

// ---------------------------------------------------------------------------
// The legacy-file import
// ---------------------------------------------------------------------------

TEST_F(AuditLogPathsTest, ReReadFailureAfterTheImportIsADatabaseError) {
    write_state_file(R"({"applied": [{"symbol": "AAPL", "ex_date": "2020-08-31", "action": "SPLIT"}]})");
    db_->fail_load_on_call = 2;  // the first read is empty; the re-read after import fails
    auto log = db_log();
    auto r = log.load();
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::DATABASE_ERROR);
    EXPECT_EQ(db_->rows.size(), 1u);  // the import itself was written
}

TEST_F(AuditLogPathsTest, AFailedImportWriteIsReportedAsAFirstRun) {
    // FIXME: the DB record is empty, the legacy file holds an applied split, and the
    // import write fails. migrate_state_file_to_db() returns false, which load() reads
    // as "nothing to import", so it reports a clean first run (ok, false) with an empty
    // applied set and the split is applied a second time. load() should fail closed
    // here, as it does when the dedup table cannot be read: return DATABASE_ERROR.
    write_state_file(R"({"applied": [{"symbol": "AAPL", "ex_date": "2020-08-31", "action": "SPLIT"}]})");
    db_->fail_store = true;
    auto log = db_log();
    auto r = log.load();
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(r.value());
    EXPECT_FALSE(log.is_applied("AAPL", "2020-08-31", CorpActionType::SPLIT));
    EXPECT_EQ(db_->store_calls, 1);
}

TEST_F(AuditLogPathsTest, AnEmptyLegacyFileImportsNothing) {
    write_state_file(R"({"applied": []})");
    auto log = db_log();
    auto r = log.load();
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(r.value());
    EXPECT_EQ(db_->store_calls, 0);
    EXPECT_EQ(db_->load_calls, 1);
}
