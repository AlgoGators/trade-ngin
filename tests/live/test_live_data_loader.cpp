// Coverage for live_data_loader.cpp focusing on:
// - Constructor throws on null DB
// - validate_connection error path when db is constructed but not connected
//
// Full query-result tests require a live PostgreSQL instance and are
// deferred to the postgres_database refactor (see deliverables/unit_testing/).

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
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

// ──────────────────────────────────────────────────────────────────────────
// T-8a (4): the three statistics histories start at the book's start, the
// metadata row's live_start_date (T-8D ruling R9), read the one way the sizing
// history reads it. The reads that carry the account value do not take it.
// ──────────────────────────────────────────────────────────────────────────
namespace {

// The bound as load_sizing_pnl_history has carried it since T-LOOP, on a date expression.
std::string book_start_bound(const std::string& date_expr) {
    return "AND " + date_expr +
           " >= COALESCE((SELECT MIN(live_start_date) FROM trading.strategy_trading_days_metadata "
           "WHERE strategy_id = 'LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST' "
           "AND portfolio_id = 'BASE_PORTFOLIO'), DATE '0001-01-01') ";
}

// 2026-04-24 05:00:00 UTC, the futures runners' stamp.
Timestamp april_24() {
    std::tm utc{};
    utc.tm_year = 126;
    utc.tm_mon = 3;
    utc.tm_mday = 24;
    utc.tm_hour = 5;
    return std::chrono::system_clock::from_time_t(timegm(&utc));
}

const char* const kBaseKey = "LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST";

}  // namespace

TEST_F(LiveDataLoaderTest, DailyReturnsHistoryStartsAtTheBookStart) {
    auto db = std::make_shared<QueryCapturingDb>();
    LiveDataLoader loader(db, "trading");
    ASSERT_TRUE(loader.load_daily_returns_history(kBaseKey, "BASE_PORTFOLIO", april_24()).is_ok());
    EXPECT_NE(db->last_query.find("AND DATE(date) <= '2026-04-24' " +
                                  book_start_bound("DATE(date)") + "ORDER BY date ASC"),
              std::string::npos)
        << "a live_results row dated before the book's live_start_date enters no statistic. "
           "Query was:\n"
        << db->last_query;
}

TEST_F(LiveDataLoaderTest, DailyPnlHistoryStartsAtTheBookStart) {
    auto db = std::make_shared<QueryCapturingDb>();
    LiveDataLoader loader(db, "trading");
    ASSERT_TRUE(loader.load_daily_pnl_history(kBaseKey, "BASE_PORTFOLIO", april_24()).is_ok());
    EXPECT_NE(db->last_query.find("AND DATE(date) <= '2026-04-24' " +
                                  book_start_bound("DATE(date)") + "ORDER BY date ASC"),
              std::string::npos)
        << db->last_query;
}

TEST_F(LiveDataLoaderTest, EquityCurveHistoryStartsAtTheBookStart) {
    auto db = std::make_shared<EquityCurveRowsDb>(std::vector<std::string>{"1"},
                                                  std::vector<std::string>{"2026-04-24"});
    LiveDataLoader loader(db, "trading");
    ASSERT_TRUE(loader.load_equity_curve_history(kBaseKey, "BASE_PORTFOLIO", april_24()).is_ok());
    EXPECT_NE(db->last_query.find("AND DATE(timestamp) <= '2026-04-24' " +
                                  book_start_bound("DATE(timestamp AT TIME ZONE 'UTC')") +
                                  "ORDER BY timestamp ASC"),
              std::string::npos)
        << "the bound reads the row's UTC date: a row stamped 00:00 UTC on the book's first day "
           "is inside it under any session time zone. Query was:\n"
        << db->last_query;
}

// An empty portfolio id reads BASE_PORTFOLIO in the row filter and in the bound alike.
TEST_F(LiveDataLoaderTest, StatisticsHistoryBoundNamesTheSamePortfolioAsTheRowFilter) {
    auto db = std::make_shared<QueryCapturingDb>();
    LiveDataLoader loader(db, "trading");
    ASSERT_TRUE(loader.load_daily_returns_history(kBaseKey, "", april_24()).is_ok());
    EXPECT_NE(db->last_query.find(book_start_bound("DATE(date)")), std::string::npos)
        << db->last_query;
}

// The sizing history is a sizing input: its query is the text T-LOOP landed, to the byte, plus
// the one column migration 029 adds to it: whether the row's settled_at is set (the sizing read's
// settled test, live/live_sizing_read.hpp). The rows read and their order are unchanged.
TEST_F(LiveDataLoaderTest, SizingPnlHistoryQueryIsUnchangedByTheStatisticsBound) {
    auto db = std::make_shared<QueryCapturingDb>();
    LiveDataLoader loader(db, "trading");
    ASSERT_TRUE(loader.load_sizing_pnl_history(kBaseKey, "BASE_PORTFOLIO", april_24()).is_ok());
    EXPECT_EQ(db->last_query,
              "SELECT to_char(date, 'YYYY-MM-DD') AS sizing_history_date, "
              "COALESCE(daily_pnl, 0)::double precision AS daily_pnl, "
              "COALESCE(active_positions, 0) AS active_positions, "
              "(settled_at IS NOT NULL)::int AS settled_at_set "
              "FROM trading.live_results "
              "WHERE strategy_id = 'LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST' "
              "AND portfolio_id = 'BASE_PORTFOLIO' "
              "AND DATE(date) < '2026-04-24' " +
                  book_start_bound("DATE(date)") + "ORDER BY date ASC");
}

// current_portfolio_value and the sizing capital come from the latest stored row before a date,
// whatever its date: a bound on these reads would move the book (T-8D section 11, part B).
TEST_F(LiveDataLoaderTest, AccountValueReadsCarryNoBookStartBound) {
    auto db = std::make_shared<QueryCapturingDb>();
    LiveDataLoader loader(db, "trading");
    const std::string unbounded_tail =
        "WHERE strategy_id = 'LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST' "
        "AND portfolio_id = 'BASE_PORTFOLIO' "
        "AND DATE(date) < '2026-04-24' "
        "ORDER BY date DESC LIMIT 1";

    (void)loader.load_previous_portfolio_value(kBaseKey, "BASE_PORTFOLIO", april_24());
    EXPECT_EQ(db->last_query, "SELECT COALESCE(current_portfolio_value, 0.0) "
                              "FROM trading.live_results " +
                                  unbounded_tail);

    (void)loader.load_previous_day_data(kBaseKey, "BASE_PORTFOLIO", april_24());
    EXPECT_EQ(db->last_query,
              "SELECT current_portfolio_value, total_pnl, daily_pnl, daily_transaction_costs, date "
              "FROM trading.live_results " +
                  unbounded_tail);

    (void)loader.load_live_results(kBaseKey, "BASE_PORTFOLIO", april_24());
    EXPECT_EQ(db->last_query.find("strategy_trading_days_metadata"), std::string::npos)
        << db->last_query;
}

// PostgresDatabase::get_previous_live_aggregates and the finalize's day_before read build their
// SQL beside a live connection, so the pin is on the source: neither names the metadata table.
TEST_F(LiveDataLoaderTest, PreviousAggregatesAndTheFinalizeReadCarryNoBookStartBound) {
    // The repository root is a parent of the test's working directory.
    const auto read = [](const std::string& relative) -> std::string {
        namespace fs = std::filesystem;
        fs::path dir = fs::current_path();
        for (int i = 0; i < 8 && !dir.empty(); ++i) {
            if (fs::exists(dir / relative)) {
                std::ifstream in(dir / relative);
                std::ostringstream ss;
                ss << in.rdbuf();
                return ss.str();
            }
            dir = dir.parent_path();
        }
        return {};
    };
    const auto npos = std::string::npos;

    const std::string pg = read("src/data/postgres_database.cpp");
    const auto fn = pg.find("PostgresDatabase::get_previous_live_aggregates(");
    ASSERT_NE(fn, npos);
    const auto fn_end = pg.find("\nResult<void> PostgresDatabase::store_trading_equity_curve(", fn);
    ASSERT_NE(fn_end, npos);
    const std::string body = pg.substr(fn, fn_end - fn);
    EXPECT_NE(body.find(" WHERE strategy_id = $1 AND portfolio_id = $2 AND DATE(date) < DATE($3) \"\n"
                        "            \"ORDER BY date DESC, created_at DESC LIMIT 1\";"),
              npos)
        << "the latest stored row before the date, with no lower bound";
    EXPECT_EQ(body.find("live_start_date"), npos);
    EXPECT_EQ(body.find("strategy_trading_days_metadata"), npos);

    for (const char* runner : {"apps/strategies/live_portfolio.cpp",
                               "apps/strategies/live_portfolio_conservative.cpp",
                               "apps/strategies/live_equity_mean_reversion.cpp"}) {
        const std::string src = read(runner);
        const auto cte = src.find("\"WITH day_before AS (\"");
        ASSERT_NE(cte, npos) << runner;
        const auto cte_end = src.find("\"UPDATE trading.live_results", cte);
        ASSERT_NE(cte_end, npos) << runner;
        const std::string day_before = src.substr(cte, cte_end - cte);
        EXPECT_NE(day_before.find("ORDER BY date DESC"), npos) << runner;
        EXPECT_EQ(day_before.find("live_start_date"), npos) << runner;
        EXPECT_EQ(day_before.find("strategy_trading_days_metadata"), npos) << runner;
    }
}

// ──────────────────────────────────────────────────────────────────────────
// T-8a (5a): the inputs of the statistics grid. The levels and the grid dates are bounded by
// the book's start as the three histories are; the futures grid is counted from the bars
// alone (T-8D R3, T-8D-2 R81).
// ──────────────────────────────────────────────────────────────────────────
TEST_F(LiveDataLoaderTest, StatisticsLevelsAreTheStoredAccountValuesFromTheBookStart) {
    auto db = std::make_shared<QueryCapturingDb>();
    LiveDataLoader loader(db, "trading");
    auto r = loader.load_statistics_levels(kBaseKey, "BASE_PORTFOLIO", april_24());
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
    EXPECT_EQ(db->last_query,
              "SELECT to_char(date, 'YYYY-MM-DD') AS level_date, "
              "current_portfolio_value::double precision AS level "
              "FROM trading.live_results "
              "WHERE strategy_id = 'LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST' "
              "AND portfolio_id = 'BASE_PORTFOLIO' "
              "AND current_portfolio_value IS NOT NULL "
              "AND DATE(date) <= '2026-04-24' " +
                  book_start_bound("DATE(date)") + "ORDER BY date ASC");
}

TEST_F(LiveDataLoaderTest, StatisticsLevelsComeBackDatedAndInOrder) {
    auto db = std::make_shared<EquityCurveRowsDb>(
        std::vector<std::string>{"2026-04-24", "2026-04-25"},
        std::vector<std::string>{"497239.3978", "497239.3978"});
    LiveDataLoader loader(db, "trading");
    auto r = loader.load_statistics_levels("S", "P", april_24());
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    ASSERT_EQ(r.value().size(), 2u);
    EXPECT_EQ(r.value()[0].date, "2026-04-24");
    EXPECT_DOUBLE_EQ(r.value()[0].level, 497239.3978);
    EXPECT_EQ(r.value()[1].date, "2026-04-25");
}

TEST_F(LiveDataLoaderTest, BookStartIsTheMetadataAnchorOfTheKey) {
    auto db = std::make_shared<QueryCapturingDb>();
    LiveDataLoader loader(db, "trading");
    auto r = loader.load_book_start(kBaseKey, "BASE_PORTFOLIO");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), "") << "a key with no metadata row has no recorded start";
    EXPECT_EQ(db->last_query,
              "SELECT COALESCE(to_char((SELECT MIN(live_start_date) FROM "
              "trading.strategy_trading_days_metadata WHERE strategy_id = "
              "'LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST' AND portfolio_id = 'BASE_PORTFOLIO'), "
              "'YYYY-MM-DD'), '') AS book_start");
}

TEST_F(LiveDataLoaderTest, FuturesStatisticsGridIsCountedFromTheBarsAlone) {
    auto db = std::make_shared<QueryCapturingDb>();
    LiveDataLoader loader(db, "trading");
    auto r = loader.load_futures_statistics_grid({"6B.v.0", "MES.v.0"}, kBaseKey, "BASE_PORTFOLIO",
                                                 april_24());
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(db->last_query,
              "SELECT to_char(grid_date, 'YYYY-MM-DD') AS grid_date FROM ("
              "SELECT DATE(time) AS grid_date, COUNT(DISTINCT symbol) AS printed "
              "FROM futures_data.ohlcv_1d "
              "WHERE symbol IN ('6B.v.0', 'MES.v.0') "
              "AND DATE(time) <= '2026-04-24' " +
                  book_start_bound("DATE(time)") +
                  "GROUP BY 1) bars "
                  "WHERE printed >= 9 AND EXTRACT(DOW FROM grid_date) <> 6 "
                  "ORDER BY grid_date ASC");
    // Statistics only: nothing of the trading session test is read (R81).
    for (const char* not_read : {"session", "classif", "closed", "volume", "live_results"}) {
        EXPECT_EQ(db->last_query.find(not_read), std::string::npos) << not_read;
    }
}

TEST_F(LiveDataLoaderTest, FuturesStatisticsGridOfNoUniverseIsEmptyAndAsksNothing) {
    auto db = std::make_shared<QueryCapturingDb>();
    LiveDataLoader loader(db, "trading");
    auto r = loader.load_futures_statistics_grid({}, kBaseKey, "BASE_PORTFOLIO", april_24());
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
    EXPECT_TRUE(db->last_query.empty());
}

// Migration 030: what the statistics read beside the series, each from the book's start (the
// bound the three statistics histories use) through the last settled date.
TEST_F(LiveDataLoaderTest, TheSymbolPnlHistoryIsThePositionsCellsSummedOverTheSleeves) {
    auto db = std::make_shared<QueryCapturingDb>();
    LiveDataLoader loader(db, "trading");
    auto r = loader.load_symbol_pnl_history(kBaseKey, "BASE_PORTFOLIO", april_24());
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
    EXPECT_EQ(db->last_query,
              "SELECT to_char(date, 'YYYY-MM-DD') AS pnl_date, symbol, "
              "SUM(COALESCE(daily_realized_pnl, 0))::double precision AS realized, "
              "SUM(COALESCE(daily_unrealized_pnl, 0))::double precision AS unrealized_level "
              "FROM trading.positions "
              "WHERE strategy_id = 'LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST' "
              "AND portfolio_id = 'BASE_PORTFOLIO' "
              "AND DATE(date) <= '2026-04-24' " +
                  book_start_bound("DATE(date)") +
                  "GROUP BY date, symbol ORDER BY date ASC, symbol ASC");
}

TEST_F(LiveDataLoaderTest, TheSizingCapitalHistoryIsRiskDetailsSizingCapitalOfTheRowsThatCarryOne) {
    auto db = std::make_shared<QueryCapturingDb>();
    LiveDataLoader loader(db, "trading");
    auto r = loader.load_sizing_capital_history(kBaseKey, "BASE_PORTFOLIO", april_24());
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(db->last_query,
              "SELECT to_char(date, 'YYYY-MM-DD') AS capital_date, "
              "(risk_detail->>'sizing_capital')::double precision AS sizing_capital "
              "FROM trading.live_results "
              "WHERE strategy_id = 'LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST' "
              "AND portfolio_id = 'BASE_PORTFOLIO' "
              "AND risk_detail->>'sizing_capital' IS NOT NULL "
              "AND DATE(date) <= '2026-04-24' " +
                  book_start_bound("DATE(date)") + "ORDER BY date ASC");
}

TEST_F(LiveDataLoaderTest, TheBooksExecutionsAreReadByTheirStoredDateFromTheBookStart) {
    auto db = std::make_shared<QueryCapturingDb>();
    LiveDataLoader loader(db, "trading");
    auto r = loader.load_book_executions(kBaseKey, "BASE_PORTFOLIO", april_24());
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(db->last_query,
              "SELECT exec_id, order_id, symbol, side, quantity::double precision AS quantity, "
              "price::double precision AS price, to_char(date, 'YYYY-MM-DD') AS fill_date, "
              "COALESCE(total_transaction_costs, 0)::double precision AS total_transaction_costs, "
              "COALESCE(netting_adjustment, 0)::double precision AS netting_adjustment, "
              "execution_type, COALESCE(instrument_id, '') AS instrument_id "
              "FROM trading.executions "
              "WHERE strategy_id = 'LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST' "
              "AND portfolio_id = 'BASE_PORTFOLIO' "
              "AND DATE(date) <= '2026-04-24' " +
                  book_start_bound("DATE(date)") + "ORDER BY date ASC, exec_id ASC");
    // The day the sleeves' rows are netted on is the stored date, never the execution time.
    EXPECT_EQ(db->last_query.find("execution_time"), std::string::npos);
}

// The previous value a run carries when it cannot recount the dividends: the cell of one row.
TEST_F(LiveDataLoaderTest, TheStoredDividendIncomeIsTheCellOfTheKeysRowOfTheDate) {
    auto capturing = std::make_shared<QueryCapturingDb>();
    LiveDataLoader bare(capturing, "trading");
    auto none = bare.load_stored_dividend_income("LIVE_EQUITY_MEAN_REVERSION",
                                                 "EQUITY_MR_PORTFOLIO", april_24());
    ASSERT_TRUE(none.is_ok());
    EXPECT_FALSE(none.value().has_value()) << "a date with no row has no stored figure";
    EXPECT_EQ(capturing->last_query,
              "SELECT total_dividend_income::double precision AS total_dividend_income "
              "FROM trading.live_results WHERE strategy_id = 'LIVE_EQUITY_MEAN_REVERSION' "
              "AND portfolio_id = 'EQUITY_MR_PORTFOLIO' AND DATE(date) = '2026-04-24'");

    auto db = std::make_shared<EquityCurveRowsDb>(std::vector<std::string>{"6.62138"},
                                                  std::vector<std::string>{"2026-04-24"});
    LiveDataLoader loader(db, "trading");
    auto stored = loader.load_stored_dividend_income("S", "P", april_24());
    ASSERT_TRUE(stored.is_ok()) << stored.error()->what();
    ASSERT_TRUE(stored.value().has_value());
    EXPECT_DOUBLE_EQ(*stored.value(), 6.62138);

    LiveDataLoader disconnected(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(disconnected.load_stored_dividend_income("S", "P", now()));
}

TEST_F(LiveDataLoaderTest, TheMigration030HistoriesLoadDisconnectedError) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.load_symbol_pnl_history("S", "P", now()));
    ASSERT_DB_ERROR(l.load_sizing_capital_history("S", "P", now()));
    ASSERT_DB_ERROR(l.load_book_executions("S", "P", now()));
}

TEST_F(LiveDataLoaderTest, StatisticsGridLoadsDisconnectedError) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.load_statistics_levels("S", "P", now()));
    ASSERT_DB_ERROR(l.load_book_start("S", "P"));
    ASSERT_DB_ERROR(l.load_futures_statistics_grid({"X"}, "S", "P", now()));
}
