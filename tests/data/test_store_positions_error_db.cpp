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
//      long", not "current transaction is aborted";
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

// 62 characters: exactly what a third enabled trend strategy joins to, and wider
// than trading.positions.strategy_id at varchar(50). If migration 010 has been
// applied the column is varchar(100) and this id fits, so the test picks its
// probe from the column's ACTUAL width rather than assuming either state.
constexpr const char* kPortfolio = "T2_STORE_ERR_PROBE_PORTFOLIO";
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
    const int width = column_width("positions");
    ASSERT_GT(width, 0) << "trading.positions.strategy_id has no character_maximum_length";

    // One character past whatever the column currently is, built from a valid
    // identifier alphabet so validate_strategy_id lets it reach the server. The
    // validator's own cap is 100 (T-1 item 11), so this only works while the
    // column is narrower than that -- which is the point of the guard below.
    ASSERT_LT(width, 100) << "trading.positions.strategy_id is now at least as wide as the "
                             "validator's cap, so no id can reach the server too long. "
                             "Rewrite this case against a different column, or drop it and "
                             "keep the duplicate-key case.";
    std::string too_long = "LIVE_";
    while (static_cast<int>(too_long.size()) <= width) too_long += "X";
    ASSERT_EQ(static_cast<int>(too_long.size()), width + 1);

    auto r = db_->store_positions({a_position()}, too_long, kStrategyName, kPortfolio,
                                  "trading.positions");
    ASSERT_TRUE(r.is_error()) << "a " << too_long.size() << "-character id was accepted by a "
                              << "varchar(" << width << ") column";
    EXPECT_EQ(r.error()->code(), ErrorCode::DATABASE_ERROR);

    const std::string what = r.error()->what();
    EXPECT_TRUE(mentions(what, "value too long"))
        << "the caller should be told the real cause; got: " << what;
    EXPECT_FALSE(mentions(what, "current transaction is aborted"))
        << "the retry fallback is back: the first error has been replaced by the "
           "aborted-transaction message. " << what;

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
