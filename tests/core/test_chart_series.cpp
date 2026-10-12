// tests/core/test_chart_series.cpp
//
// The DATA of the daily report's charts: the text of each chart's query (ChartQueries) and the
// shaping of its rows into the points that are plotted (ChartSeries). Every test here EXECUTES the
// function it names, with no database and no gnuplot; the fetchers are run against a database
// double that answers a query with a table of text cells, as the live database layer does.
// Drawing a chart (gnuplot) is not unit-tested.
//
// ChartQueriesDb is the extra: the same queries sent to a real database, read-only, through
// TRADE_NGIN_TEST_DSN (it skips, visibly, without one).

#include <gtest/gtest.h>

#include <arrow/api.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../data/test_db_utils.hpp"
#include "trade_ngin/core/chart_generator.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

const std::string kStrategy = "LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST";
const std::string kPortfolio = "BASE_PORTFOLIO";

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Answers each query with the table whose key the query's text holds (the first key that matches,
// in the order given) and records every query it was sent. A query no key matches is an error.
class AnsweringDatabase : public MockPostgresDatabase {
public:
    using Rows = std::vector<std::vector<std::string>>;
    AnsweringDatabase() : MockPostgresDatabase("mock://answering") {}

    void answer(const std::string& key, std::vector<std::string> columns, Rows rows) {
        answers_.push_back({key, std::move(columns), std::move(rows)});
    }

    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string& query) override {
        queries.push_back(query);
        for (const auto& answer : answers_) {
            if (query.find(answer.key) == std::string::npos)
                continue;
            std::vector<std::shared_ptr<arrow::Field>> fields;
            std::vector<std::shared_ptr<arrow::Array>> arrays;
            for (size_t c = 0; c < answer.columns.size(); ++c) {
                arrow::StringBuilder cells;
                for (const auto& row : answer.rows) {
                    if (row[c] == "NULL") {
                        (void)cells.AppendNull();
                    } else {
                        (void)cells.Append(row[c]);
                    }
                }
                std::shared_ptr<arrow::Array> column;
                (void)cells.Finish(&column);
                fields.push_back(arrow::field(answer.columns[c], arrow::utf8()));
                arrays.push_back(column);
            }
            return Result<std::shared_ptr<arrow::Table>>(
                arrow::Table::Make(arrow::schema(fields), arrays));
        }
        return make_error<std::shared_ptr<arrow::Table>>(ErrorCode::DATABASE_ERROR, "not asked");
    }

    std::vector<std::string> queries;

private:
    struct Answer {
        std::string key;
        std::vector<std::string> columns;
        Rows rows;
    };
    std::vector<Answer> answers_;
};

}  // namespace

// --- the queries ---------------------------------------------------------------------------------

TEST(ChartQueries, EveryQueryNamesTheBooksTwoIds) {
    const std::vector<std::string> queries = {
        ChartQueries::equity_curve(kStrategy, kPortfolio, 30),
        ChartQueries::pnl_by_symbol(kStrategy, kPortfolio, "2026-04-25"),
        ChartQueries::daily_pnl(kStrategy, kPortfolio, "2026-04-25", 30),
        ChartQueries::cost_per_million(kStrategy, kPortfolio, "2026-04-25"),
        ChartQueries::margin_posted(kStrategy, kPortfolio, "2026-04-25"),
        ChartQueries::cumulative_pnl_by_symbol(kStrategy, kPortfolio, "2026-04-25"),
    };
    for (const auto& query : queries) {
        EXPECT_TRUE(has(query, "strategy_id = '" + kStrategy + "'")) << query;
        EXPECT_TRUE(has(query, "portfolio_id = '" + kPortfolio + "'")) << query;
    }
}

TEST(ChartQueries, TheDatedQueriesAreBoundedByTheRunDate) {
    EXPECT_TRUE(has(ChartQueries::daily_pnl(kStrategy, kPortfolio, "2026-04-25", 30),
                    "DATE(date) < DATE('2026-04-25')"));
    EXPECT_TRUE(has(ChartQueries::daily_pnl(kStrategy, kPortfolio, "2026-04-25", 30), "LIMIT 30"));
    EXPECT_TRUE(has(ChartQueries::margin_posted(kStrategy, kPortfolio, "2026-04-25"),
                    "DATE(date) <= DATE('2026-04-25')"));
    EXPECT_TRUE(has(ChartQueries::cost_per_million(kStrategy, kPortfolio, "2026-04-25"),
                    "DATE(lr.date) <= DATE('2026-04-25')"));
    EXPECT_TRUE(has(ChartQueries::pnl_by_symbol(kStrategy, kPortfolio, "2026-04-25"),
                    "DATE(last_update) < DATE('2026-04-25')"));
    EXPECT_TRUE(has(ChartQueries::cumulative_pnl_by_symbol(kStrategy, kPortfolio, "2026-04-25"),
                    "DATE(last_update) <= DATE('2026-04-25')"));
}

// T-8D-2 R55: a chart reads the system book's rows only. trading.positions and
// trading.equity_curve carry a portfolio_type (migration 001); the sub-query that finds the
// previous stored day reads the same book as the rows it dates.
TEST(ChartQueries, AQueryOfATableThatCarriesAPortfolioTypeReadsTheSystemBookOnly) {
    const std::string filter = "portfolio_type = 'system'";
    EXPECT_TRUE(has(ChartQueries::equity_curve(kStrategy, kPortfolio, 30), filter));
    EXPECT_TRUE(
        has(ChartQueries::cumulative_pnl_by_symbol(kStrategy, kPortfolio, "2026-04-25"), filter));
    const std::string pnl = ChartQueries::pnl_by_symbol(kStrategy, kPortfolio, "2026-04-25");
    const auto first = pnl.find(filter);
    ASSERT_NE(first, std::string::npos) << pnl;
    EXPECT_NE(pnl.find(filter, first + 1), std::string::npos)
        << "the sub-query that finds the previous stored day is not filtered: " << pnl;
}

// trading.live_results has no portfolio_type column: a query that named it would fail and the
// chart would be left out.
TEST(ChartQueries, AQueryOfLiveResultsNamesNoPortfolioType) {
    for (const auto& query :
         {ChartQueries::daily_pnl(kStrategy, kPortfolio, "2026-04-25", 30),
          ChartQueries::margin_posted(kStrategy, kPortfolio, "2026-04-25"),
          ChartQueries::cost_per_million(kStrategy, kPortfolio, "2026-04-25")}) {
        EXPECT_FALSE(has(query, "portfolio_type")) << query;
    }
}

// --- the equity curve ----------------------------------------------------------------------------

TEST(ChartSeries, TheEquityCurveIsInDateOrderWhateverOrderItsRowsCome) {
    const ChartData data = ChartSeries::equity_curve(
        {{"2026-04-24", 497239.40}, {"2026-04-22", 496100.00}, {"2026-04-23", 495994.40}});
    ASSERT_GE(data.labels.size(), 2u);
    for (size_t i = 1; i < data.labels.size(); ++i) {
        EXPECT_LT(data.labels[i - 1], data.labels[i]);
    }
    EXPECT_EQ(data.x_label, "Date");
    EXPECT_EQ(data.y_label, "Portfolio Value ($)");
}

TEST(ChartSeries, TheEquityCurveKeepsOnlyTheRowsAfterItsLastGapOfMoreThanFiveDays) {
    const ChartData data = ChartSeries::equity_curve({{"2026-03-01", 1.0},
                                                      {"2026-03-02", 2.0},
                                                      {"2026-04-21", 3.0},
                                                      {"2026-04-22", 4.0},
                                                      {"2026-04-23", 5.0}});
    for (const auto& label : data.labels) {
        EXPECT_GE(label, std::string("2026-04-20")) << "a row before the gap is plotted";
    }
    EXPECT_FALSE(data.labels.empty());
}

TEST(ChartSeries, AnEquityCurveWithNoRowIsNoChart) {
    const ChartData data = ChartSeries::equity_curve({});
    EXPECT_TRUE(data.labels.empty());
    EXPECT_TRUE(data.values.empty());
}

// --- yesterday's PnL by symbol -------------------------------------------------------------------

TEST(ChartSeries, PnlBySymbolIsSortedLargestFirstAndTitledWithTheDayBeforeTheRunDate) {
    const ChartData data = ChartSeries::pnl_by_symbol(
        {{"ZF.v.0", -31.25}, {"KE.v.0", 1475.0}, {"6L.v.0", -185.0}}, "2026-04-24");
    ASSERT_EQ(data.labels.size(), 3u);
    EXPECT_EQ(data.labels[0], "KE.v.0");
    EXPECT_EQ(data.labels[1], "ZF.v.0");
    EXPECT_EQ(data.labels[2], "6L.v.0");
    EXPECT_DOUBLE_EQ(data.values[0], 1475.0);
    EXPECT_TRUE(has(data.title, "2026-04-23")) << data.title;
    EXPECT_TRUE(data.has_reference_line);
    EXPECT_DOUBLE_EQ(data.reference_line, 0.0);
}

TEST(ChartSeries, PnlBySymbolWithNoRowIsNoChart) {
    EXPECT_TRUE(ChartSeries::pnl_by_symbol({}, "2026-04-24").labels.empty());
}

// --- daily PnL -----------------------------------------------------------------------------------

TEST(ChartSeries, DailyPnlIsPlottedOldestFirst) {
    const ChartData data = ChartSeries::daily_pnl(
        {{"2026-04-23", 1245.0}, {"2026-04-22", -310.5}, {"2026-04-21", 12.0}});
    ASSERT_EQ(data.labels.size(), 3u);
    EXPECT_EQ(data.labels.front(), "2026-04-21");
    EXPECT_EQ(data.labels.back(), "2026-04-23");
    EXPECT_DOUBLE_EQ(data.values.back(), 1245.0);
    EXPECT_TRUE(ChartSeries::daily_pnl({}).labels.empty());
}

// --- margin posted -------------------------------------------------------------------------------

TEST(ChartSeries, MarginPostedPlotsEachRowAsItComes) {
    const ChartData data =
        ChartSeries::margin_posted({{"2026-04-23", 44100.0}, {"2026-04-24", 45997.05}});
    ASSERT_EQ(data.labels.size(), 2u);
    EXPECT_EQ(data.labels[1], "2026-04-24");
    EXPECT_DOUBLE_EQ(data.values[1], 45997.05);
    EXPECT_EQ(data.title, "Margin Posted");
    EXPECT_TRUE(ChartSeries::margin_posted({}).labels.empty());
}

// --- cumulative PnL by symbol --------------------------------------------------------------------

TEST(ChartSeries, CumulativePnlBySymbolIsSortedLargestFirst) {
    const ChartData data = ChartSeries::cumulative_pnl_by_symbol(
        {{"ZF.v.0", -900.0}, {"KE.v.0", 5200.0}, {"MES.v.0", 40.0}}, "2026-04-24");
    ASSERT_EQ(data.labels.size(), 3u);
    EXPECT_EQ(data.labels[0], "KE.v.0");
    EXPECT_EQ(data.labels[2], "ZF.v.0");
    EXPECT_TRUE(has(data.title, "2026-04-24")) << data.title;
    EXPECT_TRUE(ChartSeries::cumulative_pnl_by_symbol({}, "2026-04-24").labels.empty());
}

// --- the fetchers, against a database that answers -----------------------------------------------

TEST(ChartFetchers, DailyPnlSendsItsQueryAndPlotsTheAnswerOldestFirst) {
    auto db = std::make_shared<AnsweringDatabase>();
    db->answer("daily_pnl", {"date", "daily_pnl"},
               {{"2026-04-23", "1245.00"}, {"2026-04-22", "-310.50"}});
    const ChartData data =
        ChartGenerator::fetch_daily_pnl_data(db, kStrategy, kPortfolio, "2026-04-24", 30);
    ASSERT_EQ(db->queries.size(), 1u);
    EXPECT_EQ(db->queries[0], ChartQueries::daily_pnl(kStrategy, kPortfolio, "2026-04-24", 30));
    ASSERT_EQ(data.labels.size(), 2u);
    EXPECT_EQ(data.labels[0], "2026-04-22");
    EXPECT_DOUBLE_EQ(data.values[0], -310.5);
    EXPECT_DOUBLE_EQ(data.values[1], 1245.0);
}

TEST(ChartFetchers, MarginPostedSkipsADayWithNoStoredMargin) {
    auto db = std::make_shared<AnsweringDatabase>();
    db->answer("margin_posted", {"date", "margin_posted"},
               {{"2026-04-22", "44100.00"}, {"2026-04-23", "NULL"}, {"2026-04-24", "45997.05"}});
    const ChartData data =
        ChartGenerator::fetch_margin_posted_data(db, kStrategy, kPortfolio, "2026-04-24");
    ASSERT_EQ(data.labels.size(), 2u);
    EXPECT_EQ(data.labels[0], "2026-04-22");
    EXPECT_EQ(data.labels[1], "2026-04-24");
}

TEST(ChartFetchers, AQueryThatFailsIsNoChart) {
    auto db = std::make_shared<AnsweringDatabase>();  // answers nothing: every query is an error
    EXPECT_TRUE(
        ChartGenerator::fetch_equity_curve_data(db, kStrategy, kPortfolio, 30).labels.empty());
    EXPECT_TRUE(ChartGenerator::fetch_pnl_by_symbol_data(db, kStrategy, kPortfolio, "2026-04-24")
                    .labels.empty());
    EXPECT_TRUE(ChartGenerator::fetch_cumulative_transaction_costs_data(db, kStrategy, kPortfolio,
                                                                        "2026-04-24")
                    .labels.empty());
    EXPECT_TRUE(
        ChartGenerator::fetch_cumulative_pnl_by_symbol_data(db, kStrategy, kPortfolio, "2026-04-24")
            .labels.empty());
}

// --- the extra: the queries on a real database (read-only; skips without TRADE_NGIN_TEST_DSN) ----

class ChartQueriesDb : public ::testing::Test {
protected:
    void SetUp() override {
        const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
        if (dsn == nullptr || !*dsn) {
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set; no database to send the chart queries to";
        }
        db_ = std::make_shared<PostgresDatabase>(dsn);
        if (db_->connect().is_error() || !db_->is_connected()) {
            GTEST_SKIP() << "database unreachable";
        }
    }
    void TearDown() override {
        if (db_ && db_->is_connected())
            db_->disconnect();
    }
    std::shared_ptr<PostgresDatabase> db_;
};

// A query that names a column its table does not have fails, and its chart is silently left out
// of the report: every chart query must run on the schema as it is.
TEST_F(ChartQueriesDb, EveryChartQueryRunsOnTheDatabase) {
    const std::string strategy = "LIVE_TREND_FOLLOWING";
    const std::string portfolio = "CONSERVATIVE_PORTFOLIO";
    const std::vector<std::string> queries = {
        ChartQueries::equity_curve(strategy, portfolio, 30),
        ChartQueries::pnl_by_symbol(strategy, portfolio, "2026-04-25"),
        ChartQueries::daily_pnl(strategy, portfolio, "2026-04-25", 30),
        ChartQueries::cost_per_million(strategy, portfolio, "2026-04-25"),
        ChartQueries::margin_posted(strategy, portfolio, "2026-04-25"),
        ChartQueries::cumulative_pnl_by_symbol(strategy, portfolio, "2026-04-25"),
    };
    for (const auto& query : queries) {
        auto result = db_->execute_query(query);
        EXPECT_FALSE(result.is_error()) << query << "\n"
                                        << (result.is_error() ? result.error()->what() : "");
    }
}
