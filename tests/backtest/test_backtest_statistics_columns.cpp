// tests/backtest/test_backtest_statistics_columns.cpp
//
// Migration 031, the statistics of LOOP_SPEC v6.2 sections 7.4 and 10 on backtest.results: the
// worst day with its date and symbol, the three statistics on the sizing capital, the monthly
// skew and tail ratio, the calendar-year returns with the losing years, the account's STRATEGY
// fills. One implementation with the live columns of migration 030 (live_statistics_columns),
// in FRACTIONS, on the run's statistics series after the warm-up.
//
// Every test here calls the function it names; the figures it expects are written out by hand.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/backtest/backtest_metrics_calculator.hpp"
#include "trade_ngin/backtest/backtest_statistics_columns.hpp"
#include "trade_ngin/backtest/backtest_types.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/backtest_curve_cells.hpp"
#include "trade_ngin/data/backtest_position_row.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/risk/overlay_columns.hpp"
#include "trade_ngin/live/live_statistics_columns.hpp"

using namespace trade_ngin;

namespace {

Timestamp day(const std::string& date) {
    Timestamp t;
    EXPECT_TRUE(core::parse_utc_date(date, t)) << date;
    return t;
}

// A series from dated levels: the first is the base, every later one a return row.
StatisticsSeries series_of(const std::vector<std::pair<std::string, double>>& levels) {
    StatisticsSeries s;
    s.base = levels.front().second;
    for (size_t i = 1; i < levels.size(); ++i) {
        s.dates.push_back(levels[i].first);
        s.levels.push_back(levels[i].second);
        s.returns_pct.push_back((levels[i].second / levels[i - 1].second - 1.0) * 100.0);
        s.pnl.push_back(levels[i].second - levels[i - 1].second);
    }
    return s;
}

const std::vector<std::pair<std::string, double>> kFiveDays = {
    {"2025-03-03", 500000.0},  // the base row
    {"2025-03-04", 505000.0},  // +1.0 percent
    {"2025-03-05", 489850.0},  // -3.0 percent: the worst day on the account
    {"2025-03-06", 494748.5},  // +1.0 percent
    {"2025-03-07", 484853.53}  // -2.0 percent
};

std::optional<std::string> cell_of(const std::vector<LiveResultsCell>& cells,
                                   const std::string& column) {
    for (const auto& c : cells) {
        if (c.column == column) return c.value;
    }
    ADD_FAILURE() << "no cell " << column;
    return std::nullopt;
}

ExecutionReport fill(const std::string& sleeve, const std::string& symbol, Side side, double qty,
                     const std::string& date, int seq) {
    ExecutionReport e;
    e.execution_type = ExecutionType::STRATEGY;
    e.exec_id = sleeve + "_" + symbol + "_" + date + "_" + std::to_string(seq);
    e.order_id = e.exec_id;
    e.symbol = symbol;
    e.side = side;
    e.filled_quantity = Quantity(qty);
    e.fill_price = Price(100.0);
    e.fill_time = day(date);
    e.total_transaction_costs = Decimal(1.0);
    return e;
}

}  // namespace

// The backtest cells are the live cells of the same series, a percent figure divided by 100:
// one implementation, two units.
//   | figure              | live (percent)                 | backtest (fraction) |
//   | worst_day           | min r = -3.0                   | -0.03               |
//   | volatility_sizing   | sd(r^s) x sqrt(K), r^s percent | the same / 100      |
//   | max_drawdown_sizing | points                         | the same / 100      |
//   | year return         | percent                        | the same / 100      |
TEST(BacktestStatisticsColumns, TheFiguresAreTheLiveColumnsOfTheSameSeriesInFractions) {
    const StatisticsSeries series = series_of(kFiveDays);
    BacktestStatisticsInputs in;
    in.symbol_pnl = {{"2025-03-05", "ZN.v.0", -9000.0, 0.0}, {"2025-03-05", "MES.v.0", -6000.0, 0.0},
                     {"2025-03-07", "MES.v.0", -9800.0, 0.0}};
    in.capitals = {{"2025-03-03", 500000.0}, {"2025-03-04", 500000.0}, {"2025-03-05", 500000.0},
                   {"2025-03-06", 489850.0}, {"2025-03-07", 494748.5}};
    const double k = 311.0574;
    const LiveStatisticsColumns live = live_statistics_columns(
        series, k, "2025-03-03", "2025-03-07", in.symbol_pnl, in.capitals, "2025-03-03");
    const BacktestStatisticsColumns bt =
        backtest_statistics_columns(series, k, "2025-03-03", "2025-03-07", in);

    ASSERT_TRUE(bt.worst_day);
    EXPECT_NEAR(*bt.worst_day, -0.03, 1e-12);
    EXPECT_EQ(bt.worst_day_date.value_or(""), "2025-03-05");
    EXPECT_EQ(bt.worst_day_symbol.value_or(""), "ZN.v.0");

    // On the sizing capital, by hand: r^s = P&L / E_t, never compounded.
    const std::vector<double> rs = {5000.0 / 500000.0, -15150.0 / 500000.0, 4898.5 / 489850.0,
                                    -9894.97 / 494748.5};
    double mean = 0.0, squares = 0.0;
    for (double r : rs) mean += r;
    mean /= 4.0;
    for (double r : rs) squares += (r - mean) * (r - mean);
    ASSERT_TRUE(bt.volatility_sizing);
    EXPECT_NEAR(*bt.volatility_sizing, std::sqrt(squares / 3.0) * std::sqrt(k), 1e-12);
    // running sum: 0.01, -0.0203, -0.0103, -0.0303; peak 0.01; the largest fall 0.0403
    ASSERT_TRUE(bt.max_drawdown_sizing);
    EXPECT_NEAR(*bt.max_drawdown_sizing, 0.01 - (rs[0] + rs[1] + rs[2] + rs[3]), 1e-12);
    EXPECT_NEAR(*bt.max_drawdown_sizing, 0.0403, 1e-12);
    ASSERT_TRUE(bt.worst_day_sizing);
    EXPECT_NEAR(*bt.worst_day_sizing, -0.0303, 1e-12);
    EXPECT_EQ(bt.worst_day_sizing_date.value_or(""), "2025-03-05");
    EXPECT_EQ(bt.worst_day_sizing_symbol.value_or(""), "ZN.v.0");
    EXPECT_EQ(bt.sizing_returns, 4);

    // Each is the live figure over 100, and nothing else.
    EXPECT_DOUBLE_EQ(*bt.volatility_sizing, *live.volatility_sizing / 100.0);
    EXPECT_DOUBLE_EQ(*bt.max_drawdown_sizing, *live.max_drawdown_sizing / 100.0);
    EXPECT_DOUBLE_EQ(*bt.worst_day_sizing, *live.worst_day_sizing / 100.0);
    EXPECT_EQ(bt.worst_day_date, live.worst_day_date);
    EXPECT_EQ(bt.worst_day_symbol, live.worst_day_symbol);
    EXPECT_EQ(bt.losing_years, live.losing_years);

    // The year: 484,853.53 / 500,000 - 1 = -0.03029294, partial (the run starts and ends in it).
    ASSERT_TRUE(bt.calendar_year_returns);
    const auto& y = (*bt.calendar_year_returns)["2025"];
    EXPECT_NEAR(y["return"].get<double>(), 484853.53 / 500000.0 - 1.0, 1e-12);
    EXPECT_TRUE(y["partial"].get<bool>());
    EXPECT_NEAR((*live.calendar_year_returns)["2025"]["return"].get<double>(),
                y["return"].get<double>() * 100.0, 1e-9);
    EXPECT_EQ(bt.losing_years.value_or(-1), 0) << "a partial year is shown and never counted";
    EXPECT_FALSE(bt.monthly_skew) << "no complete month";
    EXPECT_FALSE(bt.monthly_tail_ratio);
    EXPECT_FALSE(bt.total_strategy_fills) << "the caller sets it from the account fills";
}

// The base row's own P&L happened before the base level: it is in no return, so it cannot name
// the symbol of a worst day that is the first return.
//   | date       | level   | return | G0 P&L | G1 P&L |
//   | 2025-03-03 | 500,000 | base   | -40,000 | 0     |
//   | 2025-03-04 | 485,000 | -3.0%  | +1,000  | -16,000 |
//   | 2025-03-05 | 489,850 | +1.0%  | 0       | +4,850 |
TEST(BacktestStatisticsColumns, TheFirstReturnsSymbolLeavesTheBaseRowsOwnPnlOut) {
    const StatisticsSeries series =
        series_of({{"2025-03-03", 500000.0}, {"2025-03-04", 485000.0}, {"2025-03-05", 489850.0}});
    BacktestStatisticsInputs in;
    in.symbol_pnl = {{"2025-03-03", "G0.v.0", -40000.0, 0.0}, {"2025-03-04", "G0.v.0", 1000.0, 0.0},
                     {"2025-03-04", "G1.v.0", -16000.0, 0.0}, {"2025-03-05", "G1.v.0", 4850.0, 0.0}};
    const auto bt = backtest_statistics_columns(series, 311.0574, "2025-03-03", "2025-03-05", in);
    EXPECT_EQ(bt.worst_day_date.value_or(""), "2025-03-04");
    EXPECT_EQ(bt.worst_day_symbol.value_or(""), "G1.v.0");
    // The live call without a base date reads from the first stored row, as before this commit.
    const auto live = live_statistics_columns(series, 311.0574, "2025-03-03", "2025-03-05",
                                              in.symbol_pnl, {});
    EXPECT_EQ(live.worst_day_symbol.value_or(""), "G0.v.0");
}

// An equity book's rows: a realised flow and the open position's unrealised LEVEL. A symbol's
// P&L over a return is the realised flow plus the change of the level, the base row's level
// being where the first return starts.
//   | date       | AAA realised | AAA level | BBB realised | BBB level |
//   | 2025-03-03 | 0            | +500      | 0            | -100      |  the base row
//   | 2025-03-04 | 0            | +200      | -50          | 0         |  AAA -300, BBB -50 + 100 = +50
TEST(BacktestStatisticsColumns, AnEquityRowsUnrealisedLevelIsReadAsAChangeFromTheBaseRow) {
    const StatisticsSeries series = series_of({{"2025-03-03", 100000.0}, {"2025-03-04", 99750.0}});
    BacktestStatisticsInputs in;
    in.symbol_pnl = {{"2025-03-03", "AAA", 0.0, 500.0}, {"2025-03-03", "BBB", 0.0, -100.0},
                     {"2025-03-04", "AAA", 0.0, 200.0}, {"2025-03-04", "BBB", -50.0, 0.0}};
    const auto bt = backtest_statistics_columns(series, 252.0, "2025-03-03", "2025-03-04", in);
    EXPECT_EQ(bt.worst_day_symbol.value_or(""), "AAA");
    // No sizing capital on an equity run: the three statistics, with the date and symbol, NULL.
    EXPECT_FALSE(bt.max_drawdown_sizing);
    EXPECT_FALSE(bt.volatility_sizing);
    EXPECT_FALSE(bt.worst_day_sizing);
    EXPECT_FALSE(bt.worst_day_sizing_date);
    EXPECT_FALSE(bt.worst_day_sizing_symbol);
    EXPECT_EQ(bt.sizing_returns, 0);
}

// A worst day on which no symbol lost (the loss is costs alone) names no symbol.
TEST(BacktestStatisticsColumns, AWorstDayOnWhichNoSymbolLostNamesNone) {
    const StatisticsSeries series = series_of({{"2025-03-03", 500000.0}, {"2025-03-04", 499900.0}});
    BacktestStatisticsInputs in;
    in.symbol_pnl = {{"2025-03-04", "G0.v.0", 0.0, 0.0}, {"2025-03-04", "G1.v.0", 25.0, 0.0}};
    const auto bt = backtest_statistics_columns(series, 311.0574, "2025-03-03", "2025-03-04", in);
    EXPECT_EQ(bt.worst_day_date.value_or(""), "2025-03-04");
    EXPECT_FALSE(bt.worst_day_symbol);
}

// A row with no capital of its own (an all-JUNK cycle stores no risk_detail) reads the last one
// before it; the series starts at the first return whose date has a capital.
//   | date       | P&L     | capital stored | E used  | r^s      |
//   | 2025-03-04 | +5,000  | none yet       | -       | not in the series |
//   | 2025-03-05 | -15,150 | 400,000        | 400,000 | -0.037875 |
//   | 2025-03-06 | +4,898.5 | none          | 400,000 | +0.01224625 |
//   | 2025-03-07 | -9,894.97 | 250,000      | 250,000 | -0.03957988 |
TEST(BacktestStatisticsColumns, ARowWithNoCapitalOfItsOwnReadsTheLastOneBeforeIt) {
    BacktestStatisticsInputs in;
    in.capitals = {{"2025-03-05", 400000.0}, {"2025-03-07", 250000.0}};
    const auto bt = backtest_statistics_columns(series_of(kFiveDays), 311.0574, "2025-03-03",
                                                "2025-03-07", in);
    EXPECT_EQ(bt.sizing_returns, 3);
    ASSERT_TRUE(bt.worst_day_sizing);
    EXPECT_NEAR(*bt.worst_day_sizing, -9894.97 / 250000.0, 1e-12);
    EXPECT_EQ(bt.worst_day_sizing_date.value_or(""), "2025-03-07");
    // sum: -0.037875, -0.02562875, -0.06520863; the peak stays 0; the largest fall is the last
    ASSERT_TRUE(bt.max_drawdown_sizing);
    EXPECT_NEAR(*bt.max_drawdown_sizing, 15150.0 / 400000.0 - 4898.5 / 400000.0 + 9894.97 / 250000.0,
                1e-12);
    // One return on the sizing capital is no statistic.
    BacktestStatisticsInputs one;
    one.capitals = {{"2025-03-07", 250000.0}};
    const auto single = backtest_statistics_columns(series_of(kFiveDays), 311.0574, "2025-03-03",
                                                    "2025-03-07", one);
    EXPECT_EQ(single.sizing_returns, 1);
    EXPECT_FALSE(single.volatility_sizing);
    EXPECT_FALSE(single.max_drawdown_sizing);
    EXPECT_FALSE(single.worst_day_sizing);
}

// Months and years. One return a month on its last day, from a base on 2023-11-15:
//   the month of the base (2023-11) is partial; 2023-12 .. 2025-12 are 25 complete months;
//   2026-01 is complete when the run ends on 2026-01-31 and partial when it ends on 01-30.
//   2023 partial (the base is inside it); 2024 full, 2025 full; 2026 partial.
TEST(BacktestStatisticsColumns, MonthsAndYearsAreCompleteOnlyInsideTheBaseAndTheLastRow) {
    static const int kLast[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    std::vector<std::pair<std::string, double>> levels = {{"2023-11-15", 100000.0}};
    std::vector<double> monthly;  // the return of each month, a fraction
    double level = 100000.0;
    int index = 0;
    for (int year = 2023; year <= 2026; ++year) {
        for (int month = 1; month <= 12; ++month) {
            if (year == 2023 && month < 11) continue;
            if (year == 2026 && month > 1) continue;
            const int last = month == 2 && year == 2024 ? 29 : kLast[month - 1];
            // 2024 loses, 2025 gains; one large loss in 2024-06 for the lower tail. No two
            // months are equal, so no month sits on a percentile by a tie.
            double r = year == 2024 ? -0.004 - 0.001 * (index % 3) - 0.00005 * index
                                    : 0.006 + 0.002 * (index % 4) + 0.0001 * index;
            if (year == 2024 && month == 6) r = -0.05;
            ++index;
            level *= 1.0 + r;
            char date[16];
            std::snprintf(date, sizeof(date), "%04d-%02d-%02d", year, month, last);
            levels.push_back({date, level});
            if (!(year == 2023 && month == 11)) monthly.push_back(r);
        }
    }
    const StatisticsSeries series = series_of(levels);
    const auto bt = backtest_statistics_columns(series, 252.0, "2023-11-15", "2026-01-31", {});
    EXPECT_EQ(bt.complete_months, 26) << "2023-12 through 2026-01";
    ASSERT_EQ(monthly.size(), 26u);

    // The sample skewness G1 and the tail ratio of the 26 monthly returns, by hand.
    const double n = 26.0;
    double mean = 0.0, squares = 0.0, cubes = 0.0;
    for (double r : monthly) mean += r;
    mean /= n;
    for (double r : monthly) squares += (r - mean) * (r - mean);
    const double sd = std::sqrt(squares / (n - 1.0));
    for (double r : monthly) cubes += std::pow((r - mean) / sd, 3);
    ASSERT_TRUE(bt.monthly_skew);
    EXPECT_NEAR(*bt.monthly_skew, n / ((n - 1.0) * (n - 2.0)) * cubes, 1e-9);
    EXPECT_LT(*bt.monthly_skew, 0.0) << "the June loss is the long tail";
    std::vector<double> sorted = monthly;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) {
        const double pos = p * (n - 1.0);
        const int a = static_cast<int>(std::floor(pos));
        return sorted[a] + (pos - a) * (sorted[a + 1] - sorted[a]);
    };
    double up = 0.0, down = 0.0;
    int ups = 0, downs = 0;
    for (double r : monthly) {
        if (r >= pct(0.95)) { up += r; ++ups; }
        if (r <= pct(0.05)) { down += r; ++downs; }
    }
    ASSERT_TRUE(bt.monthly_tail_ratio);
    EXPECT_NEAR(*bt.monthly_tail_ratio, (up / ups) / std::abs(down / downs), 1e-9);

    ASSERT_TRUE(bt.calendar_year_returns);
    const auto& years = *bt.calendar_year_returns;
    ASSERT_EQ(years.size(), 4u);
    EXPECT_TRUE(years["2023"]["partial"].get<bool>());
    EXPECT_FALSE(years["2024"]["partial"].get<bool>());
    EXPECT_FALSE(years["2025"]["partial"].get<bool>());
    EXPECT_TRUE(years["2026"]["partial"].get<bool>());
    EXPECT_LT(years["2024"]["return"].get<double>(), 0.0);
    EXPECT_GT(years["2025"]["return"].get<double>(), 0.0);
    EXPECT_GT(years["2024"]["return"].get<double>(), -1.0) << "a fraction, not percent";
    EXPECT_LT(std::abs(years["2024"]["return"].get<double>()), 0.2);
    EXPECT_EQ(bt.losing_years.value_or(-1), 1) << "2024 alone: FULL years only";

    // The run ending one day earlier: 2026-01 is partial, 25 complete months.
    const auto shorter = backtest_statistics_columns(series, 252.0, "2023-11-15", "2026-01-30", {});
    EXPECT_EQ(shorter.complete_months, 25);
    // A base on the last day of a year: the next year begins after it and is full.
    const auto late = backtest_statistics_columns(series, 252.0, "2023-12-31", "2026-01-31", {});
    EXPECT_FALSE((*late.calendar_year_returns)["2024"]["partial"].get<bool>());
    // A base on 1 January makes its own year partial: the base is no return.
    const auto january = backtest_statistics_columns(series, 252.0, "2024-01-01", "2026-01-31", {});
    EXPECT_TRUE((*january.calendar_year_returns)["2024"]["partial"].get<bool>());
    EXPECT_EQ(january.losing_years.value_or(-1), 0);

    // Under twelve complete months: NULL, both.
    std::vector<std::pair<std::string, double>> year_only(levels.begin(), levels.begin() + 12);
    const auto thin = backtest_statistics_columns(series_of(year_only), 252.0, "2023-11-15",
                                                  year_only.back().first, {});
    EXPECT_EQ(thin.complete_months, 10);
    EXPECT_FALSE(thin.monthly_skew);
    EXPECT_FALSE(thin.monthly_tail_ratio);
}

// An empty series: no worst day, an empty object of years, zero losing years.
TEST(BacktestStatisticsColumns, AnEmptySeriesHasNoWorstDayAndAnEmptyObjectOfYears) {
    const auto bt = backtest_statistics_columns(StatisticsSeries(), 311.0574, "", "", {});
    EXPECT_FALSE(bt.worst_day);
    EXPECT_FALSE(bt.worst_day_date);
    EXPECT_FALSE(bt.worst_day_symbol);
    ASSERT_TRUE(bt.calendar_year_returns);
    EXPECT_TRUE(bt.calendar_year_returns->empty());
    EXPECT_EQ(bt.losing_years.value_or(-1), 0);
}

// E_t is read from the text the equity-curve row stores.
TEST(BacktestStatisticsColumns, TheSizingCapitalsAreTheStoredRiskDetailCells) {
    const std::vector<std::pair<Timestamp, double>> curve = {
        {day("2025-03-03"), 500000.0}, {day("2025-03-04"), 505000.0}, {day("2025-03-05"), 489850.0},
        {day("2025-03-06"), 494748.5}};
    const std::vector<std::string> detail = {
        "", R"({"sizing_capital":500000.0,"risk_requested":1.0})",
        R"({"sizing_capital":489850.12345678901,"overlay_blind":false})", R"({"binding_term":"none"})"};
    const auto capitals = stored_sizing_capitals(curve, detail);
    ASSERT_EQ(capitals.size(), 2u);
    EXPECT_EQ(capitals[0].date, "2025-03-04");
    EXPECT_DOUBLE_EQ(capitals[0].capital, 500000.0);
    EXPECT_EQ(capitals[1].date, "2025-03-05");
    EXPECT_DOUBLE_EQ(capitals[1].capital, 489850.12345678901);
    EXPECT_TRUE(stored_sizing_capitals(curve, {}).empty()) << "an equity run stores none";
}

// The cells: the migration's thirteen in its order; a number at ten significant digits (the 030
// cells' format), never std::to_string's six decimals; a field with no value is a NULL cell.
TEST(BacktestStatisticsColumns, TheCellsCarryTenSignificantDigitsAndNullWhereThereIsNoValue) {
    BacktestStatisticsColumns c;
    c.worst_day = -0.0381234567891234;
    c.worst_day_date = "2025-04-06";
    c.worst_day_symbol = "MNQ.v.0";
    c.volatility_sizing = 0.2024771085123;
    c.calendar_year_returns = nlohmann::json{{"2025", {{"return", -0.060144}, {"partial", false}}}};
    c.losing_years = 1;
    c.total_strategy_fills = 668;
    const auto cells = backtest_statistics_cells(c);
    std::vector<std::string> names, types;
    for (const auto& cell : cells) {
        names.push_back(cell.column);
        types.push_back(cell.type);
    }
    EXPECT_EQ(names, (std::vector<std::string>{
                         "worst_day", "worst_day_date", "worst_day_symbol", "max_drawdown_sizing",
                         "volatility_sizing", "worst_day_sizing", "worst_day_sizing_date",
                         "worst_day_sizing_symbol", "monthly_skew", "monthly_tail_ratio",
                         "calendar_year_returns", "losing_years", "total_strategy_fills"}));
    EXPECT_EQ(types, (std::vector<std::string>{"numeric", "date", "text", "numeric", "numeric",
                                               "numeric", "date", "text", "numeric", "numeric",
                                               "jsonb", "integer", "integer"}));
    EXPECT_EQ(cell_of(cells, "worst_day").value_or(""), "-0.03812345679");
    EXPECT_NE(cell_of(cells, "worst_day").value_or(""), std::to_string(*c.worst_day))
        << "six decimals would store -0.038123";
    EXPECT_EQ(cell_of(cells, "volatility_sizing").value_or(""), "0.2024771085");
    EXPECT_EQ(cell_of(cells, "worst_day_date").value_or(""), "2025-04-06");
    EXPECT_EQ(cell_of(cells, "worst_day_symbol").value_or(""), "MNQ.v.0");
    EXPECT_EQ(cell_of(cells, "losing_years").value_or(""), "1");
    EXPECT_EQ(cell_of(cells, "total_strategy_fills").value_or(""), "668");
    EXPECT_EQ(nlohmann::json::parse(*cell_of(cells, "calendar_year_returns")),
              *c.calendar_year_returns);
    for (const char* null_cell : {"max_drawdown_sizing", "worst_day_sizing", "worst_day_sizing_date",
                                  "worst_day_sizing_symbol", "monthly_skew", "monthly_tail_ratio"}) {
        EXPECT_FALSE(cell_of(cells, null_cell)) << null_cell;
    }
    // A number that is not finite is no value.
    c.volatility_sizing = std::nan("");
    EXPECT_FALSE(cell_of(backtest_statistics_cells(c), "volatility_sizing"));
    EXPECT_NE(backtest_statistics_log_line(c).find("worst_day_symbol=MNQ.v.0"), std::string::npos);
    EXPECT_NE(backtest_statistics_log_line(c).find("monthly_skew=null"), std::string::npos);
}

// The rows a run stores, and the per-symbol P&L read from them.
//   | sleeve | symbol | quantity | realised | unrealised | stored | why |
//   | TREND  | G0     | 3        | -1,200   | 0          | yes    | |
//   | FAST   | G0     | -1       | +400     | 0          | yes    | the book's G0: -800 |
//   | TREND  | G1     | 0        | -9,999   | 0          | no     | a futures row closed to zero is not stored |
//   | CASH   | AAA    | 0        | -76.15   | 0          | yes    | a cash book keeps the close-day row |
//   | CASH   | BBB    | 0        | 0        | 0          | no     | no quantity and no flow |
//   | CASH   | CCC    | 10       | 0        | +35.5      | yes    | |
TEST(BacktestStatisticsColumns, TheSymbolPnlIsTakenOnTheRowsTheRunStoresSummedOverSleeves) {
    EXPECT_TRUE(backtest_position_row_is_stored(3.0, 0.0, false));
    EXPECT_TRUE(backtest_position_row_is_stored(-1e-9, 0.0, false));
    EXPECT_FALSE(backtest_position_row_is_stored(0.0, -9999.0, false));
    EXPECT_FALSE(backtest_position_row_is_stored(1e-11, -9999.0, false));
    EXPECT_TRUE(backtest_position_row_is_stored(0.0, -76.15, true));
    EXPECT_FALSE(backtest_position_row_is_stored(0.0, 1e-11, true));

    auto row = [](const std::string& symbol, double qty, double realized, double unrealized) {
        Position p;
        p.symbol = symbol;
        p.quantity = Quantity(qty);
        p.realized_pnl = Decimal(realized);
        p.unrealized_pnl = Decimal(unrealized);
        return p;
    };
    std::unordered_map<std::string, std::unordered_map<std::string, Position>> books;
    books["TREND"]["G0.v.0"] = row("G0.v.0", 3.0, -1200.0, 0.0);
    books["FAST"]["G0.v.0"] = row("G0.v.0", -1.0, 400.0, 0.0);
    books["TREND"]["G1.v.0"] = row("G1.v.0", 0.0, -9999.0, 0.0);
    books["CASH"]["AAA"] = row("AAA", 0.0, -76.15, 0.0);
    books["CASH"]["BBB"] = row("BBB", 0.0, 0.0, 0.0);
    books["CASH"]["CCC"] = row("CCC", 10.0, 0.0, 35.5);
    const auto cells = backtest::BacktestCoordinator::stored_symbol_day_pnl(
        books, [](const std::string& sleeve) { return sleeve == "CASH"; }, "2025-03-05");
    ASSERT_EQ(cells.size(), 3u);
    EXPECT_EQ(cells[0].symbol, "AAA");
    EXPECT_NEAR(cells[0].realized, -76.15, 1e-9);
    EXPECT_EQ(cells[1].symbol, "CCC");
    EXPECT_NEAR(cells[1].unrealized_level, 35.5, 1e-9);
    EXPECT_EQ(cells[2].symbol, "G0.v.0");
    EXPECT_NEAR(cells[2].realized, -800.0, 1e-9);
    for (const auto& cell : cells) EXPECT_EQ(cell.date, "2025-03-05");
}

// calculate_all_metrics takes the statistics on the rows after the warm-up: the base is the
// first row after it, and the warm-up's own fall is in no figure.
//   | row | date       | level   | |
//   | 0   | 2025-02-27 | 500,000 | warm-up |
//   | 1   | 2025-02-28 | 400,000 | warm-up: -20 percent, in no statistic |
//   | 2   | 2025-03-03 | 500,000 | the base |
//   | 3.. | kFiveDays  |         | worst day 2025-03-05, -0.03 |
// total_strategy_fills is the account's fills (the trade statistics' own netting):
//   2025-03-04 G0: TREND +3, FAST -3  -> a full cross, 0 fills
//   2025-03-05 G0: TREND +2, FAST -1  -> a partial offset, 1 fill
//   2025-03-05 G1: TREND +1, FAST +1  -> one side, 1 fill
//   2025-03-06 G1: TREND -2           -> a single sleeve, 1 fill (it closes the +2: one round trip)
TEST(BacktestStatisticsColumns, CalculateAllMetricsTakesThemAfterTheWarmUpWithTheAccountsFills) {
    std::vector<std::pair<Timestamp, double>> curve = {{day("2025-02-27"), 500000.0},
                                                       {day("2025-02-28"), 400000.0}};
    for (const auto& [date, level] : kFiveDays) curve.push_back({day(date), level});
    const std::vector<ExecutionReport> executions = {
        fill("TREND", "G0.v.0", Side::BUY, 3, "2025-03-04", 1),
        fill("FAST", "G0.v.0", Side::SELL, 3, "2025-03-04", 2),
        fill("TREND", "G0.v.0", Side::BUY, 2, "2025-03-05", 3),
        fill("FAST", "G0.v.0", Side::SELL, 1, "2025-03-05", 4),
        fill("TREND", "G1.v.0", Side::BUY, 1, "2025-03-05", 5),
        fill("FAST", "G1.v.0", Side::BUY, 1, "2025-03-05", 6),
        fill("TREND", "G1.v.0", Side::SELL, 2, "2025-03-06", 7)};
    BacktestStatisticsInputs in;
    in.symbol_pnl = {{"2025-02-28", "G9.v.0", -100000.0, 0.0}, {"2025-03-05", "G1.v.0", -15150.0, 0.0}};
    in.capitals = {{"2025-03-04", 500000.0}, {"2025-03-05", 500000.0}, {"2025-03-06", 489850.0},
                   {"2025-03-07", 494748.5}};
    BacktestMetricsCalculator calc;
    const auto r = calc.calculate_all_metrics(
        curve, executions, /*warmup_days=*/2, [](const std::string&) { return 1.0; },
        BacktestStatisticsGrid::of_own_rows(252.0), in);
    const BacktestStatisticsColumns& c = r.statistics_columns;
    ASSERT_TRUE(c.worst_day);
    EXPECT_NEAR(*c.worst_day, -0.03, 1e-12) << "not the warm-up's -0.20";
    EXPECT_EQ(c.worst_day_date.value_or(""), "2025-03-05");
    EXPECT_EQ(c.worst_day_symbol.value_or(""), "G1.v.0");
    EXPECT_EQ(c.sizing_returns, 4);
    ASSERT_TRUE(c.worst_day_sizing);
    EXPECT_NEAR(*c.worst_day_sizing, -0.0303, 1e-12);
    EXPECT_NEAR((*c.calendar_year_returns)["2025"]["return"].get<double>(),
                484853.53 / 500000.0 - 1.0, 1e-12)
        << "from the base, not from the first row of the curve";
    EXPECT_EQ(c.total_strategy_fills.value_or(-1), 3);
    EXPECT_EQ(c.total_strategy_fills.value_or(-1),
              BacktestMetricsCalculator::account_fill_counts(executions).strategy_fills)
        << "the helper the live total_strategy_fills reads";
    EXPECT_EQ(r.total_trades, 1) << "the round trips are their own total";

    // On a futures grid a row dated off the grid is folded: Saturday 2025-03-08 is no return of
    // its own, so it cannot be the worst day; the statistics end at the last grid date.
    curve.push_back({day("2025-03-08"), 400000.0});
    const auto grid = BacktestStatisticsGrid::of_dates(
        311.0574, {"2025-03-03", "2025-03-04", "2025-03-05", "2025-03-06", "2025-03-07"});
    const auto folded = calc.calculate_all_metrics(
        curve, executions, 2, [](const std::string&) { return 1.0; }, grid, in);
    EXPECT_NEAR(*folded.statistics_columns.worst_day, -0.03, 1e-12);
    EXPECT_EQ(folded.statistics_columns.worst_day_date.value_or(""), "2025-03-05");
    const StatisticsSeries series = calc.statistics_series(
        std::vector<std::pair<Timestamp, double>>(curve.begin() + 2, curve.end()), grid);
    EXPECT_EQ(series.size(), 4);
    EXPECT_EQ(series.dates.back(), "2025-03-07");
}

// ===== migration 031: the six overlay cells of a backtest.equity_curve row =====

namespace {

std::vector<LiveResultsCell> six(const OverlayColumns& c) {
    return overlay_cells(c, live_results_number);
}

OverlayColumns sized_row() {
    OverlayColumns c;
    c.overlay_risk = 0.123456789123;
    c.overlay_risk_jump = 0.2469135782;
    c.overlay_risk_shock = 0.3703703673;
    c.overlay_gross_leverage = 2.345678901;
    c.overlay_net_leverage = -0.4567890123;
    c.var_95_1d = 6338.123456;
    return c;
}

}  // namespace

// The batch INSERT names the six once and writes one value a cell a row: a sized row its ten
// significant digits, a blind row its two leverages and four NULLs, a warm-up or seed row six
// NULLs. A run with no cells (the equity book) names no column.
TEST(BacktestCurveCells, EveryRowNamesTheSixAndARowWithoutAReadingWritesNull) {
    OverlayColumns blind;
    blind.overlay_gross_leverage = 1.5;
    blind.overlay_net_leverage = -1.5;
    const BacktestCurveCells cells = {six(OverlayColumns()), six(sized_row()), six(blind)};
    EXPECT_EQ(backtest_curve_cells_refusal(cells, 3), "");
    EXPECT_EQ(backtest_curve_cell_columns(cells),
              ", overlay_risk, overlay_risk_jump, overlay_risk_shock, overlay_gross_leverage, "
              "overlay_net_leverage, var_95_1d");
    const auto quote = [](const std::string& text) { return "'" + text + "'"; };
    EXPECT_EQ(backtest_curve_cell_values(cells[0], quote), ", NULL, NULL, NULL, NULL, NULL, NULL");
    EXPECT_EQ(backtest_curve_cell_values(cells[1], quote),
              ", '0.1234567891'::numeric, '0.2469135782'::numeric, '0.3703703673'::numeric, "
              "'2.345678901'::numeric, '-0.4567890123'::numeric, '6338.123456'::numeric");
    EXPECT_EQ(backtest_curve_cell_values(cells[2], quote),
              ", NULL, NULL, NULL, '1.5'::numeric, '-1.5'::numeric, NULL");
    // No cells: the statement the equity book always wrote.
    EXPECT_EQ(backtest_curve_cell_columns({}), "");
    EXPECT_EQ(backtest_curve_cells_refusal({}, 3), "");
}

// One list a point or none: any other shape would lose cells without a word, so it is refused,
// by the writer too, before a connection is looked at.
TEST(BacktestCurveCells, AListThatIsNotOneAPointOrNamesOtherColumnsIsRefused) {
    const BacktestCurveCells two = {six(sized_row()), six(sized_row())};
    EXPECT_NE(backtest_curve_cells_refusal(two, 3).find("2 cell lists for 3 equity points"),
              std::string::npos);
    BacktestCurveCells ragged = two;
    ragged[1].pop_back();
    EXPECT_NE(backtest_curve_cells_refusal(ragged, 2).find("point 1 carries 5 cells"),
              std::string::npos);
    BacktestCurveCells renamed = two;
    renamed[1][0].column = "overlay_riskx";
    EXPECT_NE(backtest_curve_cells_refusal(renamed, 2).find("point 1 names overlay_riskx"),
              std::string::npos);
    BacktestCurveCells typed = two;
    typed[0][0].type = "numeric; DROP TABLE x";
    typed[1][0].type = typed[0][0].type;
    EXPECT_NE(backtest_curve_cells_refusal(typed, 2).find("unknown cell type"), std::string::npos);

    PostgresDatabase db("host=127.0.0.1 port=1 dbname=none_never_connected");
    const std::vector<std::pair<Timestamp, double>> points = {
        {day("2025-03-03"), 500000.0}, {day("2025-03-04"), 500100.0}, {day("2025-03-05"), 500200.0}};
    const auto r = db.store_backtest_equity_curve_batch("RUN", points, "CONSERVATIVE_PORTFOLIO",
                                                        "backtest.equity_curve", {}, two);
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_ARGUMENT) << r.error()->what();
    EXPECT_NE(std::string(r.error()->what()).find("2 cell lists for 3 equity points"),
              std::string::npos)
        << r.error()->what();
    BacktestCurveCells bad_column = {six(sized_row()), six(sized_row()), six(sized_row())};
    for (auto& row : bad_column) row[0].column = "overlay_risk; DROP TABLE x";
    EXPECT_TRUE(db.store_backtest_equity_curve_batch("RUN", points, "CONSERVATIVE_PORTFOLIO",
                                                     "backtest.equity_curve", {}, bad_column)
                    .is_error())
        << "a column that is no identifier never reaches the statement";
}
