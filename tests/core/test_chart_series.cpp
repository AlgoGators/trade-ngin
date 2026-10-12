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

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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
        ChartQueries::equity_curve(kStrategy, kPortfolio, "2026-04-25", 30),
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
    EXPECT_TRUE(has(ChartQueries::equity_curve(kStrategy, kPortfolio, "2026-04-25", 30), filter));
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

// T-8D-2 R63: the chart holds the stored days BEFORE the run date (the run date's own row is not
// settled, and a re-run of a past date must not chart the days after it).
TEST(ChartQueries, TheEquityCurveReadsTheStoredDaysBeforeTheRunDate) {
    const std::string query = ChartQueries::equity_curve(kStrategy, kPortfolio, "2026-04-25", 30);
    EXPECT_TRUE(has(query, "DATE(timestamp) < DATE('2026-04-25')")) << query;
    EXPECT_TRUE(has(query, "ORDER BY timestamp DESC LIMIT 30")) << query;
}

TEST(ChartQueries, TheBooksCapitalIsReadFromItsLatestRunMetadataOnOrBeforeTheRunDate) {
    const std::string query = ChartQueries::book_capital(kStrategy, kPortfolio, "2026-04-25");
    EXPECT_TRUE(has(query, "portfolio_config->>'total_capital'")) << query;
    EXPECT_TRUE(has(query, "FROM trading.live_run_metadata")) << query;
    EXPECT_TRUE(has(query, "strategy_id = '" + kStrategy + "'")) << query;
    EXPECT_TRUE(has(query, "portfolio_id = '" + kPortfolio + "'")) << query;
    EXPECT_TRUE(has(query, "DATE(date) <= DATE('2026-04-25')")) << query;
    EXPECT_TRUE(has(query, "ORDER BY date DESC LIMIT 1")) << query;
}

TEST(ChartSeries, TheEquityCurveIsInDateOrderWhateverOrderItsRowsCome) {
    const ChartData data = ChartSeries::equity_curve(
        {{"2026-04-24", 497239.40}, {"2026-04-22", 496100.00}, {"2026-04-23", 495994.40}},
        std::nullopt);
    ASSERT_GE(data.labels.size(), 2u);
    for (size_t i = 1; i < data.labels.size(); ++i) {
        EXPECT_LT(data.labels[i - 1], data.labels[i]);
    }
    EXPECT_EQ(data.x_label, "Date");
    EXPECT_EQ(data.y_label, "Portfolio Value ($)");
}

// T-8D-2 R63: the curve used to start with a made-up point of 500,000 one day before its first
// row (on every book, the 100,000 equity book included) and to drop its last row. The points are
// now the rows it was given, each with its own value, and no other.
TEST(ChartSeries, TheEquityCurvePlotsItsStoredRowsAndNoOtherPoint) {
    const ChartData data = ChartSeries::equity_curve(
        {{"2026-04-22", 496100.00}, {"2026-04-23", 495994.40}, {"2026-04-24", 497239.40}},
        500000.0);
    ASSERT_EQ(data.labels.size(), 3u);
    ASSERT_EQ(data.values.size(), 3u);
    EXPECT_EQ(data.labels[0], "2026-04-22");
    EXPECT_EQ(data.labels[2], "2026-04-24");
    EXPECT_DOUBLE_EQ(data.values[0], 496100.00);
    EXPECT_DOUBLE_EQ(data.values[1], 495994.40);
    EXPECT_DOUBLE_EQ(data.values[2], 497239.40);
    for (double value : data.values) {
        EXPECT_NE(value, 500000.0) << "a point the book did not store";
    }
}

TEST(ChartSeries, TheEquityCurveIsCaptionedThroughItsLastDaySettled) {
    const ChartData data = ChartSeries::equity_curve(
        {{"2026-04-23", 495994.40}, {"2026-04-24", 497239.40}}, std::nullopt);
    EXPECT_EQ(data.title, "Equity Curve through 2026-04-24 (settled)");
}

// The reference line is the book's own starting capital (500,000 on a futures book, 100,000 on
// the equity book), and there is none when the capital is not on record.
TEST(ChartSeries, TheEquityCurvesReferenceLineIsTheBooksCapitalOrThereIsNone) {
    const std::vector<ChartPoint> rows = {{"2026-06-15", 99780.0}, {"2026-06-16", 99782.7}};
    const ChartData futures = ChartSeries::equity_curve(rows, 500000.0);
    EXPECT_TRUE(futures.has_reference_line);
    EXPECT_DOUBLE_EQ(futures.reference_line, 500000.0);
    const ChartData equity = ChartSeries::equity_curve(rows, 100000.0);
    EXPECT_TRUE(equity.has_reference_line);
    EXPECT_DOUBLE_EQ(equity.reference_line, 100000.0);
    EXPECT_FALSE(ChartSeries::equity_curve(rows, std::nullopt).has_reference_line);
    EXPECT_FALSE(ChartSeries::equity_curve(rows, 0.0).has_reference_line);
    EXPECT_FALSE(ChartSeries::equity_curve(rows, std::nan("")).has_reference_line);
}

// A book whose last thirty days all lie below (or above) its capital: drawn on an axis scaled to
// its points alone, the capital line is off the chart and the reader sees none. The curve's value
// axis therefore runs from the lowest of the points and the line to the highest.
TEST(ChartSeries, TheEquityCurvesValueAxisHoldsItsCapitalLine) {
    const ChartData below = ChartSeries::equity_curve(
        {{"2026-04-22", 487650.0}, {"2026-04-23", 495994.4}, {"2026-04-24", 497239.4}}, 500000.0);
    const auto axis = ChartSeries::line_chart_y_range(below);
    ASSERT_TRUE(axis.has_value());
    EXPECT_LT(axis->first, 487650.0);
    EXPECT_GT(axis->second, 500000.0);
    EXPECT_DOUBLE_EQ(axis->first, 487650.0 - (500000.0 - 487650.0) * 0.05);
    EXPECT_DOUBLE_EQ(axis->second, 500000.0 + (500000.0 - 487650.0) * 0.05);

    const ChartData above =
        ChartSeries::equity_curve({{"2026-06-15", 101200.0}, {"2026-06-16", 103400.0}}, 100000.0);
    const auto above_axis = ChartSeries::line_chart_y_range(above);
    ASSERT_TRUE(above_axis.has_value());
    EXPECT_LT(above_axis->first, 100000.0);
    EXPECT_GT(above_axis->second, 103400.0);

    // a curve flat at its capital still has an axis with room either side
    const auto flat = ChartSeries::line_chart_y_range(
        ChartSeries::equity_curve({{"2026-04-01", 100000.0}}, 100000.0));
    ASSERT_TRUE(flat.has_value());
    EXPECT_LT(flat->first, 100000.0);
    EXPECT_GT(flat->second, 100000.0);

    // no capital on record: no line, and the renderer scales the axis to the points
    EXPECT_FALSE(ChartSeries::line_chart_y_range(
                     ChartSeries::equity_curve({{"2026-04-24", 497239.4}}, std::nullopt))
                     .has_value());
    // the margin chart's zero line does not pull its axis down to zero
    EXPECT_FALSE(
        ChartSeries::line_chart_y_range(
            ChartSeries::margin_posted({{"2026-04-23", 44100.0}, {"2026-04-24", 45997.05}}))
            .has_value());
}

TEST(ChartSeries, TheEquityCurveKeepsOnlyTheRowsAfterItsLastGapOfMoreThanFiveDays) {
    const ChartData data = ChartSeries::equity_curve({{"2026-03-01", 1.0},
                                                      {"2026-03-02", 2.0},
                                                      {"2026-04-21", 3.0},
                                                      {"2026-04-22", 4.0},
                                                      {"2026-04-23", 5.0}},
                                                     std::nullopt);
    ASSERT_EQ(data.labels.size(), 3u);
    EXPECT_EQ(data.labels.front(), "2026-04-21");
    EXPECT_EQ(data.labels.back(), "2026-04-23");
}

TEST(ChartSeries, AnEquityCurveWithNoRowIsNoChart) {
    const ChartData data = ChartSeries::equity_curve({}, 500000.0);
    EXPECT_TRUE(data.labels.empty());
    EXPECT_TRUE(data.values.empty());
}

// A book with ONE stored day before the run date: one point. Its time axis cannot run from that
// day to itself (gnuplot: "Can't plot with an empty x range!"), so it runs a day either side.
TEST(ChartSeries, ABookWithOneStoredDayIsOnePointOnAnAxisOfThreeDays) {
    const ChartData data = ChartSeries::equity_curve({{"2026-03-01", 100000.0}}, 100000.0);
    ASSERT_EQ(data.labels.size(), 1u);
    EXPECT_EQ(data.labels[0], "2026-03-01");
    EXPECT_DOUBLE_EQ(data.values[0], 100000.0);
    EXPECT_EQ(data.title, "Equity Curve through 2026-03-01 (settled)");

    const auto range = ChartSeries::line_chart_x_range(data.labels);
    EXPECT_EQ(range.first, "2026-02-28");
    EXPECT_EQ(range.second, "2026-03-02");
    EXPECT_NE(range.first, range.second);
}

TEST(ChartSeries, ALineChartsAxisRunsFromItsFirstDayToItsLast) {
    const auto range = ChartSeries::line_chart_x_range({"2026-04-22", "2026-04-23", "2026-04-24"});
    EXPECT_EQ(range.first, "2026-04-22");
    EXPECT_EQ(range.second, "2026-04-24");
    // the turn of a year and a leap day
    EXPECT_EQ(ChartSeries::line_chart_x_range({"2026-01-01"}).first, "2025-12-31");
    EXPECT_EQ(ChartSeries::line_chart_x_range({"2028-02-28"}).second, "2028-02-29");
    // no label, and one label that is not a date: no axis (the renderer leaves the chart out)
    EXPECT_TRUE(ChartSeries::line_chart_x_range({}).first.empty());
    EXPECT_TRUE(ChartSeries::line_chart_x_range({"not-a-date"}).first.empty());
    EXPECT_TRUE(ChartSeries::line_chart_x_range({"2026-02-30"}).first.empty());
}

// --- yesterday's PnL by symbol -------------------------------------------------------------------

// T-8D-2 R64: a symbol held by both sleeves of a book has two stored rows on a day; the query
// returns the book's one figure for it.
TEST(ChartQueries, YesterdaysPnlIsSummedBySymbol) {
    const std::string query = ChartQueries::pnl_by_symbol(kStrategy, kPortfolio, "2026-04-25");
    EXPECT_TRUE(has(query, "SELECT symbol, SUM(daily_realized_pnl)")) << query;
    EXPECT_TRUE(has(query, "GROUP BY symbol")) << query;
}

TEST(ChartSeries, PnlBySymbolIsSortedLargestFirstAndTitledWithTheDayBeforeTheRunDate) {
    const ChartData data = ChartSeries::pnl_by_symbol(
        {{"ZF.v.0", -31.25}, {"KE.v.0", 1475.0}, {"6L.v.0", -185.0}}, "2026-04-24");
    ASSERT_EQ(data.labels.size(), 3u);
    EXPECT_EQ(data.labels[0], "KE.v.0");
    EXPECT_EQ(data.labels[1], "ZF.v.0");
    EXPECT_EQ(data.labels[2], "6L.v.0");
    EXPECT_DOUBLE_EQ(data.values[0], 1475.0);
    EXPECT_EQ(data.title, "Yesterday's PnL by Symbol (2026-04-23)");
    EXPECT_TRUE(data.has_reference_line);
    EXPECT_DOUBLE_EQ(data.reference_line, 0.0);
}

// T-8D-2 R64: a symbol two sleeves hold was two bars, each a sleeve's figure. It is one bar of
// their sum, whatever rows the series is given.
TEST(ChartSeries, ASymbolHeldByTwoSleevesIsOneBarOfTheirSum) {
    const ChartData data = ChartSeries::pnl_by_symbol({{"MES.v.0", 40.0},
                                                       {"ZF.v.0", -31.25},
                                                       {"MES.v.0", -45.0},
                                                       {"KE.v.0", 1475.0},
                                                       {"ZF.v.0", -31.25}},
                                                      "2026-04-25");
    ASSERT_EQ(data.labels.size(), 3u);
    EXPECT_EQ(data.labels[0], "KE.v.0");
    EXPECT_EQ(data.labels[1], "MES.v.0");
    EXPECT_EQ(data.labels[2], "ZF.v.0");
    EXPECT_DOUBLE_EQ(data.values[0], 1475.0);
    EXPECT_DOUBLE_EQ(data.values[1], -5.0);
    EXPECT_DOUBLE_EQ(data.values[2], -62.5);
}

// T-8D-2 R64: on an equity book the bars are the day's realized P&L only, and the title says so.
TEST(ChartSeries, OnAnEquityBookYesterdaysPnlIsTitledRealized) {
    const std::vector<ChartPoint> rows = {{"AAPL", 0.02}, {"GOOGL", -1.87}};
    EXPECT_EQ(ChartSeries::pnl_by_symbol(rows, "2026-06-16", true).title,
              "Yesterday's Realized PnL by Symbol (2026-06-15)");
    EXPECT_EQ(ChartSeries::pnl_by_symbol(rows, "2026-06-16", false).title,
              "Yesterday's PnL by Symbol (2026-06-15)");
    EXPECT_EQ(ChartTitles::pnl_by_symbol_heading(true), "Yesterday's Realized PnL by Symbol");
    EXPECT_EQ(ChartTitles::pnl_by_symbol_heading(false), "Yesterday's PnL by Symbol");
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

// T-8D-2 R62: the chart holds the last 30 STORED days, which on a book with missing days is not
// the last 30 days; the heading said "Last 30 Days".
TEST(ChartTitles, TheDailyPnlHeadingCountsStoredDays) {
    EXPECT_EQ(ChartTitles::daily_pnl_heading(30), "Daily PnL (last 30 stored days)");
}

// --- cost per $1M traded -------------------------------------------------------------------------

namespace {
// Contract multipliers of the test's symbols; a symbol that is not listed is not on record.
std::optional<double> test_multiplier(const std::string& symbol) {
    if (symbol == "6J.v.0")
        return 12500000.0;
    if (symbol == "ZN.v.0")
        return 1000.0;
    if (symbol == "MES.v.0")
        return 5.0;
    if (symbol == "AAPL")
        return 1.0;
    return std::nullopt;
}
}  // namespace

// T-8D-2 R64: the query read the book's HELD gross notional as the denominator and joined a count
// of the executions of EVERY book (used for a log line only). It reads the book's own fills.
TEST(ChartQueries, TheCostChartReadsTheBooksOwnFillsOfEachDay) {
    const std::string query = ChartQueries::cost_per_million(kStrategy, kPortfolio, "2026-04-25");
    EXPECT_TRUE(has(query, "lr.daily_transaction_costs")) << query;
    EXPECT_TRUE(has(query, "LEFT JOIN trading.executions e")) << query;
    EXPECT_TRUE(has(query, "e.strategy_id = lr.strategy_id")) << query;
    EXPECT_TRUE(has(query, "e.portfolio_id = lr.portfolio_id")) << query;
    EXPECT_TRUE(has(query, "e.symbol, e.quantity, e.price")) << query;
    EXPECT_FALSE(has(query, "gross_notional")) << "the held notional is not the denominator";
    EXPECT_FALSE(has(query, "COUNT(")) << "the count over every book's executions is gone";
    EXPECT_FALSE(has(query, "execution_type")) << query;
}

// A day's value: its cost over the notional TRADED that day, per million. 2026-04-24: one 6J
// contract at 0.006286 (x 12,500,000 = 78,575.00) and two ZN at 108.09375 (x 1,000 = 216,187.50),
// 294,762.50 traded, cost 39.98: 39.98 / 294,762.50 x 1,000,000 = 135.63.
TEST(ChartSeries, CostPerMillionIsTheDaysCostOverTheNotionalTradedThatDay) {
    const ChartData data =
        ChartSeries::cost_per_million({{"2026-04-23", 12.50, "MES.v.0", 2.0, 7153.5},
                                       {"2026-04-24", 39.98, "6J.v.0", 1.0, 0.006286},
                                       {"2026-04-24", 39.98, "ZN.v.0", 2.0, 108.09375}},
                                      test_multiplier);
    ASSERT_EQ(data.labels.size(), 2u);
    EXPECT_EQ(data.labels[0], "2026-04-23");
    EXPECT_EQ(data.labels[1], "2026-04-24");
    EXPECT_DOUBLE_EQ(data.values[0], 12.50 / (2.0 * 7153.5 * 5.0) * 1000000.0);
    EXPECT_DOUBLE_EQ(data.values[1], 39.98 / (78575.0 + 216187.5) * 1000000.0);
    EXPECT_NEAR(data.values[1], 135.63, 0.005);
    EXPECT_EQ(data.title, "Cost per $1M Traded (Efficiency Metric)");
    EXPECT_FALSE(data.has_reference_line);
}

// A day on which the book held positions and made no fill has no point (it used to have one: a
// carried cost of 0 over the held notional, or a day's roll cost over it).
TEST(ChartSeries, ADayTheBookDidNotTradeHasNoPointOnTheCostChart) {
    const ChartData data =
        ChartSeries::cost_per_million({{"2026-04-22", 3.80, "MES.v.0", 1.0, 7100.0},
                                       {"2026-04-23", 0.0, "", 0.0, 0.0},
                                       {"2026-04-24", 39.98, "ZN.v.0", 2.0, 108.09375},
                                       {"2026-04-25", 0.0, "", 0.0, 0.0}},
                                      test_multiplier);
    ASSERT_EQ(data.labels.size(), 2u);
    EXPECT_EQ(data.labels[0], "2026-04-22");
    EXPECT_EQ(data.labels[1], "2026-04-24");
    EXPECT_TRUE(ChartSeries::cost_per_million({{"2026-04-23", 0.0, "", 0.0, 0.0}}, test_multiplier)
                    .labels.empty());
    EXPECT_TRUE(ChartSeries::cost_per_million({}, test_multiplier).labels.empty());
}

// A fill whose symbol has no contract multiplier on record is never counted at a multiplier of 1:
// the day's ratio would be wrong, so the day has no point.
TEST(ChartSeries, ADayWithAFillOfNoKnownMultiplierHasNoPointOnTheCostChart) {
    const ChartData data =
        ChartSeries::cost_per_million({{"2026-04-22", 3.80, "MES.v.0", 1.0, 7100.0},
                                       {"2026-04-22", 3.80, "ZN.v.0", 1.0, 108.0},
                                       {"2026-04-23", 9.10, "MES.v.0", 1.0, 7153.5},
                                       {"2026-04-23", 9.10, "NOT_ON_RECORD", 3.0, 55.0},
                                       {"2026-04-24", 2.25, "AAPL", 20.0, 296.42}},
                                      test_multiplier);
    ASSERT_EQ(data.labels.size(), 2u);
    EXPECT_EQ(data.labels[0], "2026-04-22");
    EXPECT_EQ(data.labels[1], "2026-04-24");
    EXPECT_DOUBLE_EQ(data.values[1], 2.25 / (20.0 * 296.42) * 1000000.0);
    // no lookup at all: no fill can be sized
    EXPECT_TRUE(ChartSeries::cost_per_million({{"2026-04-22", 3.80, "MES.v.0", 1.0, 7100.0}}, {})
                    .labels.empty());
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

// T-8D-2 R62: the bars are sums of realized P&L with no cost taken off; the heading and the
// image's title say so (they said "(All-Time)" and "Cumulative Realized PnL by Symbol").
TEST(ChartTitles, TheCumulativeChartSaysItIsRealizedAndGrossOfCosts) {
    EXPECT_EQ(ChartTitles::cumulative_pnl_heading(),
              "Cumulative PnL by Symbol (realized, gross of costs)");
    const ChartData data =
        ChartSeries::cumulative_pnl_by_symbol({{"KE.v.0", 5200.0}}, "2026-04-24");
    EXPECT_EQ(data.title,
              "Cumulative PnL by Symbol (realized, gross of costs; all-time as of 2026-04-24)");
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

TEST(ChartFetchers, YesterdaysPnlHasOneBarForASymbolAnsweredTwice) {
    auto db = std::make_shared<AnsweringDatabase>();
    db->answer(
        "trading.positions", {"symbol", "daily_realized_pnl"},
        {{"MES.v.0", "40.00"}, {"MES.v.0", "-45.00"}, {"ZF.v.0", "NULL"}, {"KE.v.0", "1475"}});
    const ChartData data =
        ChartGenerator::fetch_pnl_by_symbol_data(db, kStrategy, kPortfolio, "2026-04-25");
    ASSERT_EQ(db->queries.size(), 1u);
    EXPECT_EQ(db->queries[0], ChartQueries::pnl_by_symbol(kStrategy, kPortfolio, "2026-04-25"));
    ASSERT_EQ(data.labels.size(), 3u);
    EXPECT_EQ(data.labels[0], "KE.v.0");
    EXPECT_EQ(data.labels[1], "ZF.v.0");
    EXPECT_EQ(data.labels[2], "MES.v.0");
    EXPECT_DOUBLE_EQ(data.values[1], 0.0);
    EXPECT_DOUBLE_EQ(data.values[2], -5.0);
}

// The cost chart end to end: the LEFT JOIN answers a day without a fill as one row of NULL fill
// columns, a day of two fills as two rows carrying the same day cost.
TEST(ChartFetchers, TheCostChartDividesEachDaysCostByTheFillsAnsweredForThatDay) {
    auto db = std::make_shared<AnsweringDatabase>();
    db->answer("trading.executions", {"date", "daily_cost", "symbol", "quantity", "price"},
               {{"2026-04-22", "3.795648", "MES.v.0", "1", "7100"},
                {"2026-04-23", "0.000000", "NULL", "NULL", "NULL"},
                {"2026-04-24", "39.98", "6J.v.0", "1", "0.006286"},
                {"2026-04-24", "39.98", "ZN.v.0", "2", "108.09375"},
                {"2026-04-25", "0", "NULL", "NULL", "NULL"}});
    const ChartData data = ChartGenerator::fetch_cumulative_transaction_costs_data(
        db, kStrategy, kPortfolio, "2026-04-25", test_multiplier);
    ASSERT_EQ(db->queries.size(), 1u);
    EXPECT_EQ(db->queries[0], ChartQueries::cost_per_million(kStrategy, kPortfolio, "2026-04-25"));
    ASSERT_EQ(data.labels.size(), 2u);
    EXPECT_EQ(data.labels[0], "2026-04-22");
    EXPECT_EQ(data.labels[1], "2026-04-24");
    EXPECT_DOUBLE_EQ(data.values[0], 3.795648 / (7100.0 * 5.0) * 1000000.0);
    EXPECT_DOUBLE_EQ(data.values[1], 39.98 / (78575.0 + 216187.5) * 1000000.0);
}

// The equity curve end to end: the rows the query answers are the points, the capital of the run
// metadata is the reference line, and both queries are the ones ChartQueries builds.
TEST(ChartFetchers, TheEquityCurvePlotsTheAnsweredRowsAgainstTheBooksCapital) {
    auto db = std::make_shared<AnsweringDatabase>();
    db->answer(
        "trading.equity_curve", {"timestamp", "equity"},
        {{"2026-06-15 00:00:00+00", "99780.0000"}, {"2026-06-12 00:00:00+00", "99540.2500"}});
    db->answer("trading.live_run_metadata", {"total_capital"}, {{"100000.0"}});
    const ChartData data = ChartGenerator::fetch_equity_curve_data(
        db, "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MR_PORTFOLIO", "2026-06-16", 30);
    ASSERT_EQ(db->queries.size(), 2u);
    EXPECT_EQ(db->queries[0], ChartQueries::equity_curve("LIVE_EQUITY_MEAN_REVERSION",
                                                         "EQUITY_MR_PORTFOLIO", "2026-06-16", 30));
    EXPECT_EQ(db->queries[1], ChartQueries::book_capital("LIVE_EQUITY_MEAN_REVERSION",
                                                         "EQUITY_MR_PORTFOLIO", "2026-06-16"));
    ASSERT_EQ(data.labels.size(), 2u);
    EXPECT_EQ(data.labels[0], "2026-06-12");
    EXPECT_EQ(data.labels[1], "2026-06-15");
    EXPECT_DOUBLE_EQ(data.values[0], 99540.25);
    EXPECT_DOUBLE_EQ(data.values[1], 99780.0);
    EXPECT_EQ(data.title, "Equity Curve through 2026-06-15 (settled)");
    EXPECT_TRUE(data.has_reference_line);
    EXPECT_DOUBLE_EQ(data.reference_line, 100000.0);
}

TEST(ChartFetchers, AnEquityCurveWhoseCapitalIsNotOnRecordIsDrawnWithNoReferenceLine) {
    auto db = std::make_shared<AnsweringDatabase>();  // the run metadata query is not answered
    db->answer("trading.equity_curve", {"timestamp", "equity"},
               {{"2026-04-23 05:00:00+00", "495994.4000"}});
    const ChartData data =
        ChartGenerator::fetch_equity_curve_data(db, kStrategy, kPortfolio, "2026-04-24", 30);
    ASSERT_EQ(data.labels.size(), 1u);
    EXPECT_EQ(data.labels[0], "2026-04-23");
    EXPECT_FALSE(data.has_reference_line);

    auto no_key = std::make_shared<AnsweringDatabase>();  // a metadata row without the key
    no_key->answer("trading.equity_curve", {"timestamp", "equity"},
                   {{"2026-04-23 05:00:00+00", "495994.4000"}});
    no_key->answer("trading.live_run_metadata", {"total_capital"}, {{"NULL"}});
    EXPECT_FALSE(
        ChartGenerator::fetch_equity_curve_data(no_key, kStrategy, kPortfolio, "2026-04-24", 30)
            .has_reference_line);
}

TEST(ChartFetchers, AQueryThatFailsIsNoChart) {
    auto db = std::make_shared<AnsweringDatabase>();  // answers nothing: every query is an error
    EXPECT_TRUE(ChartGenerator::fetch_equity_curve_data(db, kStrategy, kPortfolio, "2026-04-24", 30)
                    .labels.empty());
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
        ChartQueries::equity_curve(strategy, portfolio, "2026-04-25", 30),
        ChartQueries::book_capital(strategy, portfolio, "2026-04-25"),
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

// A book of two sleeves: the chart of the day before the run date has one bar per symbol, as many
// as the day's stored rows have distinct symbols (counted by a query of its own).
TEST_F(ChartQueriesDb, OnABookOfTwoSleevesYesterdaysPnlHasOneBarPerSymbol) {
    const std::string strategy = "LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST";
    const std::string portfolio = "BASE_PORTFOLIO";
    const std::string run_date = "2026-04-25";
    auto counted = db_->execute_query(
        "SELECT COUNT(DISTINCT symbol)::text, COUNT(*)::text FROM trading.positions "
        "WHERE strategy_id = '" +
        strategy + "' AND portfolio_id = '" + portfolio +
        "' "
        "AND portfolio_type = 'system' AND date = (SELECT MAX(date) FROM trading.positions "
        "WHERE strategy_id = '" +
        strategy + "' AND portfolio_id = '" + portfolio +
        "' "
        "AND date < DATE '" +
        run_date + "')");
    ASSERT_FALSE(counted.is_error()) << counted.error()->what();
    const auto cell = [&](int column) {
        return std::stoi(
            std::static_pointer_cast<arrow::StringArray>(counted.value()->column(column)->chunk(0))
                ->GetString(0));
    };
    const int symbols = cell(0);
    const int rows = cell(1);
    if (symbols == 0)
        GTEST_SKIP() << "the database holds no BASE position row before " << run_date;
    const ChartData data =
        ChartGenerator::fetch_pnl_by_symbol_data(db_, strategy, portfolio, run_date);
    EXPECT_EQ(static_cast<int>(data.labels.size()), symbols)
        << rows << " stored rows of " << symbols << " symbols";
}

// The cost chart has a point on each day the book made a fill and on no other day: as many points
// as the book's stored days on or before the run date that have a fill of its own (counted by a
// query of its own; every symbol is given a multiplier, the count does not depend on its value).
TEST_F(ChartQueriesDb, TheCostChartHasAPointOnEachDayTheBookTradedAndOnNoOther) {
    const std::string strategy = "LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST";
    const std::string portfolio = "BASE_PORTFOLIO";
    const std::string run_date = "2026-04-25";
    auto counted = db_->execute_query(
        "SELECT COUNT(*)::text FROM trading.live_results lr "
        "WHERE lr.strategy_id = '" +
        strategy + "' AND lr.portfolio_id = '" + portfolio +
        "' "
        "AND lr.date <= DATE '" +
        run_date +
        "' AND EXISTS (SELECT 1 FROM trading.executions e "
        "WHERE e.date = lr.date AND e.strategy_id = '" +
        strategy +
        "' "
        "AND e.portfolio_id = '" +
        portfolio + "')");
    ASSERT_FALSE(counted.is_error()) << counted.error()->what();
    const int traded_days =
        std::stoi(std::static_pointer_cast<arrow::StringArray>(counted.value()->column(0)->chunk(0))
                      ->GetString(0));
    if (traded_days < 2)
        GTEST_SKIP() << "the database holds under two traded BASE days up to " << run_date;
    const ChartData data = ChartGenerator::fetch_cumulative_transaction_costs_data(
        db_, strategy, portfolio, run_date,
        [](const std::string&) { return std::optional<double>(1.0); });
    EXPECT_EQ(static_cast<int>(data.labels.size()), traded_days);
}

// --- a tool, not a check: the charts as the report draws them, written as PNG files --------------
//
// Runs only when BOTH TRADE_NGIN_TEST_DSN and TRADE_NGIN_CHART_PNG_DIR are set (it starts gnuplot,
// which no other test does). TRADE_NGIN_CHART_PNG_BOOK names the book and the run date:
// "<strategy_id>|<portfolio_id>|<YYYY-MM-DD>|<1 for an equity book, else 0>". A report body file
// holds a chart as a heading and a reference; the image is only in a sent mail, and nothing is
// mailed from a test run. This writes the six database charts of one report, by the generator's
// own functions, so they can be looked at. The contract multipliers are read from
// metadata.contract_metadata ("Contract Size"), 1 for a symbol that is not a futures root.
TEST_F(ChartQueriesDb, WritesTheChartsOfOneReportAsPngFilesWhenAskedTo) {
    const char* dir = std::getenv("TRADE_NGIN_CHART_PNG_DIR");
    const char* book = std::getenv("TRADE_NGIN_CHART_PNG_BOOK");
    if (dir == nullptr || !*dir || book == nullptr || !*book) {
        GTEST_SKIP() << "TRADE_NGIN_CHART_PNG_DIR and TRADE_NGIN_CHART_PNG_BOOK not set";
    }
    std::vector<std::string> parts;
    std::string part;
    for (const char* c = book;; ++c) {
        if (*c == '|' || *c == 0) {
            parts.push_back(part);
            part.clear();
            if (*c == 0)
                break;
        } else {
            part += *c;
        }
    }
    ASSERT_EQ(parts.size(), 4u) << "strategy_id|portfolio_id|date|equities";
    const std::string& strategy = parts[0];
    const std::string& portfolio = parts[1];
    const std::string& date = parts[2];
    const bool equities = parts[3] == "1";

    std::unordered_map<std::string, double> sizes;
    auto metadata = db_->execute_query(
        "SELECT \"Databento Symbol\"::text, \"Contract Size\"::text "
        "FROM metadata.contract_metadata");
    ASSERT_FALSE(metadata.is_error()) << metadata.error()->what();
    const auto roots =
        std::static_pointer_cast<arrow::StringArray>(metadata.value()->column(0)->chunk(0));
    const auto contract_sizes =
        std::static_pointer_cast<arrow::StringArray>(metadata.value()->column(1)->chunk(0));
    for (int64_t i = 0; i < roots->length(); ++i) {
        if (!roots->IsNull(i) && !contract_sizes->IsNull(i))
            sizes[roots->GetString(i)] = std::stod(contract_sizes->GetString(i));
    }
    const ChartSeries::MultiplierLookup multiplier_of =
        [&](const std::string& symbol) -> std::optional<double> {
        const auto found = sizes.find(symbol.substr(0, symbol.find(".v.")));
        return found == sizes.end() ? 1.0 : found->second;
    };

    const auto write_png = [&](const std::string& name, const std::string& base64) {
        if (base64.empty()) {
            ADD_FAILURE() << name << ": no image (no data, or gnuplot failed)";
            return;
        }
        static const std::string alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string bytes;
        unsigned buffer = 0;
        int bits = 0;
        for (char c : base64) {
            const auto at = alphabet.find(c);
            if (at == std::string::npos)
                continue;  // padding
            buffer = (buffer << 6) | static_cast<unsigned>(at);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                bytes += static_cast<char>((buffer >> bits) & 0xFF);
            }
        }
        const std::string path =
            std::string(dir) + "/" + portfolio + "_" + date + "_" + name + ".png";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << bytes;
        EXPECT_TRUE(out.good()) << path;
    };

    write_png("equity_curve",
              ChartGenerator::generate_equity_curve_chart(db_, strategy, portfolio, date, 30));
    write_png("pnl_by_symbol", ChartGenerator::generate_pnl_by_symbol_chart(
                                   db_, strategy, portfolio, date, equities));
    write_png("daily_pnl",
              ChartGenerator::generate_daily_pnl_chart(db_, strategy, portfolio, date, 30));
    write_png("cost_per_million", ChartGenerator::generate_total_transaction_costs_chart(
                                      db_, strategy, portfolio, date, multiplier_of));
    write_png("margin_posted",
              ChartGenerator::generate_margin_posted_chart(db_, strategy, portfolio, date));
    write_png("cumulative_pnl", ChartGenerator::generate_cumulative_pnl_by_symbol_chart(
                                    db_, strategy, portfolio, date));
}
