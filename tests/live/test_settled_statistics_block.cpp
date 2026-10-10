// tests/live/test_settled_statistics_block.cpp
//
// T-8D R39 and T-8D-2 R42 -- when the statistic columns of trading.live_results are computed
// and which rows receive them.
//
// R39: the row a run writes for its own date carries the statistics through the last settled
// row, Day T-1. R42: the Day T-1 row's statistics are refreshed whenever that row exists, not
// only when STEP 4 finalizes its levels, and that UPDATE reports a row it did not change.
//
// The three runners are `main()`s and cannot be linked into this binary, so what is tested is
// the structure in the source, the approach tests/live/test_day_t_write_ordering.cpp takes for
// the same reason: one statistics block per runner, outside STEP 4's skip, feeding both writes.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path find_repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_source(const std::string& relative) {
    auto path = find_repo_file(relative);
    if (path.empty()) return {};
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

size_t count_of(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

const std::vector<std::string> kRunners = {"apps/strategies/live_portfolio_conservative.cpp",
                                           "apps/strategies/live_portfolio.cpp",
                                           "apps/strategies/live_equity_mean_reversion.cpp"};

const char* const kStep4 = "STEP 4: UPDATE Day T-1 live_results AND equity_curve";
const char* const kSkipElse = "Skipping Day T-1 update (first trading day";
const char* const kBlock = "STEP 4b: THE STATISTICS THROUGH THE LAST SETTLED ROW (Day T-1)";
const char* const kStep5 = "STEP 5: LOAD UPDATED PREVIOUS DAY AGGREGATES";

// The statistics block of a runner: from its banner to STEP 5's.
std::string statistics_block(const std::string& src) {
    const auto begin = src.find(kBlock);
    const auto end = src.find(kStep5);
    if (begin == std::string::npos || end == std::string::npos || end < begin) return {};
    return src.substr(begin, end - begin);
}

}  // namespace

TEST(SettledStatisticsBlock, TheStatisticsAreComputedOnceAfterStepFourAndOutsideItsSkip) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

        const auto step4 = src.find(kStep4);
        const auto skip_else = src.find(kSkipElse);
        const auto block = src.find(kBlock);
        const auto step5 = src.find(kStep5);
        ASSERT_NE(step4, std::string::npos) << runner;
        ASSERT_NE(skip_else, std::string::npos) << runner;
        ASSERT_NE(step5, std::string::npos) << runner;
        ASSERT_NE(block, std::string::npos)
            << runner << ": no statistics block between STEP 4 and STEP 5";
        EXPECT_EQ(count_of(src, kBlock), 1u) << runner;

        EXPECT_GT(block, skip_else)
            << runner << ": the statistics block sits inside STEP 4's skip, so a Day T-1 whose "
                         "levels were not finalized never has its statistics refreshed (R42)";
        EXPECT_LT(block, step5) << runner;

        // Nothing inside STEP 4's conditional writes a statistic column.
        const std::string finalize = src.substr(step4, skip_else - step4);
        EXPECT_EQ(count_of(finalize, "historical_metrics_update_columns("), 0u) << runner;
        EXPECT_EQ(count_of(finalize, "hist_calc.calculate("), 0u) << runner;

        // One computation in the whole runner, and it is the block's.
        EXPECT_EQ(count_of(src, "hist_calc.calculate("), 1u)
            << runner << ": the statistics are computed more than once, so the row a run "
                         "writes and the Day T-1 row it refreshes can disagree (R39)";
        EXPECT_EQ(count_of(statistics_block(src), "hist_calc.calculate("), 1u) << runner;
    }
}

TEST(SettledStatisticsBlock, TheRowARunWritesTakesTheStatisticsThroughTheLastSettledRow) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

        const auto step5 = src.find(kStep5);
        ASSERT_NE(step5, std::string::npos) << runner;
        const std::string day_t = src.substr(step5);

        EXPECT_NE(day_t.find("const HistoricalMetrics& historical_metrics = settled_statistics;"),
                  std::string::npos)
            << runner << ": today's row does not take the block's statistics (R39)";
        // Today's own return, P&L and level are not appended to any statistics history.
        EXPECT_EQ(count_of(day_t, "returns_hist.push_back("), 0u) << runner;
        EXPECT_EQ(count_of(day_t, "load_daily_returns_history("), 0u) << runner;
        // The same column helpers still fill the INSERT.
        EXPECT_EQ(count_of(day_t, "historical_metrics_double_columns(historical_metrics)"), 1u)
            << runner;
        EXPECT_EQ(count_of(day_t, "historical_metrics_int_columns(historical_metrics)"), 1u)
            << runner;
    }
}

TEST(SettledStatisticsBlock, TheDayBeforeRefreshRunsWhenTheRowExistsAndReportsZeroRows) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

        const std::string block = statistics_block(src);
        ASSERT_FALSE(block.empty()) << runner << ": no statistics block";

        const auto exists = block.find("if (t1_row.is_ok()) {");
        const auto update = block.find("->update_live_results(");
        ASSERT_NE(exists, std::string::npos)
            << runner << ": the refresh is not conditioned on the Day T-1 row existing";
        ASSERT_NE(update, std::string::npos) << runner;
        EXPECT_GT(update, exists) << runner;
        EXPECT_NE(block.find("previous_date, metric_updates, &refreshed_rows"),
                  std::string::npos)
            << runner << ": the refresh does not read the rows it changed";
        const auto zero = block.find("} else if (refreshed_rows == 0) {");
        ASSERT_NE(zero, std::string::npos) << runner;
        EXPECT_NE(block.find("ERROR(\"Day T-1 statistics UPDATE matched 0 rows for \"", zero),
                  std::string::npos)
            << runner << ": an UPDATE that changed no row must be an ERROR";

        // No level column is written by the block.
        for (const char* level : {"current_portfolio_value =", "daily_pnl =", "daily_return =",
                                  "total_cumulative_return ="}) {
            EXPECT_EQ(block.find(level), std::string::npos) << runner << " writes " << level;
        }
    }
}

TEST(SettledStatisticsBlock, TheTwoFuturesRunnersCarryTheSameBlock) {
    const std::string conservative = read_source(kRunners[0]);
    const std::string base = read_source(kRunners[1]);
    if (conservative.empty() || base.empty()) {
        GTEST_SKIP() << "runner source not found from the test working directory";
    }
    const std::string a = statistics_block(conservative);
    const std::string b = statistics_block(base);
    ASSERT_FALSE(a.empty());
    EXPECT_EQ(a, b) << "the statistics block differs between the two futures runners";
}

// ──────────────────────────────────────────────────────────────────────────
// T-8a (5a): the block takes its returns and days from the statistics grid. total_days is the
// grid's n and total_annualized_return is a statistic of the same series, written by the
// block to both rows and no longer by the level finalize (T-8D R5, R28, R39; T-8D-2 R74, R76).
// ──────────────────────────────────────────────────────────────────────────
TEST(SettledStatisticsBlock, TheBlockBuildsTheStatisticsSeriesOnItsOwnGridAndSessionsAYear) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const std::string block = statistics_block(src);
        ASSERT_FALSE(block.empty()) << runner;
        const bool equity = runner.find("equity") != std::string::npos;

        EXPECT_EQ(count_of(block, "build_statistics_series("), 1u) << runner;
        EXPECT_EQ(count_of(block, "data_loader->load_statistics_levels("), 1u) << runner;
        EXPECT_EQ(count_of(block, "data_loader->load_book_start("), 1u) << runner;
        EXPECT_NE(block.find("initial_capital,\n                    t1_date_str);"),
                  std::string::npos)
            << runner << ": the series is not built on the initial capital through Day T-1";
        if (equity) {
            // NYSE sessions by the calendar the runner already holds; never the futures count.
            EXPECT_EQ(count_of(block, "statistics_session_dates("), 1u) << runner;
            EXPECT_NE(block.find("!holiday_checker.is_holiday(date)"), std::string::npos) << runner;
            EXPECT_EQ(count_of(block, "load_futures_statistics_grid("), 0u) << runner;
            EXPECT_NE(block.find("app_config.statistics.equity_sessions_per_year"),
                      std::string::npos)
                << runner;
        } else {
            // From the bars, over the runner's universe; no classifier verdict in the block.
            EXPECT_EQ(count_of(block, "data_loader->load_futures_statistics_grid(\n"
                                      "                    symbols, "),
                      1u)
                << runner;
            EXPECT_EQ(block.find("t1_classification"), std::string::npos) << runner;
            EXPECT_EQ(block.find("session_classifier"), std::string::npos) << runner;
            EXPECT_NE(block.find("app_config.statistics.futures_sessions_per_year"),
                      std::string::npos)
                << runner;
        }
        EXPECT_NE(block.find("statistics_series, sessions_per_year"), std::string::npos)
            << runner << ": the calculator is not handed the series and its sessions a year";
        EXPECT_EQ(count_of(block, "INFO(\"STATISTICS_CONVENTION series="), 1u) << runner;
        EXPECT_NE(block.find("statistics_days_warning(settled_statistics, statistics_series)"),
                  std::string::npos)
            << runner;
    }
}

TEST(SettledStatisticsBlock, TheAnnualizedReturnIsAStatisticOfTheBlockOnBothRows) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const std::string block = statistics_block(src);
        ASSERT_FALSE(block.empty()) << runner;

        EXPECT_NE(block.find("metric_updates[\"total_annualized_return\"] =\n"
                             "                        settled_statistics.total_annualized_return;"),
                  std::string::npos)
            << runner << ": the Day T-1 refresh does not write the grid's annualised return";

        // The level finalize no longer writes it, and nothing queries a calendar count for T.
        const auto step4 = src.find(kStep4);
        const auto skip_else = src.find(kSkipElse);
        ASSERT_NE(step4, std::string::npos);
        ASSERT_NE(skip_else, std::string::npos);
        EXPECT_EQ(src.substr(step4, skip_else - step4).find("\"total_annualized_return = \""),
                  std::string::npos)
            << runner;
        const std::string day_t = src.substr(src.find(kStep5));
        EXPECT_NE(day_t.find("double total_return_annualized = "
                             "settled_statistics.total_annualized_return;"),
                  std::string::npos)
            << runner << ": today's row does not carry the settled annualised return (R39)";
        EXPECT_EQ(day_t.find("trading.get_trading_days("), std::string::npos) << runner;
        EXPECT_EQ(day_t.find("calculate_annualized_return("), std::string::npos) << runner;
    }
}

TEST(SettledStatisticsBlock, TheConfigTemplateRecordsTheFrozenSessionsAYear) {
    const std::string tpl = read_source("config_template/defaults.json");
    if (tpl.empty()) GTEST_SKIP() << "config template not found from the test working directory";
    EXPECT_NE(tpl.find("\"sessions_per_year\": {\"futures\": 311.0574, \"equities\": 252}"),
              std::string::npos);
    for (const char* recorded : {"3111", "3653", "365.25", "2016-01-01..2025-12-31", "UTC date",
                                 "at least 9 distinct symbols"}) {
        EXPECT_NE(tpl.find(recorded), std::string::npos) << recorded;
    }
}

// T-8a (5b): the Sharpe family is taken on the grid, so no runner computes or carries a
// calendar-day annualised return any more: not in the level finalize, not in the block.
TEST(SettledStatisticsBlock, NoRunnerAnnualisesOnCalendarDaysForTheRatios) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(count_of(src, "calculate_annualized_return("), 0u)
            << runner << ": a 252-over-calendar-days annualised return is still computed";
        EXPECT_EQ(count_of(src, "finalized_t1_annualized_return"), 0u) << runner;
        const std::string block = statistics_block(src);
        ASSERT_FALSE(block.empty()) << runner;
        EXPECT_EQ(block.find("annualized_return, total_trades_hist"), std::string::npos)
            << runner << ": the calculator is still handed an annualised return";
    }
}

// T-8a (5c): the drawdown is taken on the grid levels of the statistics series, so no runner
// reads the equity-curve table for a statistic any more.
TEST(SettledStatisticsBlock, NoRunnerReadsTheEquityCurveTableForADrawdown) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(count_of(src, "load_equity_curve_history("), 0u) << runner;
        const std::string block = statistics_block(src);
        ASSERT_FALSE(block.empty()) << runner;
        EXPECT_EQ(block.find("equity_hist"), std::string::npos)
            << runner << ": the calculator is still handed an equity-curve history";
    }
}

// T-8a (5d): win_rate is W / (W + L) on the grid, so the calendar-count override is gone from
// every runner; the calendar count is only logged beside n.
TEST(SettledStatisticsBlock, NoRunnerOverridesTheWinRateWithACalendarCount) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(count_of(src, "apply_trading_days_override("), 0u) << runner;
        const std::string block = statistics_block(src);
        ASSERT_FALSE(block.empty()) << runner;
        EXPECT_NE(block.find("\" calendar_days=\" +\n"
                             "                     std::to_string(t1_trading_days_count));"),
                  std::string::npos)
            << runner << ": the calendar count is no longer logged beside n";
    }
    const std::string helper = read_source("include/trade_ngin/live/live_historical_metrics.hpp");
    if (!helper.empty()) {
        EXPECT_EQ(count_of(helper, "apply_trading_days_override"), 0u);
    }
}

// T-8a (5e): every statistic is taken on the statistics series, so no runner loads a history of
// stored daily returns or daily P&L for a statistic, and the calculator is handed the series,
// its sessions a year and the executions count only.
TEST(SettledStatisticsBlock, NoRunnerLoadsAStoredReturnOrPnlHistoryForAStatistic) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(count_of(src, "load_daily_returns_history("), 0u) << runner;
        EXPECT_EQ(count_of(src, "load_daily_pnl_history("), 0u) << runner;
        const std::string block = statistics_block(src);
        ASSERT_FALSE(block.empty()) << runner;
        EXPECT_NE(block.find("hist_calc.calculate(statistics_series, sessions_per_year, "
                             "total_trades_hist);"),
                  std::string::npos)
            << runner;
    }
}

// Migration 030 (LOOP_SPEC sections 7.4 and 10): the new statistics are taken in the same block,
// on the same series and K, once, and the same cells feed the Day T-1 refresh and the row the
// run writes. The sizing-capital history is read on the futures books only.
TEST(SettledStatisticsBlock, TheMigration030StatisticsAreTakenOnTheBlocksSeriesAndFeedBothWrites) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const std::string block = statistics_block(src);
        ASSERT_FALSE(block.empty()) << runner;
        const bool equity = runner.find("equity") != std::string::npos;

        EXPECT_EQ(count_of(src, "live_statistics_columns("), 1u) << runner;
        EXPECT_EQ(count_of(block, "live_statistics_columns("), 1u) << runner;
        EXPECT_NE(block.find("statistics_series, sessions_per_year, book_start_res.value(), t1_date_str,"),
                  std::string::npos)
            << runner << ": not the block's series, K, start and last settled date";
        EXPECT_EQ(count_of(block, "set_fill_counts(statistics_columns, book_executions_res.value())"), 1u)
            << runner;
        EXPECT_EQ(count_of(block, "->load_symbol_pnl_history("), 1u) << runner;
        EXPECT_EQ(count_of(block, "->load_book_executions("), 1u) << runner;
        EXPECT_EQ(count_of(src, "->load_sizing_capital_history("), equity ? 0u : 1u) << runner;

        // Both writes: the refresh inside the block, the day's row after STEP 5.
        const auto update = block.find("->update_live_results(");
        ASSERT_NE(update, std::string::npos) << runner;
        EXPECT_NE(block.find("settled_statistics_cells);", update), std::string::npos) << runner;
        const auto step5 = src.find(kStep5);
        EXPECT_EQ(count_of(src, "results_manager->set_cells("), 1u) << runner;
        const auto day_row = src.find("results_manager->set_cells(", step5);
        ASSERT_NE(day_row, std::string::npos) << runner;
        const auto cells_read = src.find("settled_statistics_cells", step5);
        EXPECT_TRUE(cells_read != std::string::npos && cells_read < day_row + 200)
            << runner << ": the row the run writes does not take the block's cells";
    }
}

// The three histories only the migration 030 statistics read (the per-symbol P&L, the sizing
// capitals, the book's executions) cannot take the rest of the block with them. A failed read
// throws nothing: the seventeen statistics are computed on the series as on a good day, the Day
// T-1 refresh runs (so the stamp is not withheld), and the fourteen cells are NULL under an ERROR
// line that names the read. This reads the runners' text: a runner is one main() with no seam to
// run the block against a failing loader; the helpers the branch calls are tested on their
// behaviour in test_live_statistics_columns.cpp.
TEST(SettledStatisticsBlock, AFailedReadOfTheMigration030HistoriesLeavesTheRestOfTheBlock) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const std::string block = statistics_block(src);
        ASSERT_FALSE(block.empty()) << runner;
        const bool equity = runner.find("equity") != std::string::npos;

        EXPECT_EQ(block.find("the histories of the migration 030 statistics could not be loaded"),
                  std::string::npos)
            << runner << ": a failed read of a 030 history throws out of the block, before the "
            << "seventeen statistics are computed: the day's row is written with them at 0, the "
            << "Day T-1 row is not refreshed and the settled_at stamp is withheld";
        // The only throw left is the grid's: without the series no statistic can be taken.
        EXPECT_EQ(count_of(block, "throw std::runtime_error("), 1u) << runner;
        EXPECT_NE(block.find("throw std::runtime_error(\"the statistics grid could not be "
                             "loaded\");"),
                  std::string::npos)
            << runner;

        // The reads are gathered into one description of what failed, each by name.
        const auto failed = block.find("const std::string failed_histories = "
                                       "failed_statistics_history_reads(");
        ASSERT_NE(failed, std::string::npos) << runner << ": the 030 reads are not caught apart";
        EXPECT_EQ(count_of(block, "statistics_history_read(\"the per-symbol P&L history\", "
                                  "symbol_pnl_res)"),
                  1u)
            << runner;
        EXPECT_EQ(count_of(block, "statistics_history_read(\"the sizing capital history\", "
                                  "sizing_capitals_res)"),
                  equity ? 0u : 1u)
            << runner;
        EXPECT_EQ(count_of(block, "statistics_history_read(\"the book's executions\", "
                                  "book_executions_res)"),
                  1u)
            << runner;

        // The seventeen are computed whatever the reads did, before the branch on them.
        const auto seventeen = block.find(
            "hist_calc.calculate(statistics_series, sessions_per_year, total_trades_hist);");
        const auto branch = block.find("if (!failed_histories.empty()) {");
        ASSERT_NE(seventeen, std::string::npos) << runner;
        ASSERT_NE(branch, std::string::npos) << runner;
        EXPECT_LT(failed, seventeen) << runner;
        EXPECT_LT(seventeen, branch) << runner;

        // The failure branch: an ERROR naming the reads, NULL cells, and nothing else. The good
        // day's computation is its else, and no value of a failed read is asked outside it.
        const auto good_day = block.find("} else {", branch);
        ASSERT_NE(good_day, std::string::npos) << runner;
        const std::string on_failure = block.substr(branch, good_day - branch);
        EXPECT_NE(on_failure.find("ERROR(\"STATISTICS_COLUMNS through Day T-1 \" + t1_date_str +"),
                  std::string::npos)
            << runner;
        EXPECT_NE(on_failure.find("failed_histories"), std::string::npos) << runner;
        EXPECT_NE(on_failure.find("settled_statistics_cells = null_live_statistics_cells();"),
                  std::string::npos)
            << runner;
        EXPECT_EQ(on_failure.find("throw"), std::string::npos) << runner;
        EXPECT_EQ(on_failure.find("return"), std::string::npos) << runner;
        for (const char* value : {"symbol_pnl_res.value()", "sizing_capitals_res.value()",
                                  "book_executions_res.value()"}) {
            const auto asked = block.find(value);
            if (asked == std::string::npos) continue;  // the equity book reads no sizing capital
            EXPECT_GT(asked, good_day) << runner << ": " << value << " is read before the branch";
            EXPECT_EQ(count_of(block, value), 1u) << runner << ": " << value;
        }

        // The refresh and its flag come after the branch, outside it.
        const auto refresh = block.find("->update_live_results(", good_day);
        const auto refreshed = block.find("t1_statistics_refreshed = true;", good_day);
        ASSERT_NE(refresh, std::string::npos) << runner;
        ASSERT_NE(refreshed, std::string::npos) << runner;
        EXPECT_NE(block.find("settled_statistics_cells = "
                             "live_statistics_cells(statistics_columns);",
                             good_day),
                  std::string::npos)
            << runner;
    }
}

// T-8D-2 R44 (b): the equity book's dividend counter is dated by ex-date. The event of an ex-date
// is applied by the next run, so the figure through Day T-1 rides the Day T-1 refresh and the row
// the run writes takes the figure through its own date; neither is the undated lifetime sum. The
// futures rows do not carry the column.
TEST(SettledStatisticsBlock, TheEquityDividendCounterIsDatedAndRefreshedOnTheDayBeforeRow) {
    const std::string src = read_source("apps/strategies/live_equity_mean_reversion.cpp");
    if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
    const std::string block = statistics_block(src);
    ASSERT_FALSE(block.empty());
    const auto step5 = src.find(kStep5);
    ASSERT_NE(step5, std::string::npos);
    const std::string day_t = src.substr(step5);

    // Both figures are taken once, in the block, from the position rows dated the ex-dates.
    EXPECT_EQ(count_of(src, "div_log.dividend_income_through("), 2u);
    EXPECT_EQ(count_of(block, "div_log.dividend_income_through("), 2u);
    EXPECT_NE(block.find("t1_date_str, std::ref(shares_on_ex_date_row), &recorded_quantity_used);"),
              std::string::npos);
    EXPECT_NE(
        block.find("today_date_str, std::ref(shares_on_ex_date_row), &recorded_quantity_used);"),
        std::string::npos);
    EXPECT_EQ(count_of(block, "db->load_positions_by_date("), 1u);
    // A failed read: the two cells come from the one tested rule (live/dividend_counter.hpp,
    // executed in test_live_metrics_includes_dividend_income.cpp), never a literal zero.
    EXPECT_EQ(count_of(block, "dividend_counter_cells("), 1u);
    EXPECT_EQ(count_of(block, "load_stored_dividend_income("), 1u);
    EXPECT_EQ(count_of(src, "reporting 0 on today's row"), 0u);

    // The Day T-1 refresh assigns the figure through Day T-1, before the UPDATE is sent.
    const auto assigned = block.find("metric_updates[\"total_dividend_income\"] = *dividend_income_t1;");
    const auto update = block.find("->update_live_results(");
    ASSERT_NE(assigned, std::string::npos) << "the Day T-1 row is not given the counter";
    ASSERT_NE(update, std::string::npos);
    EXPECT_LT(assigned, update);

    // The row the run writes takes the figure through its own date, not the undated sum.
    EXPECT_EQ(count_of(day_t, "double_metrics[\"total_dividend_income\"] = *dividend_income_today;"),
              1u);
    EXPECT_EQ(count_of(src, "total_dividend_income = div_log.total_cumulative_dividend_income()"),
              0u);

    for (const char* futures : {"apps/strategies/live_portfolio_conservative.cpp",
                                "apps/strategies/live_portfolio.cpp"}) {
        const std::string fut = read_source(futures);
        ASSERT_FALSE(fut.empty()) << futures;
        EXPECT_EQ(count_of(fut, "total_dividend_income"), 0u) << futures;
    }
}
