// tests/live/test_live_statistics_columns.cpp
//
// The live statistics of migration 030 (LOOP_SPEC v6.2 sections 7.4 and 10; T-8a commit (12b)):
// the worst day's symbol, the three statistics on the sizing capital, the monthly skew and tail
// ratio, the calendar years, the fill counts, and the NULL rules, as values on small fixtures
// whose arithmetic is written beside each expectation.

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "trade_ngin/backtest/backtest_metrics_calculator.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/live/live_statistics_columns.hpp"

using namespace trade_ngin;

namespace {

// A series of grid returns given directly (the levels and P&L are not read by the monthly and
// yearly statistics).
StatisticsSeries series_of(const std::vector<std::pair<std::string, double>>& dated_returns) {
    StatisticsSeries s;
    s.base = 100000.0;
    double level = s.base;
    for (const auto& [date, r] : dated_returns) {
        const double next = level * (1.0 + r / 100.0);
        s.dates.push_back(date);
        s.returns_pct.push_back(r);
        s.pnl.push_back(next - level);
        s.levels.push_back(next);
        level = next;
    }
    return s;
}

// A book that starts on 2025-01-15: January 2025 is partial (two returns), fourteen complete
// months February 2025 .. March 2026 with two returns each (the 10th and the 20th), and one
// return in April 2026.
const std::vector<std::pair<double, double>> kMonths = {
    {1.0, 0.5},  {-2.0, 1.0}, {3.0, -0.5}, {0.2, 0.3}, {-1.5, -1.0}, {2.0, 2.5},  {0.0, 1.0},
    {-0.4, -0.1}, {4.0, 2.0}, {-3.0, 0.5}, {1.0, 0.4}, {0.0, 0.0},   {1.5, 1.0}, {-1.0, -0.6}};

StatisticsSeries fourteen_month_series() {
    std::vector<std::pair<std::string, double>> rows = {{"2025-01-20", 1.0}, {"2025-01-27", -0.5}};
    int year = 2025, month = 2;
    for (const auto& [a, b] : kMonths) {
        char ym[8];
        std::snprintf(ym, sizeof(ym), "%04d-%02d", year, month);
        rows.emplace_back(std::string(ym) + "-10", a);
        rows.emplace_back(std::string(ym) + "-20", b);
        if (++month == 13) {
            month = 1;
            ++year;
        }
    }
    rows.emplace_back("2026-04-06", 0.7);
    return series_of(rows);
}

const char* const kStart = "2025-01-15";

ExecutionReport row(const std::string& id, const std::string& symbol, Side side, double quantity,
                    double price, const std::string& date, double cost, double adjustment,
                    ExecutionType type = ExecutionType::STRATEGY) {
    ExecutionReport e;
    e.exec_id = id;
    e.order_id = id;
    e.symbol = symbol;
    e.side = side;
    e.filled_quantity = Quantity(quantity);
    e.fill_price = Price(price);
    core::parse_utc_date(date, e.fill_time);
    e.total_transaction_costs = Decimal(cost);
    e.netting_adjustment = Decimal(adjustment);
    e.execution_type = type;
    return e;
}

// The symbol, or "<none>": a value test never dereferences an empty result.
std::string worst_symbol(const std::vector<SymbolDayPnl>& pnl, const std::string& after,
                         const std::string& through) {
    return most_negative_symbol(pnl, after, through).value_or("<none>");
}

}  // namespace

TEST(LiveStatisticsColumns, AMonthIsCompoundedFromItsGridReturnsAndOnlyCompleteMonthsCount) {
    const StatisticsSeries s = fourteen_month_series();
    const auto months = complete_month_returns(s, kStart, "2026-04-10");
    // January 2025 holds the start (partial), April 2026 holds the last settled date (partial).
    ASSERT_EQ(months.size(), 14u);
    EXPECT_EQ(months.front().month, "2025-02");
    EXPECT_EQ(months.back().month, "2026-03");
    // February 2025: (1 + 0.010) x (1 + 0.005) - 1 = 1.5050 percent.
    EXPECT_NEAR(months[0].return_pct, 1.505, 1e-9);
    // October 2025: 1.04 x 1.02 - 1 = 6.08 percent; November 2025: 0.97 x 1.005 - 1 = -2.515.
    EXPECT_NEAR(months[8].return_pct, 6.08, 1e-9);
    EXPECT_NEAR(months[9].return_pct, -2.515, 1e-9);

    // The last settled date is March's last day: March is complete. One day short: it is not.
    EXPECT_EQ(complete_month_returns(s, kStart, "2026-03-31").size(), 14u);
    EXPECT_EQ(complete_month_returns(s, kStart, "2026-03-30").size(), 13u);
    // A book that starts on the 1st: the start date is the base, its month is still partial.
    EXPECT_EQ(complete_month_returns(s, "2025-01-01", "2026-04-10").front().month, "2025-02");
    // No recorded start: the month of the first return is the partial one.
    EXPECT_EQ(complete_month_returns(s, "", "2026-04-10").front().month, "2025-02");
}

TEST(LiveStatisticsColumns, TheMonthlySkewIsTheAdjustedSampleSkewnessOfTheCompleteMonths) {
    const StatisticsSeries s = fourteen_month_series();
    const LiveStatisticsColumns c =
        live_statistics_columns(s, 311.0574, kStart, "2026-04-10", {}, {});
    EXPECT_EQ(c.complete_months, 14);
    // The fourteen monthly returns: 1.505, -1.02, 2.485, 0.5006, -2.485, 4.55, 1.0, -0.4996,
    // 6.08, -2.515, 1.404, 0.0, 2.515, -1.594. G1 = 14 / (13 x 12) x sum(((x - mean) / s)^3)
    // = 0.586727494245 (the plain moment ratio m3 / m2^1.5 of the same months is 0.521893625).
    ASSERT_TRUE(c.monthly_skew.has_value());
    ASSERT_TRUE(c.monthly_skew.has_value());
    EXPECT_NEAR(*c.monthly_skew, 0.586727494245, 1e-9);
}

TEST(LiveStatisticsColumns, TheTailRatioIsTheMeanAboveTheNinetyFifthOverTheMeanBelowTheFifth) {
    const StatisticsSeries s = fourteen_month_series();
    const LiveStatisticsColumns c =
        live_statistics_columns(s, 311.0574, kStart, "2026-04-10", {}, {});
    // Sorted: -2.515, -2.485, ..., 4.55, 6.08. Position of the 95th percentile = 0.95 x 13 =
    // 12.35: 4.55 + 0.35 x (6.08 - 4.55) = 5.0855; of the 5th = 0.65: -2.515 + 0.65 x 0.03 =
    // -2.4955. At or above 5.0855: {6.08}; at or below -2.4955: {-2.515}. 6.08 / 2.515.
    ASSERT_TRUE(c.monthly_tail_ratio.has_value());
    ASSERT_TRUE(c.monthly_tail_ratio.has_value());
    EXPECT_NEAR(*c.monthly_tail_ratio, 2.417495029821, 1e-9);
    EXPECT_NEAR(tail_ratio({1.0, -1.0, 3.0, -2.0}).value_or(0.0), 3.0 / 2.0, 1e-12);  // one value each tail
}

TEST(LiveStatisticsColumns, UnderTwelveCompleteMonthsTheSkewAndTheTailRatioAreNull) {
    const StatisticsSeries s = fourteen_month_series();
    // Through 2025-12-31: February .. December 2025, eleven complete months.
    const LiveStatisticsColumns eleven =
        live_statistics_columns(s, 311.0574, kStart, "2025-12-31", {}, {});
    EXPECT_EQ(eleven.complete_months, 11);
    EXPECT_FALSE(eleven.monthly_skew.has_value());
    EXPECT_FALSE(eleven.monthly_tail_ratio.has_value());
    // Through 2026-01-31: twelve.
    const LiveStatisticsColumns twelve =
        live_statistics_columns(s, 311.0574, kStart, "2026-01-31", {}, {});
    EXPECT_EQ(twelve.complete_months, 12);
    EXPECT_TRUE(twelve.monthly_skew.has_value());
    EXPECT_TRUE(twelve.monthly_tail_ratio.has_value());
    // Equal months have no skew (s = 0) and a lower tail of 0 has no ratio.
    EXPECT_FALSE(sample_skewness({1.0, 1.0, 1.0, 1.0}).has_value());
    EXPECT_FALSE(sample_skewness({1.0, 2.0}).has_value());
    EXPECT_FALSE(tail_ratio({0.0, 0.0, 1.0}).has_value());
}

TEST(LiveStatisticsColumns, CalendarYearsFlagAPartialYearAndCountOnlyFullLosingYears) {
    // Start 2023-06-15, last settled date 2026-02-10.
    const StatisticsSeries s = series_of({{"2023-07-03", 2.0},  {"2023-11-01", 1.0},
                                          {"2024-03-01", -3.0}, {"2024-09-02", 1.0},
                                          {"2025-02-03", 4.0},  {"2025-10-01", -1.0},
                                          {"2026-01-15", -0.5}});
    int losing = -1;
    const nlohmann::json years = calendar_year_returns(s, "2023-06-15", "2026-02-10", &losing);
    ASSERT_EQ(years.size(), 4u);
    // 2023: 1.02 x 1.01 - 1 = 3.02, partial (the book starts inside it).
    EXPECT_NEAR(years["2023"]["return"].get<double>(), 3.02, 1e-9);
    EXPECT_TRUE(years["2023"]["partial"].get<bool>());
    // 2024: 0.97 x 1.01 - 1 = -2.03, full and losing.
    EXPECT_NEAR(years["2024"]["return"].get<double>(), -2.03, 1e-9);
    EXPECT_FALSE(years["2024"]["partial"].get<bool>());
    // 2025: 1.04 x 0.99 - 1 = 2.96, full.
    EXPECT_NEAR(years["2025"]["return"].get<double>(), 2.96, 1e-9);
    EXPECT_FALSE(years["2025"]["partial"].get<bool>());
    // 2026: -0.5, partial: negative, shown, never counted.
    EXPECT_NEAR(years["2026"]["return"].get<double>(), -0.5, 1e-9);
    EXPECT_TRUE(years["2026"]["partial"].get<bool>());
    EXPECT_EQ(losing, 1);

    // The last settled date on 31 December makes the last year full; a start on 1 January
    // leaves the first year partial (the start date is the base, not a return).
    calendar_year_returns(s, "2023-06-15", "2026-12-31", &losing);
    EXPECT_EQ(losing, 2);
    const nlohmann::json from_new_year = calendar_year_returns(s, "2023-01-01", "2026-02-10", &losing);
    EXPECT_TRUE(from_new_year["2023"]["partial"].get<bool>());

    // The fourteen-month book: 2025 = 11.7316869528 percent, 2026 = 1.5870772763, both partial.
    const LiveStatisticsColumns c = live_statistics_columns(fourteen_month_series(), 311.0574,
                                                           kStart, "2026-04-10", {}, {});
    ASSERT_TRUE(c.calendar_year_returns.has_value());
    EXPECT_NEAR((*c.calendar_year_returns)["2025"]["return"].get<double>(), 11.7316869528, 1e-9);
    EXPECT_NEAR((*c.calendar_year_returns)["2026"]["return"].get<double>(), 1.5870772763, 1e-9);
    ASSERT_TRUE(c.losing_years.has_value());
    EXPECT_EQ(*c.losing_years, 0);
}

TEST(LiveStatisticsColumns, TheWorstDaysSymbolIsTakenOverTheRowsFoldedIntoTheGridReturn) {
    // Friday 04-10, then Sunday 04-12, whose grid return folds Saturday 04-11's row.
    // Stored levels: Thu 500,000 (the start), Fri 500,100, Sat 500,000 (-100), Sun 499,970 (-30).
    const std::vector<DatedLevel> levels = {{"2026-04-09", 500000.0}, {"2026-04-10", 500100.0},
                                            {"2026-04-11", 500000.0}, {"2026-04-12", 499970.0}};
    const StatisticsSeries s = build_statistics_series(
        levels, {"2026-04-09", "2026-04-10", "2026-04-12"}, "2026-04-09", 500000.0, "2026-04-12");
    ASSERT_EQ(s.dates.size(), 2u);
    // Per symbol: Friday AAA +100. Saturday AAA -100. Sunday AAA +30 and BBB -60. Over the
    // Sunday return's rows (Saturday and Sunday together) AAA = -70 and BBB = -60: AAA. On
    // Sunday's row alone it would be BBB.
    const std::vector<SymbolDayPnl> pnl = {{"2026-04-10", "AAA", 100.0, 0.0},
                                           {"2026-04-11", "AAA", -100.0, 0.0},
                                           {"2026-04-12", "AAA", 30.0, 0.0},
                                           {"2026-04-12", "BBB", -60.0, 0.0}};
    const LiveStatisticsColumns c =
        live_statistics_columns(s, 311.0574, "2026-04-09", "2026-04-12", pnl, {});
    ASSERT_TRUE(c.worst_day_date.has_value());
    EXPECT_EQ(*c.worst_day_date, "2026-04-12");
    ASSERT_TRUE(c.worst_day_symbol.has_value());
    EXPECT_EQ(*c.worst_day_symbol, "AAA");
    EXPECT_EQ(worst_symbol(pnl, "2026-04-11", "2026-04-12"), "BBB");

    // A symbol two sleeves hold is the book's: -40 and -30 of CCC are -70, beyond DDD's -60.
    EXPECT_EQ(worst_symbol({{"2026-04-13", "CCC", -40.0, 0.0},
                                     {"2026-04-13", "CCC", -30.0, 0.0},
                                     {"2026-04-13", "DDD", -60.0, 0.0}},
                                    "2026-04-12", "2026-04-13"),
              "CCC");
    // Equal losses: the first in text order. No symbol lost: none.
    EXPECT_EQ(worst_symbol({{"2026-04-13", "ZZZ", -5.0, 0.0}, {"2026-04-13", "MMM", -5.0, 0.0}},
                                    "", "2026-04-13"),
              "MMM");
    EXPECT_FALSE(most_negative_symbol({{"2026-04-13", "MMM", 5.0, 0.0}}, "", "2026-04-13").has_value());
    EXPECT_FALSE(most_negative_symbol({}, "", "2026-04-13").has_value());
}

TEST(LiveStatisticsColumns, AnEquityRowsUnrealisedLevelCountsByItsChangeOverTheRows) {
    // EEE: unrealised level 128 on Friday, 128 carried on Saturday and Sunday, 90 on Monday with
    // +10 realised: Monday's P&L is 10 + (90 - 128) = -28. FFF: opened Monday, level -20: -20.
    const std::vector<SymbolDayPnl> pnl = {{"2026-06-12", "EEE", 25.0, 128.0},
                                           {"2026-06-13", "EEE", 0.0, 128.0},
                                           {"2026-06-14", "EEE", 0.0, 128.0},
                                           {"2026-06-15", "EEE", 10.0, 90.0},
                                           {"2026-06-15", "FFF", 0.0, -20.0}};
    EXPECT_EQ(worst_symbol(pnl, "2026-06-12", "2026-06-15"), "EEE");
    // A symbol closed on the day: its level leaves, its realised P&L books the move: GGG held at
    // +50 unrealised on Friday and closed Monday for +30 realised made 30 - 50 = -20 that day.
    EXPECT_EQ(worst_symbol({{"2026-06-12", "GGG", 0.0, 50.0}, {"2026-06-12", "HHH", 0.0, 5.0},
                                     {"2026-06-15", "GGG", 30.0, 0.0}, {"2026-06-15", "HHH", 0.0, 0.0}},
                                    "2026-06-12", "2026-06-15"),
              "GGG");
}

TEST(LiveStatisticsColumns, TheSizingCapitalStatisticsAreTakenOnPnlOverTheRowsSizingCapital) {
    // Stored levels: Thu 04-23 500,000 (the start), Fri 499,800, Sat 499,790 (a cost of 10),
    // Sun 500,040, Mon 499,050, Tue 499,550. Grid: Thu, Fri, Sun, Mon, Tue.
    const std::vector<DatedLevel> levels = {{"2026-04-23", 500000.0}, {"2026-04-24", 499800.0},
                                            {"2026-04-25", 499790.0}, {"2026-04-26", 500040.0},
                                            {"2026-04-27", 499050.0}, {"2026-04-28", 499550.0}};
    const std::vector<std::string> grid = {"2026-04-23", "2026-04-24", "2026-04-26", "2026-04-27",
                                           "2026-04-28"};
    const StatisticsSeries s =
        build_statistics_series(levels, grid, "2026-04-23", 500000.0, "2026-04-28");
    ASSERT_EQ(s.pnl.size(), 4u);  // -200, +240 (Saturday's -10 and Sunday's +250), -990, +500
    // The sizing capital each row was sized on. Sunday's row sized nothing and carries none: its
    // return reads Saturday's.
    const std::vector<DatedCapital> capitals = {{"2026-04-24", 400000.0}, {"2026-04-25", 399800.0},
                                                {"2026-04-27", 400040.0}, {"2026-04-28", 399050.0}};
    const SizingReturns r = sizing_capital_returns(s, capitals);
    ASSERT_EQ(r.returns_pct.size(), 4u);
    EXPECT_NEAR(r.returns_pct[0], -200.0 / 400000.0 * 100.0, 1e-12);  // -0.05
    EXPECT_NEAR(r.returns_pct[1], 240.0 / 399800.0 * 100.0, 1e-12);   // 0.060030015008
    EXPECT_NEAR(r.returns_pct[2], -990.0 / 400040.0 * 100.0, 1e-12);  // -0.247475252475
    EXPECT_NEAR(r.returns_pct[3], 500.0 / 399050.0 * 100.0, 1e-12);   // 0.125297581757

    const std::vector<SymbolDayPnl> pnl = {{"2026-04-27", "XXX", -1200.0, 0.0},
                                           {"2026-04-27", "YYY", 210.0, 0.0}};
    const LiveStatisticsColumns c =
        live_statistics_columns(s, 311.0574, "2026-04-23", "2026-04-28", pnl, capitals);
    EXPECT_EQ(c.sizing_returns, 4);
    // sample sd of the four x sqrt(311.0574) = 2.878330609897.
    ASSERT_TRUE(c.volatility_sizing.has_value());
    EXPECT_NEAR(*c.volatility_sizing, 2.878330609897, 1e-9);
    // Running sum: -0.05, 0.010030, -0.237445, -0.112148; peak 0, then 0.010030: the largest
    // fall is 0.010030015008 - (-0.237445237467) = 0.247475252475 points.
    ASSERT_TRUE(c.max_drawdown_sizing.has_value());
    EXPECT_NEAR(*c.max_drawdown_sizing, 0.247475252475, 1e-9);
    ASSERT_TRUE(c.worst_day_sizing.has_value());
    EXPECT_NEAR(*c.worst_day_sizing, -0.247475252475, 1e-9);
    ASSERT_TRUE(c.worst_day_sizing_date.has_value());
    EXPECT_EQ(*c.worst_day_sizing_date, "2026-04-27");
    ASSERT_TRUE(c.worst_day_sizing_symbol.has_value());
    EXPECT_EQ(*c.worst_day_sizing_symbol, "XXX");

    // The series starts at the first grid date that has a capital: with capitals from Monday
    // there are two returns (sd of -0.247475 and 0.125298 x sqrt(K) = 4.648892744937).
    const LiveStatisticsColumns two = live_statistics_columns(
        s, 311.0574, "2026-04-23", "2026-04-28", pnl,
        {{"2026-04-27", 400040.0}, {"2026-04-28", 399050.0}});
    EXPECT_EQ(two.sizing_returns, 2);
    ASSERT_TRUE(two.volatility_sizing.has_value());
    EXPECT_NEAR(*two.volatility_sizing, 4.648892744937, 1e-9);
    ASSERT_TRUE(two.max_drawdown_sizing.has_value());
    EXPECT_NEAR(*two.max_drawdown_sizing, 0.247475252475, 1e-9);
}

TEST(LiveStatisticsColumns, FewerThanTwoSizingReturnsAndABookWithNoSizingCapitalAreNull) {
    const std::vector<DatedLevel> levels = {{"2026-04-23", 500000.0}, {"2026-04-24", 499800.0},
                                            {"2026-04-27", 499050.0}};
    const StatisticsSeries s = build_statistics_series(
        levels, {"2026-04-23", "2026-04-24", "2026-04-27"}, "2026-04-23", 500000.0, "2026-04-27");
    for (const std::vector<DatedCapital>& capitals :
         {std::vector<DatedCapital>{}, std::vector<DatedCapital>{{"2026-04-27", 400000.0}}}) {
        const LiveStatisticsColumns c =
            live_statistics_columns(s, 311.0574, "2026-04-23", "2026-04-27", {}, capitals);
        EXPECT_FALSE(c.max_drawdown_sizing.has_value());
        EXPECT_FALSE(c.volatility_sizing.has_value());
        EXPECT_FALSE(c.worst_day_sizing.has_value());
        EXPECT_FALSE(c.worst_day_sizing_date.has_value());
        EXPECT_FALSE(c.worst_day_sizing_symbol.has_value());
        // The account's worst day does not depend on them.
        ASSERT_TRUE(c.worst_day_date.has_value());
        EXPECT_EQ(*c.worst_day_date, "2026-04-27");
    }
    // An empty series: no worst day, an empty object of years, no losing year.
    const LiveStatisticsColumns none =
        live_statistics_columns(StatisticsSeries{}, 252.0, "2026-04-01", "2026-04-01", {}, {});
    EXPECT_FALSE(none.worst_day_date.has_value());
    EXPECT_FALSE(none.worst_day_symbol.has_value());
    ASSERT_TRUE(none.calendar_year_returns.has_value());
    EXPECT_TRUE(none.calendar_year_returns->empty());
    ASSERT_TRUE(none.losing_years.has_value());
    EXPECT_EQ(*none.losing_years, 0);
}

TEST(LiveStatisticsColumns, ACellWithNoValueIsNullAndTheFourteenAreInTheMigrationsOrder) {
    LiveStatisticsColumns c;
    c.worst_day_date = "2025-10-10";
    c.worst_day_symbol = "MES.v.0";
    c.worst_day_sizing = -1.0274012134567;
    c.calendar_year_returns = nlohmann::json{{"2025", {{"return", -4.5}, {"partial", true}}}};
    c.losing_years = 0;
    c.total_trades = 71;
    const std::vector<LiveResultsCell> cells = live_statistics_cells(c);
    const std::vector<std::string> names = {
        "worst_day_date", "worst_day_symbol", "max_drawdown_sizing", "volatility_sizing",
        "worst_day_sizing", "worst_day_sizing_date", "worst_day_sizing_symbol", "monthly_skew",
        "monthly_tail_ratio", "calendar_year_returns", "losing_years", "total_trades",
        "total_strategy_fills", "total_roll_fills"};
    ASSERT_EQ(cells.size(), names.size());
    for (size_t i = 0; i < names.size(); ++i) {
        EXPECT_EQ(cells[i].column, names[i]);
        EXPECT_TRUE(live_results_cell_type_known(cells[i].type)) << cells[i].type;
    }
    EXPECT_EQ(cells[0].type, "date");
    EXPECT_EQ(*cells[0].value, "2025-10-10");
    EXPECT_EQ(*cells[1].value, "MES.v.0");
    EXPECT_FALSE(cells[2].value.has_value());             // max_drawdown_sizing: NULL
    EXPECT_EQ(*cells[4].value, "-1.027401213");           // ten significant digits
    EXPECT_FALSE(cells[7].value.has_value());             // monthly_skew: NULL
    EXPECT_EQ(cells[9].type, "jsonb");
    EXPECT_EQ(*cells[9].value, "{\"2025\":{\"partial\":true,\"return\":-4.5}}");
    EXPECT_EQ(*cells[10].value, "0");
    EXPECT_EQ(*cells[11].value, "71");
    EXPECT_FALSE(cells[12].value.has_value());
    // A number that is not finite has no value.
    EXPECT_FALSE(live_results_number(std::nan("")).has_value());
    EXPECT_FALSE(live_results_cell_type_known("numeric); DROP TABLE t; --"));
}

TEST(LiveStatisticsColumns, TheFillCountsAreTheAccountsAndEqualTheTradeStatisticsCounts) {
    // Two sleeves, A and B; the stored date is the day the rows are netted on.
    const std::vector<ExecutionReport> rows = {
        // 04-20 ZF: A buys 2, B sells 2: a full cross, no fill (the costs after netting sum to 0).
        row("EX-A-0", "ZF", Side::BUY, 2, 108.0, "2026-04-20", 5.0, 5.0),
        row("EX-B-0", "ZF", Side::SELL, 2, 108.0, "2026-04-20", 5.0, 5.0),
        // 04-20 MES: A buys 3, B sells 1: a partial offset, one fill (BUY 2), opens.
        row("EX-A-1", "MES", Side::BUY, 3, 7000.0, "2026-04-20", 3.0, 1.0),
        row("EX-B-1", "MES", Side::SELL, 1, 7000.0, "2026-04-20", 1.0, 1.0),
        // 04-21 MES: A sells 1, B sells 1: one fill (SELL 2), closes: a round trip.
        row("EX-A-2", "MES", Side::SELL, 1, 7010.0, "2026-04-21", 1.0, 0.0),
        row("EX-B-2", "MES", Side::SELL, 1, 7010.0, "2026-04-21", 1.0, 0.0),
        // 04-22 6C: A buys 1: one fill, opens.
        row("EX-A-3", "6C", Side::BUY, 1, 0.72, "2026-04-22", 2.0, 0.0),
        // 04-23 6C: the two legs of a roll: two ROLL fills, no trade.
        row("EXEC_6C_1_RC", "6C", Side::SELL, 1, 0.7210, "2026-04-23", 2.0, 0.0, ExecutionType::ROLL),
        row("EXEC_6C_1_RO", "6C", Side::BUY, 1, 0.7225, "2026-04-23", 2.0, 0.0, ExecutionType::ROLL),
        // 04-24 6C: A sells 2: one fill, closes and flips: a round trip.
        row("EX-A-4", "6C", Side::SELL, 2, 0.73, "2026-04-24", 4.0, 0.0),
        // 04-27 6C: one sleeve's two rows of the day (a reversal stored as a close and an open):
        // one account fill (BUY 2), a round trip.
        row("EX-A-5", "6C", Side::BUY, 1, 0.71, "2026-04-27", 2.0, 0.0),
        row("EX-A-6", "6C", Side::BUY, 1, 0.71, "2026-04-27", 2.0, 0.0)};
    const auto counts = BacktestMetricsCalculator::account_fill_counts(rows);
    EXPECT_EQ(counts.strategy_fills, 5);  // against 10 STRATEGY rows
    EXPECT_EQ(counts.round_trips, 3);
    EXPECT_EQ(counts.roll_fills, 2);

    // One definition: the counts of the trade statistics on the same rows.
    const auto statistics = BacktestMetricsCalculator().calculate_trade_statistics(
        rows, [](const std::string&) { return 1.0; });
    EXPECT_EQ(counts.round_trips, statistics.total_trades);
    EXPECT_EQ(counts.strategy_fills, statistics.strategy_fills);
    EXPECT_EQ(counts.roll_fills, statistics.roll_fills);

    // The order of the rows changes nothing.
    std::vector<ExecutionReport> reversed(rows.rbegin(), rows.rend());
    const auto again = BacktestMetricsCalculator::account_fill_counts(reversed);
    EXPECT_EQ(again.strategy_fills, 5);
    EXPECT_EQ(again.round_trips, 3);
    EXPECT_EQ(again.roll_fills, 2);

    LiveStatisticsColumns c;
    EXPECT_TRUE(set_fill_counts(c, rows).empty());
    ASSERT_TRUE(c.total_trades.has_value());
    EXPECT_EQ(*c.total_trades, 3);
    ASSERT_TRUE(c.total_strategy_fills.has_value());
    EXPECT_EQ(*c.total_strategy_fills, 5);
    ASSERT_TRUE(c.total_roll_fills.has_value());
    EXPECT_EQ(*c.total_roll_fills, 2);

    // A count reads no entry price, so ROLL legs the dollar statistics cannot pair are still
    // counted. A live book of two sleeves stores one roll's legs under one exec id per leg, once
    // a sleeve: four legs of MBT, which the trade statistics refuse (two closing legs carry one
    // pair) and the counts take as four ROLL fills.
    std::vector<ExecutionReport> two_sleeves = {
        row("EX-A-0", "MBT", Side::BUY, 1, 77000.0, "2026-04-22", 2.0, 0.0),
        row("EX-B-0", "MBT", Side::BUY, 1, 77000.0, "2026-04-22", 2.0, 0.0),
        row("EXEC_MBT_20260427_RC", "MBT", Side::SELL, 1, 77960.0, "2026-04-28", 2.4, 0.0, ExecutionType::ROLL),
        row("EXEC_MBT_20260427_RC", "MBT", Side::SELL, 1, 77960.0, "2026-04-28", 2.4, 0.0, ExecutionType::ROLL),
        row("EXEC_MBT_20260427_RO", "MBT", Side::BUY, 1, 79070.0, "2026-04-28", 2.4, 0.0, ExecutionType::ROLL),
        row("EXEC_MBT_20260427_RO", "MBT", Side::BUY, 1, 79070.0, "2026-04-28", 2.4, 0.0, ExecutionType::ROLL),
        row("EX-A-1", "MBT", Side::SELL, 1, 80000.0, "2026-04-30", 2.0, 0.0)};
    EXPECT_THROW(BacktestMetricsCalculator().calculate_trade_statistics(
                     two_sleeves, [](const std::string&) { return 0.1; }),
                 std::runtime_error);
    EXPECT_TRUE(set_fill_counts(c, two_sleeves).empty());
    ASSERT_TRUE(c.total_roll_fills.has_value());
    EXPECT_EQ(*c.total_roll_fills, 4);
    ASSERT_TRUE(c.total_strategy_fills.has_value());
    EXPECT_EQ(*c.total_strategy_fills, 2);  // 04-22: two sleeves the same side, one fill; 04-30: one
    ASSERT_TRUE(c.total_trades.has_value());
    EXPECT_EQ(*c.total_trades, 1);          // 04-30 reduces the account's +2
}
