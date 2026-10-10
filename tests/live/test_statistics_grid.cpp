// tests/live/test_statistics_grid.cpp
//
// The series the live statistics are taken on (T-8D R1, R3, R5, R6, R27, R28, R39; T-8D-2
// R74, R76, R77): the returns of the statistics grid from the book's base, and each family of
// statistic computed on it. Small dated fixtures with the cases the rulings name: a Saturday
// row, a missing session, a start row that carries P&L, and a Sunday-to-Friday date too thin
// to be a grid row.

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "trade_ngin/live/live_historical_metrics.hpp"

using namespace trade_ngin;

namespace {

const double kFuturesK = 311.0574;

// A futures week of a book that starts flat on Sunday 2026-04-19 with 500,000.
//   Sun 04-19  the start row, flat at the initial capital: the base, not a return row
//   Mon 04-20  +1,000
//   Tue 04-21  no stored row (a missing session): carries Monday's level
//   Wed 04-22  -2,000 from Monday's level
//   Thu 04-23  a row is stored (+500) but only 5 symbols printed: NOT a grid date
//   Fri 04-24  +1,500 on Thursday's level
//   Sat 04-25  a cost-only row, -100: never a grid date
//   Sun 04-26  +400 on Saturday's level
const std::vector<DatedLevel> kStored = {
    {"2026-04-19", 500000.0}, {"2026-04-20", 501000.0}, {"2026-04-22", 499000.0},
    {"2026-04-23", 499500.0}, {"2026-04-24", 501000.0}, {"2026-04-25", 500900.0},
    {"2026-04-26", 501300.0}};
// The grid the bars give: Sunday to Friday without the thin Thursday; never the Saturday.
const std::vector<std::string> kGrid = {"2026-04-19", "2026-04-20", "2026-04-21",
                                        "2026-04-22", "2026-04-24", "2026-04-26"};

StatisticsSeries week_through(const std::string& through) {
    return build_statistics_series(kStored, kGrid, "2026-04-19", 500000.0, through);
}

}  // namespace

// ===== the series =====

TEST(StatisticsGrid, ReturnsAreTakenOnTheGridLevelsFromTheBase) {
    const auto s = week_through("2026-04-26");
    const std::vector<std::string> dates = {"2026-04-20", "2026-04-21", "2026-04-22",
                                            "2026-04-24", "2026-04-26"};
    EXPECT_EQ(s.dates, dates) << "the flat start row is the base and not a return row (R5)";
    EXPECT_DOUBLE_EQ(s.base, 500000.0);
    const std::vector<double> levels = {501000.0, 501000.0, 499000.0, 501000.0, 501300.0};
    EXPECT_EQ(s.levels, levels);
    ASSERT_EQ(s.size(), 5);
    EXPECT_DOUBLE_EQ(s.returns_pct[0], (501000.0 / 500000.0 - 1.0) * 100.0);
    EXPECT_DOUBLE_EQ(s.pnl[0], 1000.0);
}

TEST(StatisticsGrid, AMissingSessionCarriesTheLastStoredLevelAsAFlatReturn) {
    const auto s = week_through("2026-04-26");
    EXPECT_DOUBLE_EQ(s.returns_pct[1], 0.0) << "Tuesday has no stored row (R6)";
    EXPECT_DOUBLE_EQ(s.pnl[1], 0.0);
    const std::vector<std::string> carried = {"2026-04-21"};
    EXPECT_EQ(s.carried, carried);
    // Wednesday's return is taken on Monday's carried level.
    EXPECT_DOUBLE_EQ(s.returns_pct[2], (499000.0 / 501000.0 - 1.0) * 100.0);
}

TEST(StatisticsGrid, AThinDateFoldsIntoTheNextGridReturn) {
    const auto s = week_through("2026-04-26");
    // Thursday's +500 is a stored row off the grid: Friday's return is Wednesday to Friday.
    EXPECT_DOUBLE_EQ(s.pnl[3], 500.0 + 1500.0);
    EXPECT_DOUBLE_EQ(s.returns_pct[3], (501000.0 / 499000.0 - 1.0) * 100.0);
}

TEST(StatisticsGrid, ASaturdayRowFoldsIntoSundaysReturn) {
    const auto s = week_through("2026-04-26");
    // Saturday's -100 and Sunday's +400 are one grid return on Friday's level (R1).
    EXPECT_DOUBLE_EQ(s.pnl[4], -100.0 + 400.0);
    EXPECT_DOUBLE_EQ(s.returns_pct[4], (501300.0 / 501000.0 - 1.0) * 100.0);
    // Nothing is dropped: the returns chain to the cumulative return.
    double growth = 1.0;
    double pnl = 0.0;
    for (int i = 0; i < s.size(); ++i) {
        growth *= 1.0 + s.returns_pct[i] / 100.0;
        pnl += s.pnl[i];
    }
    EXPECT_NEAR(growth, 501300.0 / 500000.0, 1e-12);
    EXPECT_DOUBLE_EQ(pnl, 1300.0);
}

TEST(StatisticsGrid, ANonGridRowCarriesTheLastGridRowsSeries) {
    // R28 / R27: a series through Saturday is the series through Friday; a series through the
    // thin Thursday is the series through Wednesday.
    const auto friday = week_through("2026-04-24");
    const auto saturday = week_through("2026-04-25");
    EXPECT_EQ(saturday.dates, friday.dates);
    EXPECT_EQ(saturday.returns_pct, friday.returns_pct);
    EXPECT_EQ(saturday.size(), 4);
    EXPECT_EQ(week_through("2026-04-23").dates, week_through("2026-04-22").dates);
}

TEST(StatisticsGrid, TheSeriesStopsAtTheLastSettledRow) {
    // R39: the row of Sunday 04-26 as written reads through Saturday 04-25, so Sunday's own
    // level is not in it.
    const auto as_written = week_through("2026-04-25");
    EXPECT_EQ(as_written.dates.back(), "2026-04-24");
    EXPECT_DOUBLE_EQ(as_written.levels.back(), 501000.0);
}

TEST(StatisticsGrid, AStartRowThatCarriesPnlIsAReturnRowOnTheInitialCapital) {
    // R77 (c): BASE's start row stores 502,357.1083 on a base of 500,000.
    const std::vector<DatedLevel> stored = {{"2025-11-11", 502357.1083},
                                            {"2025-11-12", 503182.4441}};
    const auto s = build_statistics_series(stored, {"2025-11-11", "2025-11-12", "2025-11-13"},
                                           "2025-11-11", 500000.0, "2025-11-13");
    ASSERT_EQ(s.size(), 3);
    EXPECT_EQ(s.dates.front(), "2025-11-11");
    EXPECT_DOUBLE_EQ(s.base, 500000.0);
    EXPECT_DOUBLE_EQ(s.returns_pct[0], (502357.1083 / 500000.0 - 1.0) * 100.0);
    EXPECT_DOUBLE_EQ(s.pnl[0], 502357.1083 - 500000.0);
    EXPECT_DOUBLE_EQ(s.returns_pct[2], 0.0) << "11-13 has no row: carried";
}

TEST(StatisticsGrid, RowsBeforeTheStartEnterNoReturnAndNoCarry) {
    const std::vector<DatedLevel> stored = {{"2025-01-29", 494803.66}, {"2025-11-11", 500000.0},
                                            {"2025-11-12", 500500.0}};
    const auto s = build_statistics_series(stored, {"2025-01-29", "2025-11-11", "2025-11-12"},
                                           "2025-11-11", 500000.0, "2025-11-12");
    ASSERT_EQ(s.size(), 1);
    EXPECT_EQ(s.dates.front(), "2025-11-12");
    EXPECT_DOUBLE_EQ(s.pnl[0], 500.0);
}

TEST(StatisticsGrid, ABookWithNoRecordedStartStartsAtItsFirstStoredRow) {
    const std::vector<DatedLevel> stored = {{"2026-04-20", 100000.0}, {"2026-04-21", 100100.0}};
    const auto s = build_statistics_series(stored, {"2026-04-17", "2026-04-20", "2026-04-21"}, "",
                                           100000.0, "2026-04-21");
    ASSERT_EQ(s.size(), 1);
    EXPECT_EQ(s.dates.front(), "2026-04-21");
}

TEST(StatisticsGrid, AnEmptyBookOrGridGivesAnEmptySeries) {
    EXPECT_EQ(build_statistics_series({}, kGrid, "", 500000.0, "2026-04-26").size(), 0);
    EXPECT_EQ(build_statistics_series(kStored, {}, "2026-04-19", 500000.0, "2026-04-26").size(), 0);
    // Through the start date itself: the base only.
    EXPECT_EQ(week_through("2026-04-19").size(), 0);
}

// ===== an equity book's grid: the walk with its calendar =====

TEST(StatisticsGrid, SessionDatesWalkTheCalendarWithThePredicate) {
    // 2026-06-15 Monday .. 06-22 Monday; 06-19 (Juneteenth) is a holiday.
    const auto dates = statistics_session_dates(
        "2026-06-15", "2026-06-22", [](const std::string& date, int weekday) {
            return weekday != 0 && weekday != 6 && date != "2026-06-19";
        });
    const std::vector<std::string> want = {"2026-06-15", "2026-06-16", "2026-06-17", "2026-06-18",
                                           "2026-06-22"};
    EXPECT_EQ(dates, want);
    EXPECT_TRUE(statistics_session_dates("2026-06-22", "2026-06-15",
                                         [](const std::string&, int) { return true; })
                    .empty());
    EXPECT_TRUE(statistics_session_dates("not a date", "2026-06-15",
                                         [](const std::string&, int) { return true; })
                    .empty());
}

// ===== (5a) returns and days =====

TEST(StatisticsGrid, TotalDaysIsTheCountOfSettledGridReturns) {
    LiveHistoricalMetricsCalculator calc;
    // Seven stored rows; five grid returns. The legacy row series is deliberately longer.
    const std::vector<double> rows(7, 0.1);
    const auto m = calc.calculate(rows, rows, 0, week_through("2026-04-26"), kFuturesK);
    EXPECT_EQ(m.total_days, 5);
    // The Saturday row and the row as written carry Friday's n (R28, R39).
    EXPECT_EQ(calc.calculate(rows, rows, 0, week_through("2026-04-25"), kFuturesK)
                  .total_days,
              4);
    EXPECT_EQ(historical_metrics_int_columns(m).at("total_days"), 5);
}

TEST(StatisticsGrid, AnnualizedReturnIsGeometricOnTheGridForEveryN) {
    LiveHistoricalMetricsCalculator calc;
    const auto s = week_through("2026-04-26");
    const auto m = calc.calculate({}, {}, 0, s, kFuturesK);
    // R = 501,300 / 500,000 - 1 = 0.0026; n = 5; K = 311.0574.
    const double want = (std::pow(1.0026, 311.0574 / 5.0) - 1.0) * 100.0;
    EXPECT_NEAR(m.total_annualized_return, want, 1e-9);
    EXPECT_NEAR(m.total_annualized_return, 17.532, 5e-3);
    EXPECT_DOUBLE_EQ(grid_annualized_return_pct(s, kFuturesK), m.total_annualized_return);

    // No NULL and no special case at a short n (T-8D-2 R74): n = 1 is the formula too.
    const auto one = week_through("2026-04-20");
    ASSERT_EQ(one.size(), 1);
    EXPECT_NEAR(grid_annualized_return_pct(one, kFuturesK),
                (std::pow(1.002, 311.0574) - 1.0) * 100.0, 1e-9);
    // An equity series of the same levels annualises with ITS grid's sessions a year.
    EXPECT_NEAR(grid_annualized_return_pct(s, 252.0), (std::pow(1.0026, 252.0 / 5.0) - 1.0) * 100.0,
                1e-9);
    // Nothing to annualise.
    EXPECT_DOUBLE_EQ(grid_annualized_return_pct(week_through("2026-04-19"), kFuturesK), 0.0);
    EXPECT_DOUBLE_EQ(grid_annualized_return_pct(StatisticsSeries{}, kFuturesK), 0.0);
}

TEST(StatisticsGrid, AnnualizedReturnOfAStartRowWithPnlIsMeasuredFromTheInitialCapital) {
    const std::vector<DatedLevel> stored = {{"2025-11-11", 502357.1083}};
    const auto s =
        build_statistics_series(stored, {"2025-11-11"}, "2025-11-11", 500000.0, "2025-11-11");
    EXPECT_NEAR(grid_annualized_return_pct(s, kFuturesK),
                (std::pow(502357.1083 / 500000.0, 311.0574) - 1.0) * 100.0, 1e-6);
}

TEST(StatisticsGrid, TheDaysWarningFiresWhenTheCountsDoNotAddUpAndNamesCarriedSessions) {
    HistoricalMetrics m;
    m.winning_days = 3;
    m.losing_days = 1;
    m.flat_days = 1;
    m.total_days = 5;
    StatisticsSeries complete;
    EXPECT_EQ(statistics_days_warning(m, complete), "");

    m.total_days = 4;
    EXPECT_EQ(statistics_days_warning(m, complete),
              "winning_days + losing_days + flat days = 3 + 1 + 1 = 5 differs from total_days = 4");

    m.total_days = 5;
    const auto s = week_through("2026-04-26");
    EXPECT_EQ(statistics_days_warning(m, s),
              "1 session(s) of the statistics grid have no stored row and carry the last stored "
              "level (first 2026-04-21, last 2026-04-21)");
}

// ===== (5b) Sharpe, volatility, and with them the downside deviation and Sortino =====

TEST(StatisticsGrid, VolatilityAndSharpeAreTakenOnTheGridReturnsWithItsSessionsAYear) {
    LiveHistoricalMetricsCalculator calc;
    const auto s = week_through("2026-04-26");
    // The stored-row series handed in is deliberately different: seven rows of +9 percent.
    const std::vector<double> rows(7, 9.0);
    const auto m = calc.calculate(rows, {}, 0, s, kFuturesK);

    // The five grid returns in percent: 0.2, 0, -0.399201..., 0.400801..., 0.059880...
    double sum = 0.0;
    for (double r : s.returns_pct) sum += r;
    const double mean = sum / 5.0;
    double squares = 0.0;
    double downside = 0.0;
    for (double r : s.returns_pct) {
        squares += (r - mean) * (r - mean);
        if (r < 0.0) downside += r * r;
    }
    const double sd = std::sqrt(squares / 4.0);  // the sample divisor, n - 1
    EXPECT_NEAR(m.volatility, sd * std::sqrt(311.0574), 1e-9);
    EXPECT_NEAR(m.volatility, 5.216409, 1e-5);
    EXPECT_NEAR(m.sharpe_ratio, mean / sd * std::sqrt(311.0574), 1e-9);
    EXPECT_NEAR(m.sharpe_ratio, 3.118443, 1e-5);
    // Downside: the one losing grid return squared, over ALL five grid returns.
    const double dd = std::sqrt(downside / 5.0) * std::sqrt(311.0574);
    EXPECT_NEAR(m.downside_deviation, dd, 1e-9);
    EXPECT_NEAR(m.downside_deviation, 3.148673, 1e-5);
    EXPECT_NEAR(m.sortino_ratio, mean * 311.0574 / dd, 1e-9);
    EXPECT_NEAR(m.sortino_ratio, 5.166327, 1e-5);
}

TEST(StatisticsGrid, TheSaturdayAndTheThinDateAreNotObservationsOfTheVolatility) {
    LiveHistoricalMetricsCalculator calc;
    // A series through Saturday is Friday's (R28): same volatility, Sharpe and Sortino.
    const auto friday = calc.calculate({}, {}, 0, week_through("2026-04-24"), kFuturesK);
    const auto saturday = calc.calculate({}, {}, 0, week_through("2026-04-25"), kFuturesK);
    EXPECT_DOUBLE_EQ(saturday.volatility, friday.volatility);
    EXPECT_DOUBLE_EQ(saturday.sharpe_ratio, friday.sharpe_ratio);
    EXPECT_DOUBLE_EQ(saturday.sortino_ratio, friday.sortino_ratio);
    EXPECT_DOUBLE_EQ(saturday.downside_deviation, friday.downside_deviation);
    // Sunday's observation holds Saturday's P&L: the series through Sunday has five returns.
    const auto sunday = calc.calculate({}, {}, 0, week_through("2026-04-26"), kFuturesK);
    EXPECT_NE(sunday.volatility, friday.volatility);
}

TEST(StatisticsGrid, AnEquitySeriesIsAnnualisedWithTwoHundredFiftyTwo) {
    LiveHistoricalMetricsCalculator calc;
    const auto s = week_through("2026-04-26");
    const auto fut = calc.calculate({}, {}, 0, s, kFuturesK);
    const auto eq = calc.calculate({}, {}, 0, s, 252.0);
    // One K per series, in every annualised figure of it.
    EXPECT_NEAR(eq.volatility / fut.volatility, std::sqrt(252.0 / 311.0574), 1e-12);
    EXPECT_NEAR(eq.sharpe_ratio / fut.sharpe_ratio, std::sqrt(252.0 / 311.0574), 1e-12);
    EXPECT_NEAR(eq.downside_deviation / fut.downside_deviation, std::sqrt(252.0 / 311.0574),
                1e-12);
    EXPECT_NEAR(eq.sortino_ratio / fut.sortino_ratio, std::sqrt(252.0 / 311.0574), 1e-12);
}

TEST(StatisticsGrid, FewerThanTwoGridReturnsGiveNoVolatilityAndNoRatio) {
    LiveHistoricalMetricsCalculator calc;
    const auto one = calc.calculate({1.0, 2.0, 3.0}, {}, 0, week_through("2026-04-20"), kFuturesK);
    EXPECT_DOUBLE_EQ(one.volatility, 0.0);
    EXPECT_DOUBLE_EQ(one.sharpe_ratio, 0.0);
    EXPECT_DOUBLE_EQ(one.downside_deviation, 0.0);
    EXPECT_DOUBLE_EQ(one.sortino_ratio, 0.0);
}

// ===== (5c) the drawdown =====

TEST(StatisticsGrid, MaxDrawdownIsTakenOnEveryGridLevelFromTheBase) {
    LiveHistoricalMetricsCalculator calc;
    // Grid levels 501,000 / 501,000 / 499,000 / 501,000 / 501,300 on a base of 500,000: the
    // peak 501,000 (Monday) to 499,000 (Wednesday) is 2,000 / 501,000 = 0.399201 percent.
    const auto m = calc.calculate({}, {}, 0, week_through("2026-04-26"), kFuturesK);
    EXPECT_NEAR(m.max_drawdown, 2000.0 / 501000.0 * 100.0, 1e-9);
    EXPECT_NEAR(m.max_drawdown, 0.399202, 1e-6);
    // Through Monday there is one level above the base: no drawdown yet.
    EXPECT_DOUBLE_EQ(calc.calculate({}, {}, 0, week_through("2026-04-20"), kFuturesK).max_drawdown,
                     0.0);
    // The Saturday level 500,900 is off the grid: the series through Saturday is Friday's.
    EXPECT_DOUBLE_EQ(calc.calculate({}, {}, 0, week_through("2026-04-25"), kFuturesK).max_drawdown,
                     calc.calculate({}, {}, 0, week_through("2026-04-24"), kFuturesK).max_drawdown);
}

TEST(StatisticsGrid, TheDrawdownPeakIsSeededAtTheBookEquityAtItsStart) {
    LiveHistoricalMetricsCalculator calc;
    // A book of 500,000 whose first grid level is 498,000 and which then recovers: the fall from
    // the base is its drawdown, 0.4 percent. Seeded at the first level it would read 0.
    const std::vector<DatedLevel> stored = {{"2026-04-19", 500000.0}, {"2026-04-20", 498000.0},
                                            {"2026-04-21", 499000.0}};
    const auto s = build_statistics_series(stored, {"2026-04-19", "2026-04-20", "2026-04-21"},
                                           "2026-04-19", 500000.0, "2026-04-21");
    EXPECT_NEAR(calc.calculate({}, {}, 0, s, kFuturesK).max_drawdown, 0.4, 1e-9);

    // A start row that carries a loss (R77 (c)): the base is still the initial capital.
    const std::vector<DatedLevel> losing_start = {{"2025-11-11", 495000.0},
                                                  {"2025-11-12", 496000.0}};
    const auto t = build_statistics_series(losing_start, {"2025-11-11", "2025-11-12"},
                                           "2025-11-11", 500000.0, "2025-11-12");
    EXPECT_NEAR(calc.calculate({}, {}, 0, t, kFuturesK).max_drawdown, 1.0, 1e-9);
}

TEST(StatisticsGrid, TheStoredRowHistoriesDoNotReachTheDrawdown) {
    LiveHistoricalMetricsCalculator calc;
    const auto s = week_through("2026-04-26");
    const auto quiet = calc.calculate({}, {}, 0, s, kFuturesK);
    const auto noisy = calc.calculate({-50.0, 40.0}, {-250000.0, 100000.0}, 0, s, kFuturesK);
    EXPECT_DOUBLE_EQ(noisy.max_drawdown, quiet.max_drawdown);
}
