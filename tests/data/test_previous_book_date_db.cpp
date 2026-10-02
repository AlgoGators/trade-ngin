// The live leg span's lower bound (LOOP_SPEC v6.1 sections 2.1, 6.5, L-09; T-ROLLX-FIX commit 3): the previous
// run is the latest date before the run date with the book's positions stored, whether or not that day's
// live_results row exists. The run-gap guard lets a run proceed after a day whose positions were stored but
// whose live_results write failed; that day stored its ROLL legs (executions are written before positions), so
// its span must not be booked again. Through TRADE_NGIN_TEST_DSN only, on a throwaway strategy id, every row
// deleted at the end.
#include <gtest/gtest.h>
#include <pqxx/pqxx>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/live/live_roll_legs.hpp"

using namespace trade_ngin;

namespace {
std::string dsn() {
    const char* v = std::getenv("TRADE_NGIN_TEST_DSN");
    return v && *v ? std::string(v) : std::string();
}
Timestamp utc(int y, int m, int d) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}
const char* kId = "TROLLX_BOOKDATE_TEST";
const char* kPortfolio = "TROLLX_BOOKDATE_PORTFOLIO";
}  // namespace

class PreviousBookDateTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string c = dsn();
        if (c.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        db_ = std::make_shared<PostgresDatabase>(c);
        if (db_->connect().is_error()) GTEST_SKIP() << "database unreachable";
        conn_ = c;
        wipe();
    }
    void TearDown() override {
        if (!conn_.empty()) wipe();
        if (db_) db_->disconnect();
    }
    void wipe() {
        pqxx::connection conn(conn_);
        pqxx::work w(conn);
        w.exec("DELETE FROM trading.positions WHERE strategy_id = '" + std::string(kId) + "'");
        w.exec("DELETE FROM trading.live_results WHERE strategy_id = '" + std::string(kId) + "'");
        w.commit();
    }
    void store_book(const Timestamp& day) {
        Position p;
        p.symbol = "6L.v.0";
        p.quantity = Decimal(2.0);
        p.average_price = Decimal(0.20025);
        p.last_update = day;
        p.instrument_id = "36339";
        ASSERT_TRUE(db_->store_positions({p}, kId, "TREND_FOLLOWING", kPortfolio, "trading.positions").is_ok());
    }
    std::shared_ptr<PostgresDatabase> db_;
    std::string conn_;
};

// Books stored on 05-03 and 05-04 and no live_results row at all (05-04's results write "failed"): the run of
// 05-05 bounds its span by 05-04, so a roll confirmed by the 05-03 bar (legged by the 05-04 run) is outside it.
TEST_F(PreviousBookDateTest, ADayWithAStoredBookAndNoLiveResultsIsThePreviousRun) {
    store_book(utc(2026, 5, 3));
    store_book(utc(2026, 5, 4));
    auto prev = db_->get_previous_book_date(kId, kPortfolio, utc(2026, 5, 5), "trading.positions");
    ASSERT_TRUE(prev.is_ok()) << prev.error()->what();
    EXPECT_EQ(prev.value(), "2026-05-04");
    const std::string since = live_legs_since(prev.value(), "2026-05-04");
    EXPECT_EQ(since, "2026-05-03");
    EXPECT_FALSE("2026-05-03" > since) << "the 05-03 confirm was legged by the 05-04 run";
}

// Strictly before the run date (a re-run of a day reads the day before), and empty for a book never stored.
TEST_F(PreviousBookDateTest, TheBoundIsStrictlyBeforeTheRunDateAndEmptyForANewBook) {
    store_book(utc(2026, 5, 3));
    store_book(utc(2026, 5, 4));
    auto rerun = db_->get_previous_book_date(kId, kPortfolio, utc(2026, 5, 4), "trading.positions");
    ASSERT_TRUE(rerun.is_ok());
    EXPECT_EQ(rerun.value(), "2026-05-03");
    auto none = db_->get_previous_book_date("TROLLX_BOOKDATE_NONE", kPortfolio, utc(2026, 5, 4),
                                            "trading.positions");
    ASSERT_TRUE(none.is_ok());
    EXPECT_EQ(none.value(), "");
}
