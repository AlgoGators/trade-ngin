// DB-equity-curve-onconflict (with the REG-F15 half).
//
// trading.equity_curve is UNIQUE (portfolio_id, strategy_id, timestamp,
// portfolio_type) and store_trading_equity_curve's ON CONFLICT named all four
// columns -- but the INSERT listed only three, so portfolio_type came from the
// column DEFAULT. That works only while the default and the intended stream are
// the same value. It is 'system' today, so nothing is broken; the writer simply
// had no way to express which stream it meant, and no way to write a second one.
//
// What the tests below establish, in order:
//   1. the default write still lands on portfolio_type='system', so no stored
//      row moves (this is the class-A byte-identical claim);
//   2. re-writing the same (portfolio, strategy, timestamp, stream) UPDATES in
//      place rather than inserting a duplicate -- the upsert genuinely resolves;
//   3. two different streams at the same timestamp are two rows, and updating
//      one does not touch the other. Before the fix this could not be written at
//      all, and had the column default ever differed from the conflict target,
//      every upsert would have silently become an insert.
//
// Writes are confined to a scratch portfolio id that no book uses, and TearDown
// removes them.
//
// Reachability gate matches tests/data/test_delete_stale_executions_scope_db.cpp.

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {

constexpr const char* kPortfolio = "T1_EQCURVE_PROBE_PORTFOLIO";
constexpr const char* kStrategy = "T1_EQCURVE_PROBE_STRATEGY";

std::string discover_connection_string() {
    const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
    if (dsn && *dsn) return std::string(dsn);
    return {};
}

Timestamp point_in_time() {
    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 4;
    tm.tm_mday = 4;
    tm.tm_hour = 5;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

}  // namespace

class EquityCurveStreamTest : public ::testing::Test {
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
                          "equity-curve stream key goes unverified";
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
        w.exec("DELETE FROM trading.equity_curve WHERE portfolio_id = " + w.quote(kPortfolio));
        w.commit();
    }

    struct Row {
        std::string stream;
        double equity;
    };

    std::vector<Row> read_rows() {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        auto r = w.exec("SELECT portfolio_type, equity FROM trading.equity_curve "
                        "WHERE portfolio_id = " + w.quote(kPortfolio) + " ORDER BY portfolio_type");
        std::vector<Row> out;
        for (const auto& row : r) out.push_back({row[0].as<std::string>(), row[1].as<double>()});
        return out;
    }

    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

// 1. The byte-identical claim: the default write is a 'system' row, as before.
TEST_F(EquityCurveStreamTest, DefaultWriteLandsOnTheSystemStream) {
    ASSERT_TRUE(db_->store_trading_equity_curve(kStrategy, point_in_time(), 100010.0, kPortfolio)
                    .is_ok());
    auto rows = read_rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].stream, "system")
        << "the default stream moved; every stored equity_curve row would change";
    EXPECT_DOUBLE_EQ(rows[0].equity, 100010.0);
}

// 2. The upsert resolves: a second write to the same key updates, not duplicates.
TEST_F(EquityCurveStreamTest, RewritingTheSameKeyUpdatesInPlace) {
    ASSERT_TRUE(db_->store_trading_equity_curve(kStrategy, point_in_time(), 100010.0, kPortfolio)
                    .is_ok());
    ASSERT_TRUE(db_->store_trading_equity_curve(kStrategy, point_in_time(), 100999.0, kPortfolio)
                    .is_ok());
    auto rows = read_rows();
    ASSERT_EQ(rows.size(), 1u) << "the upsert inserted a duplicate instead of updating";
    EXPECT_DOUBLE_EQ(rows[0].equity, 100999.0);
}

// 3. What the signature could not previously say: two streams, independently keyed.
TEST_F(EquityCurveStreamTest, TwoStreamsAtTheSameTimestampAreTwoIndependentRows) {
    ASSERT_TRUE(db_->store_trading_equity_curve(kStrategy, point_in_time(), 100010.0, kPortfolio,
                                                "trading.equity_curve", "system")
                    .is_ok());
    ASSERT_TRUE(db_->store_trading_equity_curve(kStrategy, point_in_time(), 200020.0, kPortfolio,
                                                "trading.equity_curve", "qt")
                    .is_ok());
    auto rows = read_rows();
    ASSERT_EQ(rows.size(), 2u) << "the two streams collapsed onto one row";

    // Update only the qt row; system must not move.
    ASSERT_TRUE(db_->store_trading_equity_curve(kStrategy, point_in_time(), 200999.0, kPortfolio,
                                                "trading.equity_curve", "qt")
                    .is_ok());
    rows = read_rows();
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].stream, "qt");
    EXPECT_DOUBLE_EQ(rows[0].equity, 200999.0);
    EXPECT_EQ(rows[1].stream, "system");
    EXPECT_DOUBLE_EQ(rows[1].equity, 100010.0)
        << "writing the qt stream overwrote the system stream";
}

// The batch writer shares the defect and the fix.
TEST_F(EquityCurveStreamTest, BatchWriterCarriesTheStreamToo) {
    std::vector<std::pair<Timestamp, double>> pts{{point_in_time(), 100010.0}};
    ASSERT_TRUE(db_->store_trading_equity_curve_batch(kStrategy, pts, kPortfolio,
                                                      "trading.equity_curve", "qt")
                    .is_ok());
    auto rows = read_rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].stream, "qt");
}
