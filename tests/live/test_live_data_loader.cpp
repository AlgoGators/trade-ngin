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

// ──────────────────────────────────────────────────────────────────────────
// BA-5 / C-1 D2 -- a real pin for 43dfefb7.
//
// That commit's headline fix was load_commissions_by_symbol's column name:
// <schema>.executions stores realised commissions in `commissions_fees`, never
// `commission`, so the old query FAILED at runtime and every caller silently
// degraded to a WARN with an empty map. C-1 found the fix had no test anywhere
// at HEAD -- the one test the commit added covers an unrelated dividend
// contract, so the column name could be reverted and the suite stays green.
//
// The query is built as a string and handed to execute_query, so its SHAPE is
// observable: capture it and assert the column. Note `commission` is a
// SUBSTRING of `commissions_fees`, so the assertion has to name the full
// aggregate expression or it would pass on the broken query too.
// ──────────────────────────────────────────────────────────────────────────
namespace {

class QueryCapturingDb : public PostgresDatabase {
public:
    QueryCapturingDb() : PostgresDatabase("mock://capture") {}

    Result<void> connect() override {
        connected_ = true;
        return Result<void>();
    }
    void disconnect() override { connected_ = false; }
    bool is_connected() const override { return connected_; }

    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string& query) override {
        last_query = query;
        // A null table is the documented "no rows" path in the caller.
        return Result<std::shared_ptr<arrow::Table>>(nullptr);
    }

    std::string last_query;

private:
    bool connected_{true};
};

}  // namespace

TEST_F(LiveDataLoaderTest, CommissionsQueryNamesTheCommissionsFeesColumn) {
    auto db = std::make_shared<QueryCapturingDb>();
    ASSERT_TRUE(db->connect().is_ok());
    LiveDataLoader loader(db, "trading");

    // 2026-08-06 00:00:00 UTC, a date inside the equity book's window.
    std::tm utc{};
    utc.tm_year = 126;
    utc.tm_mon = 7;
    utc.tm_mday = 6;
    const Timestamp when = std::chrono::system_clock::from_time_t(timegm(&utc));

    auto r = loader.load_commissions_by_symbol("EQUITY_MR_PORTFOLIO", when);
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    ASSERT_FALSE(db->last_query.empty()) << "the loader must have issued a query";

    // The column. Asserting the whole aggregate expression matters: "commission"
    // alone is a substring of "commissions_fees" and would match the broken query.
    EXPECT_NE(db->last_query.find("SUM(commissions_fees)"), std::string::npos)
        << "realised commissions live in commissions_fees; SUM(commission) fails at "
           "runtime and the caller degrades to an empty map. Query was:\n"
        << db->last_query;
    EXPECT_EQ(db->last_query.find("SUM(commission)"), std::string::npos)
        << "the pre-fix column name must not come back";

    // The rest of the query's shape, so a rewrite cannot quietly change what is
    // being aggregated or over what.
    EXPECT_NE(db->last_query.find("trading.executions"), std::string::npos);
    EXPECT_NE(db->last_query.find("GROUP BY symbol"), std::string::npos);

    // Portfolio id and date are quoted literals, not bare concatenation -- the
    // other half of what 43dfefb7 changed.
    EXPECT_NE(db->last_query.find("'EQUITY_MR_PORTFOLIO'"), std::string::npos);
    EXPECT_NE(db->last_query.find("'2026-08-06'"), std::string::npos)
        << "the date key must be the UTC date, quoted. Query was:\n"
        << db->last_query;
}

// A portfolio id containing a quote must not break out of the literal.
TEST_F(LiveDataLoaderTest, CommissionsQueryEscapesAQuoteInThePortfolioId) {
    auto db = std::make_shared<QueryCapturingDb>();
    ASSERT_TRUE(db->connect().is_ok());
    LiveDataLoader loader(db, "trading");

    auto r = loader.load_commissions_by_symbol("O'BRIEN", std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    EXPECT_NE(db->last_query.find("'O''BRIEN'"), std::string::npos)
        << "an embedded quote must be doubled. Query was:\n"
        << db->last_query;
}

// ──────────────────────────────────────────────────────────────────────────
// T-8a (2): the equity curve history is one value per UTC date, the LAST row of
// a date, before any statistic reads it (T-8D section 13, row 2).
// ──────────────────────────────────────────────────────────────────────────
namespace {

// Hands back a fixed (equity, utc_date) result, both columns text as
// convert_generic_to_arrow builds them, and keeps the query.
class EquityCurveRowsDb : public PostgresDatabase {
public:
    EquityCurveRowsDb(std::vector<std::string> equity, std::vector<std::string> utc_dates)
        : PostgresDatabase("mock://equity-curve"),
          equity_(std::move(equity)),
          utc_dates_(std::move(utc_dates)) {}

    Result<void> connect() override { return Result<void>(); }
    void disconnect() override {}
    bool is_connected() const override { return true; }

    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string& query) override {
        last_query = query;
        arrow::StringBuilder equity_builder;
        arrow::StringBuilder date_builder;
        std::shared_ptr<arrow::Array> equity_array;
        std::shared_ptr<arrow::Array> date_array;
        if (!equity_builder.AppendValues(equity_).ok() || !equity_builder.Finish(&equity_array).ok() ||
            !date_builder.AppendValues(utc_dates_).ok() || !date_builder.Finish(&date_array).ok()) {
            return make_error<std::shared_ptr<arrow::Table>>(ErrorCode::DATABASE_ERROR,
                                                             "fixture build failed", "test");
        }
        auto schema = arrow::schema({arrow::field("equity", arrow::utf8()),
                                     arrow::field("utc_date", arrow::utf8())});
        return Result<std::shared_ptr<arrow::Table>>(
            arrow::Table::Make(schema, {equity_array, date_array}));
    }

    std::string last_query;

private:
    std::vector<std::string> equity_;
    std::vector<std::string> utc_dates_;
};

}  // namespace

TEST_F(LiveDataLoaderTest, EquityCurveHistoryKeepsTheLastRowOfAUtcDate) {
    // 2026-04-25 is stored three times and 2026-04-27 twice, in timestamp order.
    auto db = std::make_shared<EquityCurveRowsDb>(
        std::vector<std::string>{"500000", "400000", "450000", "510000", "505000", "300000",
                                 "520000"},
        std::vector<std::string>{"2026-04-24", "2026-04-25", "2026-04-25", "2026-04-25",
                                 "2026-04-26", "2026-04-27", "2026-04-27"});
    LiveDataLoader loader(db, "trading");

    auto r = loader.load_equity_curve_history("LIVE_TREND_FOLLOWING", "CONSERVATIVE_PORTFOLIO",
                                              now());
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const std::vector<double> want = {500000.0, 510000.0, 505000.0, 520000.0};
    EXPECT_EQ(r.value(), want)
        << "one value per UTC date, the last row of the date; the 400000 and 300000 rows are "
           "on no date's close and would read as a 20 and a 40 percent drawdown";
}

TEST_F(LiveDataLoaderTest, EquityCurveHistoryWithNoRepeatedDateIsEveryRowInOrder) {
    auto db = std::make_shared<EquityCurveRowsDb>(
        std::vector<std::string>{"500000", "500564.6314", "499000.5"},
        std::vector<std::string>{"2025-10-05", "2025-10-06", "2025-10-07"});
    LiveDataLoader loader(db, "trading");

    auto r = loader.load_equity_curve_history("LIVE_TREND_FOLLOWING", "CONSERVATIVE_PORTFOLIO",
                                              now());
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const std::vector<double> want = {500000.0, 500564.6314, 499000.5};
    EXPECT_EQ(r.value(), want);
}

// The date is the row's UTC date whatever the session's time zone, and the rows still come in
// timestamp order, which is what makes "the last row" the latest one.
TEST_F(LiveDataLoaderTest, EquityCurveHistoryQueryTakesTheUtcDateInTimestampOrder) {
    auto db = std::make_shared<EquityCurveRowsDb>(std::vector<std::string>{"1"},
                                                  std::vector<std::string>{"2026-04-24"});
    LiveDataLoader loader(db, "trading");
    ASSERT_TRUE(loader.load_equity_curve_history("S", "P", now()).is_ok());
    EXPECT_NE(db->last_query.find("to_char(timestamp AT TIME ZONE 'UTC', 'YYYY-MM-DD')"),
              std::string::npos)
        << db->last_query;
    EXPECT_NE(db->last_query.find("ORDER BY timestamp ASC"), std::string::npos) << db->last_query;
    EXPECT_NE(db->last_query.find("trading.equity_curve"), std::string::npos) << db->last_query;
}
