// DB-equity-curve-fallback-stream.
//
// LiveResultsManager::save_equity_curve has a rescue path: when the equity it
// was handed is unusable (zero, negative, NaN, inf, or under $1000) it does not
// write garbage -- it reaches into trading.equity_curve for the most recent
// valid equity and writes that instead.
//
// The SELECT behind that rescue filtered on strategy_id and portfolio_id only.
// trading.equity_curve is keyed (portfolio_id, strategy_id, timestamp,
// portfolio_type), so as soon as a second stream shares a portfolio and
// strategy, "most recent by timestamp" can be a row belonging to the other
// stream. The rescue would then copy that stream's equity onto our row: a
// number with no run behind it, written by the very code whose job is to stop
// unfounded values reaching the table.
//
// One stream exists today, so the predicate changes nothing that is stored --
// which is the claim this test has to make good on in BOTH directions:
//   * with only 'system' rows present the rescue picks exactly what it picked
//     before (byte-identical);
//   * with a 'qt' row that is NEWER and carries a different equity, the rescue
//     must still pick the 'system' row. Without the predicate it picks the qt
//     one, and the test fails.
//
// This is exercised through the real SQL, not a mock, because the defect is in
// the SELECT's WHERE clause and a mock has no WHERE clause to get wrong.

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/storage/live_results_manager.hpp"

using namespace trade_ngin;

namespace {

constexpr const char* kPortfolio = "T1_FALLBACK_PROBE_PORTFOLIO";
constexpr const char* kStrategy = "T1_FALLBACK_PROBE_STRATEGY";
constexpr double kSystemEquity = 123456.0;
constexpr double kQtEquity = 999999.0;

std::string discover_connection_string() {
    const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
    if (dsn && *dsn) return std::string(dsn);
    return {};
}

Timestamp at_utc(int y, int m, int d, int hh) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = hh;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

}  // namespace

class EquityFallbackStreamTest : public ::testing::Test {
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
                          "equity-curve fallback stream predicate goes unverified";
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

    void seed(const std::string& stream, const std::string& ts, double equity) {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        w.exec("INSERT INTO trading.equity_curve "
               "(strategy_id, timestamp, equity, portfolio_id, portfolio_type) VALUES (" +
               w.quote(kStrategy) + "," + w.quote(ts) + "," + w.quote(equity) + "," +
               w.quote(kPortfolio) + "," + w.quote(stream) + ")");
        w.commit();
    }

    // What the runner wrote for the day it was asked to save, on the system stream.
    double stored_system_equity(const std::string& ts) {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        auto r = w.exec("SELECT equity FROM trading.equity_curve WHERE portfolio_id = " +
                        w.quote(kPortfolio) + " AND portfolio_type = 'system' AND timestamp = " +
                        w.quote(ts));
        EXPECT_EQ(r.size(), 1u);
        return r.empty() ? -1.0 : r[0][0].as<double>();
    }

    // Drive the rescue path: hand the manager an equity it must refuse.
    void save_with_invalid_equity(Timestamp date) {
        LiveResultsManager mgr(db_, /*store_enabled=*/true, kStrategy, kPortfolio);
        mgr.set_equity(0.0);  // zero -> "invalid", so the fallback SELECT runs
        ASSERT_TRUE(mgr.save_equity_curve(date).is_ok());
    }

    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

// Byte-identical half: with only system rows, the rescue picks what it always did.
TEST_F(EquityFallbackStreamTest, PicksTheSystemRowWhenItIsTheOnlyStream) {
    seed("system", "2026-05-01 05:00:00+00", kSystemEquity);
    save_with_invalid_equity(at_utc(2026, 5, 4, 5));
    EXPECT_DOUBLE_EQ(stored_system_equity("2026-05-04 05:00:00+00"), kSystemEquity);
}

// The defect: a NEWER row on another stream must not be picked up.
TEST_F(EquityFallbackStreamTest, IgnoresANewerRowBelongingToAnotherStream) {
    seed("system", "2026-05-01 05:00:00+00", kSystemEquity);
    seed("qt", "2026-05-03 05:00:00+00", kQtEquity);  // newer, different stream

    save_with_invalid_equity(at_utc(2026, 5, 4, 5));

    const double got = stored_system_equity("2026-05-04 05:00:00+00");
    EXPECT_DOUBLE_EQ(got, kSystemEquity)
        << "the fallback carried the qt stream's equity (" << kQtEquity
        << ") onto a system row; ORDER BY timestamp DESC crossed the stream boundary";
    EXPECT_NE(got, kQtEquity);
}
