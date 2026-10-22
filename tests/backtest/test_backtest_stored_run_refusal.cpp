// tests/backtest/test_backtest_stored_run_refusal.cpp
//
// T-8D-2 R49 (D2-BT-2): a backtest whose run id is already stored is refused before its first
// cycle (include/trade_ngin/backtest/stored_run_refusal.hpp). The run id is the strategy ids and
// the window's end instant, so two runs of the same strategies to the same end date carry one id.
//
// Three layers, all executed:
//   * the statements and the refusal's text (no database);
//   * the count and the decision on a database that answers the two counts (StoredRunDb);
//   * the count and the decision against the real backtest.results and backtest.final_positions
//     (with TRADE_NGIN_TEST_DSN, on probe ids deleted before and after).
// The whole run (run_portfolio refusing, storing nothing) is in test_backtest_statistics_grid.cpp.

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cost_basis_test_helpers.hpp"
#include "trade_ngin/backtest/stored_run_refusal.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;
using trade_ngin::testing::cost_basis::StoredRunDb;

namespace {

constexpr const char* kRun = "T8B_R49_PROBE_20261007_040000_000";

Timestamp at_utc(int y, int m, int d, int hh) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = hh;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

}  // namespace

// ===== the statements and the text =====

TEST(StoredRunRefusal, TheTwoCountsNameTheRunIdAsALiteral) {
    EXPECT_EQ(stored_run_results_query("TREND_FOLLOWING_20261007_040000_000"),
              "SELECT COUNT(*) FROM backtest.results WHERE run_id = "
              "'TREND_FOLLOWING_20261007_040000_000'");
    EXPECT_EQ(stored_run_positions_query("TREND_FOLLOWING_20261007_040000_000"),
              "SELECT COUNT(*) FROM backtest.final_positions WHERE run_id = "
              "'TREND_FOLLOWING_20261007_040000_000'");
    // A quote in a strategy id cannot end the literal.
    EXPECT_EQ(stored_run_results_query("O'BRIEN_1"),
              "SELECT COUNT(*) FROM backtest.results WHERE run_id = 'O''BRIEN_1'");
}

TEST(StoredRunRefusal, TheTextIsEmptyWithNoStoredRowAndNamesTheIdAndBothCountsOtherwise) {
    EXPECT_EQ(stored_run_refusal_text("RUN_A", {0, 0}), "");
    const std::string results_only = stored_run_refusal_text("RUN_A", {1, 0});
    EXPECT_EQ(results_only.rfind("BACKTEST REFUSED: run_id RUN_A is already stored "
                                 "(backtest.results rows: 1, backtest.final_positions rows: 0).",
                                 0),
              0u)
        << results_only;
    EXPECT_NE(results_only.find("Nothing was stored."), std::string::npos);
    const std::string positions_only = stored_run_refusal_text("RUN_A", {0, 10209});
    EXPECT_NE(positions_only.find("backtest.results rows: 0, backtest.final_positions rows: 10209"),
              std::string::npos)
        << positions_only;
}

// ===== the count and the decision on a database that answers the counts =====

TEST(StoredRunRefusal, TheCountsAreReadOncePerTableAndComeBackAsAsked) {
    StoredRunDb db("mock://testdb");
    ASSERT_TRUE(db.connect().is_ok());
    db.results_rows = 1;
    db.final_positions_rows = 10209;
    auto rows = count_stored_run_rows(db, "RUN_A");
    ASSERT_TRUE(rows.is_ok()) << rows.error()->what();
    EXPECT_EQ(rows.value().results, 1);
    EXPECT_EQ(rows.value().final_positions, 10209);
    ASSERT_EQ(db.count_queries.size(), 2u);
    EXPECT_EQ(db.count_queries[0], stored_run_results_query("RUN_A"));
    EXPECT_EQ(db.count_queries[1], stored_run_positions_query("RUN_A"));
}

//   | results rows | final_positions rows | decision                      |
//   | 0            | 0                    | ok                            |
//   | 1            | 0                    | refused (INVALID_ARGUMENT)    |
//   | 0            | 2                    | refused                       |
//   | unreadable   | -                    | ok, `unchecked` says why      |
TEST(StoredRunRefusal, ARunIsRefusedWhenEitherTableHoldsItsIdAndOnlyThen) {
    StoredRunDb db("mock://testdb");
    ASSERT_TRUE(db.connect().is_ok());

    std::string unchecked;
    EXPECT_TRUE(refuse_stored_run(db, "RUN_A", &unchecked).is_ok());
    EXPECT_TRUE(unchecked.empty());

    db.results_rows = 1;
    auto by_results = refuse_stored_run(db, "RUN_A", &unchecked);
    ASSERT_TRUE(by_results.is_error());
    EXPECT_EQ(by_results.error()->code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_NE(std::string(by_results.error()->what()).find("BACKTEST REFUSED: run_id RUN_A"),
              std::string::npos)
        << by_results.error()->what();
    EXPECT_TRUE(unchecked.empty());

    db.results_rows = 0;
    db.final_positions_rows = 2;
    auto by_positions = refuse_stored_run(db, "RUN_A", &unchecked);
    ASSERT_TRUE(by_positions.is_error());
    EXPECT_NE(std::string(by_positions.error()->what()).find("backtest.final_positions rows: 2"),
              std::string::npos);

    db.counts_fail = true;
    EXPECT_TRUE(refuse_stored_run(db, "RUN_A", &unchecked).is_ok())
        << "a count that cannot be read is not a refusal";
    EXPECT_NE(unchecked.find("backtest.results rows"), std::string::npos) << unchecked;
    EXPECT_TRUE(refuse_stored_run(db, "RUN_A").is_ok()) << "and the reason is optional";
}

// ===== against the real tables =====

class StoredRunRefusalDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const bool require_db = [] {
            const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
            return v && std::string(v) == "1";
        }();
        const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
        conn_ = dsn ? std::string(dsn) : std::string();
        if (conn_.empty()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set, so the "
                          "refusal of a stored run id goes unverified against the real tables";
            }
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set; no database to exercise";
        }
        db_ = std::make_shared<PostgresDatabase>(conn_);
        auto connected = db_->connect();
        if (connected.is_error() || !db_->is_connected()) {
            if (require_db) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable";
            GTEST_SKIP() << "database unreachable; skipping";
        }
        purge();
    }

    void TearDown() override {
        if (!conn_.empty()) purge();
        if (db_ && db_->is_connected()) db_->disconnect();
    }

    void purge() {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        w.exec("DELETE FROM backtest.final_positions WHERE run_id LIKE 'T8B\\_R49\\_PROBE%'");
        w.exec("DELETE FROM backtest.results WHERE run_id LIKE 'T8B\\_R49\\_PROBE%'");
        w.commit();
    }

    void store_results_row(const std::string& run_id) {
        const std::unordered_map<std::string, double> metrics = {{"total_return", 0.25},
                                                                 {"total_trades", 3.0}};
        auto stored = db_->store_backtest_summary(run_id, at_utc(2023, 10, 7, 4),
                                                  at_utc(2026, 10, 7, 4), metrics,
                                                  "T8B_R49_PROBE_PORTFOLIO", "backtest.results");
        ASSERT_TRUE(stored.is_ok()) << stored.error()->what();
    }

    // One daily position row of a sleeve, written as the coordinator writes it
    // ("<run id>|<strategy id>" handed in; the writer stores the two apart).
    void store_position_row(const std::string& run_id, const std::string& strategy_id) {
        Position p;
        p.symbol = "ES.v.0";
        p.quantity = Decimal(2.0);
        p.average_price = Decimal(5000.0);
        p.last_update = at_utc(2026, 10, 6, 10);
        auto stored = db_->store_backtest_positions({p}, run_id + "|" + strategy_id,
                                                    "T8B_R49_PROBE_PORTFOLIO",
                                                    "backtest.final_positions");
        ASSERT_TRUE(stored.is_ok()) << stored.error()->what();
    }

    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

TEST_F(StoredRunRefusalDbTest, ARunIdWithNoStoredRowIsNotRefused) {
    auto rows = count_stored_run_rows(*db_, kRun);
    ASSERT_TRUE(rows.is_ok()) << rows.error()->what();
    EXPECT_EQ(rows.value().results, 0);
    EXPECT_EQ(rows.value().final_positions, 0);
    std::string unchecked;
    EXPECT_TRUE(refuse_stored_run(*db_, kRun, &unchecked).is_ok());
    EXPECT_TRUE(unchecked.empty()) << unchecked;
}

TEST_F(StoredRunRefusalDbTest, AStoredResultsRowRefusesTheRunId) {
    store_results_row(kRun);
    auto rows = count_stored_run_rows(*db_, kRun);
    ASSERT_TRUE(rows.is_ok()) << rows.error()->what();
    EXPECT_EQ(rows.value().results, 1);
    EXPECT_EQ(rows.value().final_positions, 0);
    auto refused = refuse_stored_run(*db_, kRun);
    ASSERT_TRUE(refused.is_error());
    EXPECT_NE(std::string(refused.error()->what())
                  .find(std::string("BACKTEST REFUSED: run_id ") + kRun),
              std::string::npos)
        << refused.error()->what();
}

// The daily positions are stored through the run, before the results row exists: a run that
// died half-way leaves positions and no results row, and its id is refused all the same.
TEST_F(StoredRunRefusalDbTest, StoredPositionsOfAnySleeveRefuseTheRunIdWithNoResultsRow) {
    store_position_row(kRun, "SLEEVE_A");
    store_position_row(kRun, "SLEEVE_B");
    auto rows = count_stored_run_rows(*db_, kRun);
    ASSERT_TRUE(rows.is_ok()) << rows.error()->what();
    EXPECT_EQ(rows.value().results, 0);
    EXPECT_EQ(rows.value().final_positions, 2) << "one row a sleeve";
    EXPECT_TRUE(refuse_stored_run(*db_, kRun).is_error());
}

// Another run's rows are not this run's: an id that only starts with the same text, and the
// same strategies to another end date.
TEST_F(StoredRunRefusalDbTest, AnotherRunsRowsDoNotRefuseThisRunId) {
    store_results_row(std::string(kRun) + "_X");
    store_position_row(std::string(kRun) + "_X", "SLEEVE_A");
    store_results_row("T8B_R49_PROBE_20261006_040000_000");
    auto rows = count_stored_run_rows(*db_, kRun);
    ASSERT_TRUE(rows.is_ok()) << rows.error()->what();
    EXPECT_EQ(rows.value().results, 0);
    EXPECT_EQ(rows.value().final_positions, 0);
    EXPECT_TRUE(refuse_stored_run(*db_, kRun).is_ok());
}
