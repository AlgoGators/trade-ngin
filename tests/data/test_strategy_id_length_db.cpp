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
//   3. the 62-character id STILL fails at the database, because
//      trading.positions.strategy_id is varchar(50). This is asserted, not
//      hoped: if somebody later widens the column, this test fails and points at
//      the ledger row, which is exactly when the rest of E2-F36 can be closed.
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

    // The validator no longer refuses it: whatever stops this id now, it is not
    // the length check.
    auto p = db_->store_positions({a_position()}, kJoinedId, kStrategyName, kPortfolio,
                                  "trading.positions");
    ASSERT_TRUE(p.is_error()) << "the varchar(50) column accepted 62 characters";
    EXPECT_NE(p.error()->code(), ErrorCode::INVALID_ARGUMENT)
        << "the 62-character joined id is still being refused by validate_strategy_id's "
           "length cap rather than reaching the database (E2-F36): " << p.error()->what();

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

// 3. The half that is NOT done. Asserted so the day it changes is visible.
TEST_F(StrategyIdLengthTest, PositionsColumnIsStillTooNarrowForTheJoinedId) {
    ASSERT_EQ(column_width("positions"), 50)
        << "trading.positions.strategy_id is no longer varchar(50). If it was widened "
           "deliberately, the rest of E2-F36 can now be closed: update this test and the "
           "ledger row.";
    ASSERT_EQ(column_width("live_results"), 50);
    ASSERT_EQ(column_width("signals"), 50);
    ASSERT_EQ(column_width("executions"), 100);

    // So a third strategy still cannot store positions -- but it fails at the
    // database, loudly, with the value in the message, rather than being turned
    // away by a string length check that had no schema behind it.
    auto r = db_->store_positions({a_position()}, kJoinedId, kStrategyName, kPortfolio,
                                  "trading.positions");
    ASSERT_TRUE(r.is_error())
        << "a 62-character id was stored in a varchar(50) column; the schema changed under "
           "this test";
    EXPECT_EQ(r.error()->code(), ErrorCode::DATABASE_ERROR)
        << "the refusal should now come from the server, not the validator";

    // And nothing was written. This is the assertion that matters: whatever the
    // message says, the book must not have acquired a row.
    pqxx::connection c(conn_);
    pqxx::work w(c);
    auto rows = w.exec("SELECT count(*) FROM trading.positions WHERE portfolio_id = " +
                       w.quote(kPortfolio));
    EXPECT_EQ(rows[0][0].as<int>(), 0);

    // NOTE, and this is a finding rather than an expectation: the error text
    // that comes back is NOT the server's "value too long for type character
    // varying(50)". It is "current transaction is aborted, commands ignored
    // until end of transaction block".
    //
    // store_positions wraps its INSERT in a catch that assumes ONE cause --
    // "strategy_id column may not exist, trying without it" -- and retries with
    // empty strategy_id/strategy_name and a hardcoded 'BASE_PORTFOLIO'
    // portfolio_id. The retry runs on the same, now-aborted, transaction, so it
    // fails too and its error is what the caller sees. The real diagnosis is
    // only in a WARN line.
    //
    // Here the retry harmlessly fails. The reason it is worth writing down is
    // what it would do if it did NOT: on any insert failure with a live
    // transaction it would write this portfolio's positions into BASE_PORTFOLIO
    // with no strategy attribution. Out of scope for E2-F36; reported for the
    // ledger.
    EXPECT_NE(std::string(r.error()->what()).find("ERROR"), std::string::npos)
        << "expected a server error to be reported; got: " << r.error()->what();
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
