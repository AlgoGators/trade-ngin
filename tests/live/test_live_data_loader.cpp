// Coverage for live_data_loader.cpp focusing on:
// - Constructor throws on null DB
// - validate_connection error path when db is constructed but not connected
//
// Full query-result tests require a live PostgreSQL instance and are
// deferred to the postgres_database refactor (see deliverables/unit_testing/).

#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/live/live_data_loader.hpp"

using namespace trade_ngin;

class LiveDataLoaderTest : public ::testing::Test {
protected:
    // Construct a PostgresDatabase but DON'T call connect(); is_connected()
    // returns false so validate_connection() will return DATABASE_ERROR.
    std::shared_ptr<PostgresDatabase> make_disconnected_db() const {
        return std::make_shared<PostgresDatabase>("host=invalid port=1 user=u dbname=d");
    }
    Timestamp now() const { return std::chrono::system_clock::now(); }
};

TEST_F(LiveDataLoaderTest, ConstructorRejectsNullDb) {
    EXPECT_THROW(LiveDataLoader(nullptr, "trading"), std::invalid_argument);
}

TEST_F(LiveDataLoaderTest, IsConnectedFalseWhenDbNotConnected) {
    LiveDataLoader loader(make_disconnected_db(), "trading");
    EXPECT_FALSE(loader.is_connected());
}

#define ASSERT_DB_ERROR(expr)                                                  \
    do {                                                                       \
        auto __r = (expr);                                                     \
        ASSERT_TRUE(__r.is_error());                                           \
        EXPECT_EQ(__r.error()->code(), ErrorCode::DATABASE_ERROR);             \
    } while (0)

TEST_F(LiveDataLoaderTest, LoadPreviousPortfolioValueDisconnectedErrors) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.load_previous_portfolio_value("S", "P", now()));
}

TEST_F(LiveDataLoaderTest, LoadPortfolioValueDisconnectedErrors) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.load_portfolio_value("S", "P", now()));
}

TEST_F(LiveDataLoaderTest, LoadLiveResultsDisconnectedErrors) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.load_live_results("S", "P", now()));
}

TEST_F(LiveDataLoaderTest, LoadPreviousDayDataDisconnectedErrors) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.load_previous_day_data("S", "P", now()));
}

TEST_F(LiveDataLoaderTest, HasLiveResultsDisconnectedErrors) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.has_live_results("S", "P", now()));
}

TEST_F(LiveDataLoaderTest, GetLiveResultsCountDisconnectedErrors) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.get_live_results_count("S", "P"));
}

TEST_F(LiveDataLoaderTest, LoadDailyReturnsHistoryDisconnectedErrors) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.load_daily_returns_history("S", "P", now()));
}

TEST_F(LiveDataLoaderTest, LoadDailyPnLHistoryDisconnectedErrors) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.load_daily_pnl_history("S", "P", now()));
}
