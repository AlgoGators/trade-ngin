// STORE-positions-catchall-retry: store_positions must return the FIRST error.
//
// The INSERT in store_positions_in used to be wrapped in a catch that read every
// failure as one specific cause -- "strategy_id column may not exist, trying
// without it" -- and retried on the SAME pqxx::work with strategy_id = '',
// strategy_name = '' and portfolio_id = 'BASE_PORTFOLIO'. Postgres refuses every
// command in a transaction after the first error, so the retry always threw
// "current transaction is aborted, commands ignored until end of transaction
// block", and THAT is the message that reached the caller. The real cause
// survived only in a WARN line that misnamed it.
//
// These tests pin the behaviour that replaced it, on the two failures actually
// reachable from a runner:
//
//   1. an identifier too long for the column -> the caller is told "value too
//      long", not "current transaction is aborted" (on a TEMP table of width 50,
//      so it runs whatever width the real column has);
//   2. a duplicate key inside one batch -> the caller is told "duplicate key",
//      not "current transaction is aborted";
//
// and, in both, that nothing was written -- neither under the caller's portfolio
// nor under the 'BASE_PORTFOLIO' / empty-strategy_id shape the retry would have
// used. The last assertion is the one that would catch a re-introduction of the
// fallback even if its error text changed.
//
// Reachability gate matches tests/data/test_strategy_id_length_db.cpp: the test
// runs only against TRADE_NGIN_TEST_DSN and skips when it is unset, unless
// TRADE_NGIN_REQUIRE_DB=1 demands it.

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {

constexpr const char* kPortfolio = "T2_STORE_ERR_PROBE_PORTFOLIO";
// The overlong-id pin's own table: a TEMP copy of trading.positions, strategy_id varchar(50).
constexpr const char* kNarrowTable = "t2_store_err_probe_narrow_positions";
constexpr const char* kStrategyName = "TREND_FOLLOWING";
constexpr const char* kGoodId = "LIVE_TREND_FOLLOWING";

std::string discover_connection_string() {
    const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
    if (dsn && *dsn) return std::string(dsn);
    return {};
}

Timestamp date_at(int y, int m, int d) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = 5;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

Position a_position(const std::string& symbol = "ES.v.0") {
    Position p;
    p.symbol = symbol;
    p.quantity = 1.0;
    p.average_price = Price(5000.0);
    p.unrealized_pnl = 0.0;
    p.realized_pnl = 0.0;
    p.last_update = date_at(2026, 5, 4);
    return p;
}

bool mentions(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

class StorePositionsErrorTest : public ::testing::Test {
protected:
    void SetUp() override {
        const bool require_db = [] {
            const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
            return v && std::string(v) == "1";
        }();
        conn_ = discover_connection_string();
        if (conn_.empty()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set, so the "
                          "store_positions error path goes unverified";
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

    // Both the probe portfolio and the shape the deleted retry would have written.
    void purge() {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        w.exec("DELETE FROM trading.positions WHERE portfolio_id = " + w.quote(kPortfolio));
        w.exec("DELETE FROM trading.positions WHERE portfolio_id = 'BASE_PORTFOLIO' "
               "AND strategy_id = '' AND date = DATE '2026-05-04'");
        w.commit();
    }

    int column_width(const char* table) {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        auto r = w.exec("SELECT character_maximum_length FROM information_schema.columns "
                        "WHERE table_schema='trading' AND column_name='strategy_id' "
                        "AND table_name=" + w.quote(table));
        return r.empty() || r[0][0].is_null() ? -1 : r[0][0].as<int>();
    }

    int rows_for(const char* portfolio) {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        auto r = w.exec("SELECT count(*) FROM trading.positions WHERE portfolio_id = " +
                        w.quote(portfolio));
        return r[0][0].as<int>();
    }

    // The row shape the deleted fallback wrote: no strategy attribution at all.
    int unattributed_rows() {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        auto r = w.exec("SELECT count(*) FROM trading.positions "
                        "WHERE strategy_id = '' AND strategy_name = '' "
                        "AND date = DATE '2026-05-04'");
        return r[0][0].as<int>();
    }

    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

// 1. Too long for the column: the caller is told what the server said.
TEST_F(StorePositionsErrorTest, AnOverlongIdReportsValueTooLongNotAnAbortedTransaction) {
    // Migration 012 widened trading.positions.strategy_id to validate_strategy_id's cap of 100,
    // so on the real table no id that passes the validator is too long any more. The error text
    // is still the store's to carry, so the pin runs on a table of its own: a TEMP copy of
    // trading.positions on the store's own session with strategy_id at the old width of 50. It
    // is visible to that session only and goes with it, so nothing is left in the database.
    const int real_width = column_width("positions");
    ASSERT_GT(real_width, 0) << "trading.positions.strategy_id has no character_maximum_length";
    EXPECT_LE(real_width, 100)
        << "trading.positions.strategy_id is wider than validate_strategy_id's cap; "
           "the validator and the schema have drifted apart again, the other way";

    constexpr int kWidth = 50;
    const std::string table = std::string("pg_temp.") + kNarrowTable;
    struct DropOnExit {
        PostgresDatabase& db;
        ~DropOnExit() {
            db.execute_direct_query(std::string("DROP TABLE IF EXISTS pg_temp.") + kNarrowTable);
        }
    } drop_on_exit{*db_};
    auto made = db_->execute_direct_query("CREATE TEMP TABLE " + std::string(kNarrowTable) +
                                          " (LIKE trading.positions INCLUDING DEFAULTS)");
    ASSERT_TRUE(made.is_ok()) << made.error()->what();
    auto narrowed = db_->execute_direct_query("ALTER TABLE " + table +
                                              " ALTER COLUMN strategy_id TYPE varchar(" +
                                              std::to_string(kWidth) + ")");
    ASSERT_TRUE(narrowed.is_ok()) << narrowed.error()->what();

    // One character past the column, built from a valid identifier alphabet so
    // validate_strategy_id lets it reach the server.
    std::string too_long = "LIVE_";
    while (static_cast<int>(too_long.size()) <= kWidth) too_long += "X";
    ASSERT_EQ(static_cast<int>(too_long.size()), kWidth + 1);

    auto r = db_->store_positions({a_position()}, too_long, kStrategyName, kPortfolio, table);
    ASSERT_TRUE(r.is_error()) << "a " << too_long.size() << "-character id was accepted by a "
                              << "varchar(" << kWidth << ") column";
    EXPECT_EQ(r.error()->code(), ErrorCode::DATABASE_ERROR);

    const std::string what = r.error()->what();
    EXPECT_TRUE(mentions(what, "value too long"))
        << "the caller should be told the real cause; got: " << what;
    EXPECT_FALSE(mentions(what, "current transaction is aborted"))
        << "the retry fallback is back: the first error has been replaced by the "
           "aborted-transaction message. " << what;

    // Nothing was written: not to the narrow table (a DELETE reports the rows it held), and not
    // to the real one under either shape.
    auto held = db_->execute_direct_query("DELETE FROM " + table);
    ASSERT_TRUE(held.is_ok()) << held.error()->what();
    EXPECT_EQ(held.value(), 0u) << "the failed transaction committed something";
    EXPECT_EQ(rows_for(kPortfolio), 0);
    EXPECT_EQ(unattributed_rows(), 0)
        << "rows were written with no strategy attribution -- the deleted fallback's shape";
}

// 2. Duplicate key inside one batch: the caller is told what the server said.
TEST_F(StorePositionsErrorTest, ADuplicateKeyReportsDuplicateKeyNotAnAbortedTransaction) {
    // The same symbol twice on the same date under the same identifiers violates
    // positions_pkey (portfolio_id, strategy_id, strategy_name, date, symbol,
    // portfolio_type). The scoped DELETE that precedes the INSERT clears any
    // earlier rows, so the collision is inside the batch and does not depend on
    // what the table already held.
    std::vector<Position> two_of_the_same{a_position(), a_position()};

    auto r = db_->store_positions(two_of_the_same, kGoodId, kStrategyName, kPortfolio,
                                  "trading.positions");
    ASSERT_TRUE(r.is_error()) << "two rows with the same primary key were both accepted";
    EXPECT_EQ(r.error()->code(), ErrorCode::DATABASE_ERROR);

    const std::string what = r.error()->what();
    EXPECT_TRUE(mentions(what, "duplicate key"))
        << "the caller should be told the real cause; got: " << what;
    EXPECT_FALSE(mentions(what, "current transaction is aborted"))
        << "the retry fallback is back: the first error has been replaced by the "
           "aborted-transaction message. " << what;

    EXPECT_EQ(rows_for(kPortfolio), 0) << "the failed transaction committed something";
    EXPECT_EQ(unattributed_rows(), 0)
        << "rows were written with no strategy attribution -- the deleted fallback's shape";
}

// 3. The healthy path is untouched: this is what makes the deletion class A.
TEST_F(StorePositionsErrorTest, AnOrdinaryBatchStillStoresEveryRow) {
    std::vector<Position> book{a_position("ES.v.0"), a_position("CL.v.0"), a_position("GC.v.0")};

    auto r = db_->store_positions(book, kGoodId, kStrategyName, kPortfolio, "trading.positions");
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(rows_for(kPortfolio), 3);

    // And storing the same day again replaces rather than duplicates, which is
    // the scoped DELETE the error path must never widen.
    auto again = db_->store_positions(book, kGoodId, kStrategyName, kPortfolio,
                                      "trading.positions");
    ASSERT_TRUE(again.is_ok()) << again.error()->what();
    EXPECT_EQ(rows_for(kPortfolio), 3);
}
