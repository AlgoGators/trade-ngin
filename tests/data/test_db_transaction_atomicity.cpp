// Atomicity of composed writes.
//
// The live equity path writes corp-action-adjusted positions and then the dedup
// rows that stop those events being applied again. Before DbTransaction these
// were two independent transactions, so a failure between them left positions
// adjusted with no dedup record -- and the next run re-applied every event,
// re-multiplying quantities and re-rescaling cost basis. These tests pin the
// unit-of-work contract that removes that window.
//
// Transaction semantics cannot be faked: pqxx rollback is only observable
// against a real server. The DB-backed cases therefore skip when no database is
// reachable, and write only to a scratch table plus scratch identifiers, never
// to production rows.

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "../core/test_base.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {

constexpr const char* kScratchTable = "trading.dbtxn_atomicity_probe";
constexpr const char* kScratchPortfolio = "DBTXN_PROBE_PORTFOLIO";
constexpr const char* kScratchStrategyId = "DBTXN_PROBE_STRATEGY";
constexpr const char* kScratchStrategyName = "DBTXN_PROBE_NAME";

/// Connection string from config/defaults.json, empty when it cannot be found.
std::string discover_connection_string() {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        fs::path candidate = dir / "config" / "defaults.json";
        if (fs::exists(candidate)) {
            try {
                std::ifstream in(candidate);
                nlohmann::json j = nlohmann::json::parse(in);
                const auto& d = j.at("database");
                return "postgresql://" + d.at("username").get<std::string>() + ":" +
                       d.at("password").get<std::string>() + "@" +
                       d.at("host").get<std::string>() + ":" + d.at("port").get<std::string>() +
                       "/" + d.at("name").get<std::string>();
            } catch (const std::exception&) {
                return {};
            }
        }
        dir = dir.parent_path();
    }
    return {};
}

Position make_position(const std::string& symbol, double qty, double avg_price) {
    Position p;
    p.symbol = symbol;
    p.quantity = qty;
    p.average_price = avg_price;
    p.unrealized_pnl = 0.0;
    p.realized_pnl = 0.0;
    p.last_update = std::chrono::system_clock::now();
    return p;
}

PostgresDatabase::AppliedCorpActionRow make_applied_row(const std::string& symbol,
                                                        const std::string& ex_date) {
    PostgresDatabase::AppliedCorpActionRow r;
    r.symbol = symbol;
    r.action_type = "DIVIDEND";
    r.ex_date = ex_date;
    r.qty_held = 10.0;
    r.dividend_per_share = 0.27;
    r.total_cash = 2.70;
    return r;
}

class DbTransactionAtomicityTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string conn = discover_connection_string();
        if (conn.empty()) {
            GTEST_SKIP() << "config/defaults.json not reachable; no database to exercise";
        }
        db_ = std::make_shared<PostgresDatabase>(conn);
        auto connected = db_->connect();
        if (connected.is_error() || !db_->is_connected()) {
            GTEST_SKIP() << "database unreachable; transaction semantics need a real server";
        }
        ASSERT_TRUE(create_scratch_table());
        clear_scratch_rows();
    }

    void TearDown() override {
        if (db_ && db_->is_connected()) {
            drop_scratch_table();
            clear_dedup_rows();
            db_->disconnect();
        }
    }

    // Raw DDL/queries go through a private connection so the tests never depend
    // on the class under test for their own setup and verification.
    bool run_raw(const std::string& sql) {
        try {
            pqxx::connection c(discover_connection_string());
            pqxx::work w(c);
            w.exec(sql);
            w.commit();
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    int scalar(const std::string& sql) {
        try {
            pqxx::connection c(discover_connection_string());
            pqxx::work w(c);
            auto r = w.exec(sql);
            w.commit();
            if (r.empty()) return -1;
            return r[0][0].as<int>();
        } catch (const std::exception&) {
            return -1;
        }
    }

    bool create_scratch_table() {
        return run_raw(std::string("CREATE TABLE IF NOT EXISTS ") + kScratchTable +
                       " (symbol text, quantity double precision, average_price double precision,"
                       "  daily_unrealized_pnl double precision, daily_realized_pnl double precision,"
                       "  last_update timestamptz, updated_at timestamptz, strategy_id text,"
                       "  strategy_name text, date date, portfolio_id text)");
    }

    void drop_scratch_table() { run_raw(std::string("DROP TABLE IF EXISTS ") + kScratchTable); }

    void clear_scratch_rows() { run_raw(std::string("DELETE FROM ") + kScratchTable); }

    void clear_dedup_rows() {
        run_raw(std::string("DELETE FROM trading.corp_action_applied WHERE portfolio_id = '") +
                kScratchPortfolio + "'");
    }

    int scratch_row_count() {
        return scalar(std::string("SELECT count(*) FROM ") + kScratchTable);
    }

    int dedup_row_count() {
        return scalar(std::string("SELECT count(*) FROM trading.corp_action_applied "
                                 "WHERE portfolio_id = '") +
                      kScratchPortfolio + "'");
    }

    std::shared_ptr<PostgresDatabase> db_;
};

// The regression this whole change exists for: the dedup write fails after the
// position write has already run. Before the unit of work the positions were
// committed and the dedup row was not, so the next run re-applied the events.
TEST_F(DbTransactionAtomicityTest, DedupFailureRollsBackTheAdjustedPositions) {
    auto unit = db_->begin_unit_of_work();
    ASSERT_TRUE(unit.is_ok()) << unit.error()->what();
    DbTransaction& txn = *unit.value();

    std::vector<Position> positions{make_position("AAPL", 10.0, 100.0)};
    auto stored = db_->store_positions(txn, positions, kScratchStrategyId, kScratchStrategyName,
                                       kScratchPortfolio, kScratchTable);
    ASSERT_TRUE(stored.is_ok()) << stored.error()->what();

    // Invalid ex_date makes the $6::date cast fail server-side -- a real write
    // failure in the second half of the unit, not a simulated one.
    std::vector<PostgresDatabase::AppliedCorpActionRow> rows{
        make_applied_row("AAPL", "not-a-date")};
    auto dedup = db_->store_applied_corp_actions(txn, kScratchPortfolio, kScratchStrategyId,
                                                 kScratchStrategyName, rows);
    EXPECT_TRUE(dedup.is_error()) << "an invalid ex_date must fail the dedup write";

    // Caller does not commit; the scope rolls back on destruction.
    unit = db_->begin_unit_of_work();  // drops the failed scope

    EXPECT_EQ(scratch_row_count(), 0)
        << "positions were committed even though the dedup write failed -- the next "
           "run would re-apply every event";
    EXPECT_EQ(dedup_row_count(), 0);
}

TEST_F(DbTransactionAtomicityTest, CommittedUnitPersistsBothWrites) {
    {
        auto unit = db_->begin_unit_of_work();
        ASSERT_TRUE(unit.is_ok()) << unit.error()->what();
        DbTransaction& txn = *unit.value();

        std::vector<Position> positions{make_position("MSFT", 5.0, 200.0)};
        ASSERT_TRUE(db_->store_positions(txn, positions, kScratchStrategyId, kScratchStrategyName,
                                         kScratchPortfolio, kScratchTable)
                        .is_ok());

        std::vector<PostgresDatabase::AppliedCorpActionRow> rows{
            make_applied_row("MSFT", "2026-08-20")};
        ASSERT_TRUE(db_->store_applied_corp_actions(txn, kScratchPortfolio, kScratchStrategyId,
                                                    kScratchStrategyName, rows)
                        .is_ok());

        auto committed = txn.commit();
        ASSERT_TRUE(committed.is_ok()) << committed.error()->what();
        EXPECT_TRUE(txn.committed());
    }

    EXPECT_EQ(scratch_row_count(), 1);
    EXPECT_EQ(dedup_row_count(), 1);
}

TEST_F(DbTransactionAtomicityTest, AbandonedUnitOfWorkRollsBackOnDestruction) {
    {
        auto unit = db_->begin_unit_of_work();
        ASSERT_TRUE(unit.is_ok()) << unit.error()->what();
        DbTransaction& txn = *unit.value();

        std::vector<Position> positions{make_position("NVDA", 3.0, 50.0)};
        ASSERT_TRUE(db_->store_positions(txn, positions, kScratchStrategyId, kScratchStrategyName,
                                         kScratchPortfolio, kScratchTable)
                        .is_ok());
        EXPECT_FALSE(txn.committed());
        // scope ends without commit
    }

    EXPECT_EQ(scratch_row_count(), 0) << "an abandoned unit of work must leave nothing behind";
}

// The single-write signature every existing caller uses -- futures included --
// must still own and commit its own transaction.
TEST_F(DbTransactionAtomicityTest, SingleWriteOverloadStillCommitsOnItsOwn) {
    std::vector<Position> positions{make_position("TSLA", 7.0, 150.0)};
    auto stored = db_->store_positions(positions, kScratchStrategyId, kScratchStrategyName,
                                       kScratchPortfolio, kScratchTable);
    ASSERT_TRUE(stored.is_ok()) << stored.error()->what();

    EXPECT_EQ(scratch_row_count(), 1)
        << "the unchanged overload must commit without the caller doing anything";
}

TEST_F(DbTransactionAtomicityTest, CommitTwiceIsRejectedRatherThanRepeated) {
    auto unit = db_->begin_unit_of_work();
    ASSERT_TRUE(unit.is_ok());
    DbTransaction& txn = *unit.value();

    std::vector<Position> positions{make_position("AMZN", 2.0, 90.0)};
    ASSERT_TRUE(db_->store_positions(txn, positions, kScratchStrategyId, kScratchStrategyName,
                                     kScratchPortfolio, kScratchTable)
                    .is_ok());
    ASSERT_TRUE(txn.commit().is_ok());
    EXPECT_TRUE(txn.commit().is_error()) << "a second commit must be refused, not repeated";
}

}  // namespace
