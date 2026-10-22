// The invalid equity-curve value, against the real table (T-8D-2 R47).
//
// LiveResultsManager::save_equity_curve used to have a rescue path: handed an equity it could
// not store (zero, negative, NaN, inf, or under $1000) it read the most recent stored equity of
// at least 1,000 from trading.equity_curve and wrote THAT for the day: a number with no run
// behind it, taken with no bound on the row's date (in a replay, a later day's), in the table
// the statistics read. This file first pinned which stream that rescue read
// (DB-equity-curve-fallback-stream); R47 removed the rescue, and the file now pins the refusal
// on the same seeded rows:
//   * with a valid 'system' row stored, an invalid value writes NO row for its date, returns an
//     error, and leaves the stored row as it was;
//   * the same with a NEWER row on another stream ('qt') beside it: nothing of either stream is
//     copied onto the day.
//
// It runs against the real table, not a mock: the claim is about the rows the table holds after
// the call.

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <vector>

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

    // The rows of the probe portfolio dated `ts`, on any stream.
    size_t rows_at(const std::string& ts) {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        auto r = w.exec("SELECT count(*) FROM trading.equity_curve WHERE portfolio_id = " +
                        w.quote(kPortfolio) + " AND timestamp = " + w.quote(ts));
        return r[0][0].as<size_t>();
    }

    // Every stored row of the probe portfolio: "stream timestamp-epoch equity", ordered.
    std::vector<std::string> stored_rows() {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        auto r = w.exec("SELECT portfolio_type || ' ' || extract(epoch from timestamp)::bigint || "
                        "' ' || equity FROM trading.equity_curve WHERE portfolio_id = " +
                        w.quote(kPortfolio) + " ORDER BY 1");
        std::vector<std::string> out;
        for (const auto& row : r) out.push_back(row[0].as<std::string>());
        return out;
    }

    // Hand the manager an equity it must refuse.
    Result<void> save_with_invalid_equity(Timestamp date, double equity = 0.0) {
        LiveResultsManager mgr(db_, /*store_enabled=*/true, kStrategy, kPortfolio);
        mgr.set_equity(equity);
        return mgr.save_equity_curve(date);
    }

    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

// With a valid system row stored, the invalid value is refused: no row for the day.
TEST_F(EquityFallbackStreamTest, RefusesAnInvalidValueAndWritesNoRowBesideAStoredSystemRow) {
    seed("system", "2026-05-01 05:00:00+00", kSystemEquity);
    const auto before = stored_rows();
    ASSERT_EQ(before.size(), 1u);

    for (double equity : {0.0, -1.0, 999.0, std::numeric_limits<double>::quiet_NaN()}) {
        auto r = save_with_invalid_equity(at_utc(2026, 5, 4, 5), equity);
        ASSERT_TRUE(r.is_error()) << equity;
        EXPECT_NE(std::string(r.error()->what()).find("2026-05-04"), std::string::npos)
            << r.error()->what();
        EXPECT_EQ(rows_at("2026-05-04 05:00:00+00"), 0u)
            << "a row was written for the day of the invalid value " << equity;
    }
    EXPECT_EQ(stored_rows(), before) << "the stored row was touched";
}

// A NEWER row on another stream changes nothing: neither stream's equity is carried to the day.
TEST_F(EquityFallbackStreamTest, RefusesAnInvalidValueWithANewerRowOnAnotherStream) {
    seed("system", "2026-05-01 05:00:00+00", kSystemEquity);
    seed("qt", "2026-05-03 05:00:00+00", kQtEquity);  // newer, different stream
    const auto before = stored_rows();
    ASSERT_EQ(before.size(), 2u);

    auto r = save_with_invalid_equity(at_utc(2026, 5, 4, 5));
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(rows_at("2026-05-04 05:00:00+00"), 0u);
    EXPECT_EQ(stored_rows(), before);
}

// The valid path is as it was: the value handed in is the value stored.
TEST_F(EquityFallbackStreamTest, AValidValueIsStoredAsHandedIn) {
    seed("system", "2026-05-01 05:00:00+00", kSystemEquity);
    LiveResultsManager mgr(db_, /*store_enabled=*/true, kStrategy, kPortfolio);
    mgr.set_equity(234567.5);
    ASSERT_TRUE(mgr.save_equity_curve(at_utc(2026, 5, 4, 5)).is_ok());
    pqxx::connection c(conn_);
    pqxx::work w(c);
    auto row = w.exec("SELECT equity FROM trading.equity_curve WHERE portfolio_id = " +
                      w.quote(kPortfolio) + " AND timestamp = '2026-05-04 05:00:00+00'");
    ASSERT_EQ(row.size(), 1u);
    EXPECT_DOUBLE_EQ(row[0][0].as<double>(), 234567.5);
}
