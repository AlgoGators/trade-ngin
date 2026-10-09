// E2-F36 / REG-F9: the joined futures strategy id and the 50-character cap.
//
// The futures runners store a JOINED strategy id -- "LIVE_" followed by every
// enabled trend strategy's name (live_portfolio_conservative.cpp, around the
// combined_strategy_id construction). With two strategies that is 41 characters
// and everything works. A THIRD gives 62:
//
//   LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST_TREND_FOLLOWING_SLOW
//
// validate_strategy_id rejected anything over 50, so enabling a third strategy
// would have made store_positions fail on a value the operator never typed.
//
// These tests cover BOTH halves of what was done, including the half that is NOT
// finished, because a test that only showed the cap raised would imply a third
// strategy now works. It does not.
//
//   1. the validator accepts the 62-character id (the cap raise);
//   2. store_executions now runs the same identifier checks store_positions has
//      always run (the asymmetry half);
//   3. the 62-character id is now STORED, because migration 012 widened
//      trading.positions / live_results / signals .strategy_id from varchar(50)
//      to varchar(100). This half was outstanding when the file was written and
//      the test asserted the failure; it now asserts the success, and it fails
//      on any database where 012 has not been applied, which is the tripwire that
//      matters from here on.
//
// Reachability gate matches tests/data/test_delete_stale_executions_scope_db.cpp.

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

// Exactly what a third enabled trend strategy produces.
constexpr const char* kJoinedId =
    "LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST_TREND_FOLLOWING_SLOW";
constexpr const char* kPortfolio = "T1_IDLEN_PROBE_PORTFOLIO";
constexpr const char* kStrategyName = "TREND_FOLLOWING";

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

Position a_position() {
    Position p;
    p.symbol = "ES.v.0";
    p.quantity = 1.0;
    p.average_price = Price(5000.0);
    p.unrealized_pnl = 0.0;
    p.realized_pnl = 0.0;
    p.last_update = date_at(2026, 5, 4);
    return p;
}

ExecutionReport an_execution() {
    ExecutionReport e;
    e.symbol = "ES.v.0";
    e.order_id = "DAILY_ES.v.0_20260504";
    e.exec_id = "T1_IDLEN_PROBE_EXEC";
    e.side = Side::BUY;
    e.filled_quantity = Quantity(1.0);
    e.fill_price = Price(5000.0);
    e.fill_time = date_at(2026, 5, 4);
    e.commissions_fees = Decimal(1.0);
    e.implicit_price_impact = Decimal(0.0);
    e.slippage_market_impact = Decimal(0.0);
    e.total_transaction_costs = Decimal(1.0);
    e.is_partial = false;
    return e;
}

}  // namespace

class StrategyIdLengthTest : public ::testing::Test {
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
                          "E2-F36 identifier length behaviour goes unverified";
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
        for (const char* t : {"positions", "executions"}) {
            w.exec("DELETE FROM trading." + std::string(t) + " WHERE portfolio_id = " +
                   w.quote(kPortfolio));
        }
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

    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

// 1. The cap raise: a 62-character joined id is no longer refused by the validator.
//
// The store_executions half of this cannot carry the claim on its own. Before
// the fix store_executions ran NO identifier validation, so a 62-character id
// went through it either way and the assertion would have passed vacuously. The
// load-bearing assertion is the second one: store_positions DOES validate, so
// the error code it returns says which layer refused the id --
// INVALID_ARGUMENT is the validator (the old 50 cap), DATABASE_ERROR is the
// server (the varchar(50) column, which is the half still outstanding).
TEST_F(StrategyIdLengthTest, TheJoinedThreeStrategyIdPassesTheValidator) {
    ASSERT_EQ(std::string(kJoinedId).size(), 62u);

    // The subject of THIS test is the validator, not the schema, so it must not turn a
    // missing migration into a failure about the cap. Ask the column first; the schema
    // half is asserted on its own below, where a missing 012 is the point.
    if (column_width("positions") < 62) {
        GTEST_SKIP() << "trading.positions.strategy_id is varchar(" << column_width("positions")
                     << "), so migration 012_strategy_id_width.sql has not been applied to this "
                        "database and the id cannot reach the server whatever the validator says";
    }

    // The validator does not refuse it, and since migration 012 neither does the
    // column: the id is stored whole.
    auto p = db_->store_positions({a_position()}, kJoinedId, kStrategyName, kPortfolio,
                                  "trading.positions");
    ASSERT_TRUE(p.is_ok())
        << "the 62-character joined id was refused with the column wide enough to hold it, so "
           "validate_strategy_id's cap has regressed: " << p.error()->what();

    // And where the column is wide enough (executions is varchar(100)), it is
    // stored intact.
    auto r = db_->store_executions({an_execution()}, kJoinedId, kStrategyName, kPortfolio,
                                   "trading.executions");
    ASSERT_TRUE(r.is_ok()) << "a 62-character joined id was refused: " << r.error()->what();

    pqxx::connection c(conn_);
    pqxx::work w(c);
    auto rows = w.exec("SELECT strategy_id FROM trading.executions WHERE portfolio_id = " +
                       w.quote(kPortfolio));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].as<std::string>(), kJoinedId);
}

// 2. The asymmetry: store_executions now validates identifiers as store_positions does.
TEST_F(StrategyIdLengthTest, StoreExecutionsRejectsTheSameIdentifiersStorePositionsRejects) {
    const char* hostile[] = {
        "",
        "bad--comment",
        "has space",
        "quote'injection",
    };
    for (const char* bad : hostile) {
        auto e = db_->store_executions({an_execution()}, bad, kStrategyName, kPortfolio,
                                       "trading.executions");
        ASSERT_TRUE(e.is_error()) << "store_executions accepted strategy_id '" << bad << "'";
        EXPECT_EQ(e.error()->code(), ErrorCode::INVALID_ARGUMENT)
            << "store_executions refused '" << bad << "' but not via the identifier check: "
            << e.error()->what();

        // And the two writers agree, which is what the asymmetry broke.
        auto p = db_->store_positions({a_position()}, bad, kStrategyName, kPortfolio,
                                      "trading.positions");
        EXPECT_TRUE(p.is_error()) << "store_positions accepted what store_executions refused";
    }
}

// 3. The half that WAS outstanding, now done. Migration 012 widened the three
// narrow columns to the width trading.executions already had, so the joined id a
// third enabled trend strategy produces can be stored everywhere it has to be.
//
// This test fails on a database where 012 has not been applied. That is the
// point: from here on the tripwire is the missing migration, not the missing
// width.
TEST_F(StrategyIdLengthTest, TheThreeColumnsAreWideEnoughForTheJoinedId) {
    for (const char* t : {"positions", "live_results", "signals", "executions"}) {
        EXPECT_EQ(column_width(t), 100)
            << "trading." << t << ".strategy_id is not varchar(100). If it is 50, "
               "migration 012_strategy_id_width.sql has not been applied to this database "
               "and a third enabled trend strategy cannot store " << t << ".";
    }

    // And the id goes in and comes back whole -- not truncated, which is the other
    // way a widening can be got wrong.
    auto r = db_->store_positions({a_position()}, kJoinedId, kStrategyName, kPortfolio,
                                  "trading.positions");
    ASSERT_TRUE(r.is_ok()) << r.error()->what();

    pqxx::connection c(conn_);
    pqxx::work w(c);
    auto rows = w.exec("SELECT strategy_id FROM trading.positions WHERE portfolio_id = " +
                       w.quote(kPortfolio));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].as<std::string>(), kJoinedId)
        << "the id was stored truncated rather than whole";
}

// Ordinary ids -- everything in production today -- are unaffected by the raise.
TEST_F(StrategyIdLengthTest, TodaysLiveIdsAreUnaffected) {
    for (const char* id : {"LIVE_TREND_FOLLOWING",
                           "LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST",
                           "LIVE_EQUITY_MEAN_REVERSION"}) {
        auto r = db_->store_positions({a_position()}, id, kStrategyName, kPortfolio,
                                      "trading.positions");
        EXPECT_TRUE(r.is_ok()) << "id '" << id << "' was refused: " << r.error()->what();
    }
}
