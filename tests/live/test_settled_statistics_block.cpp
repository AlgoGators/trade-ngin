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
        EXPECT_NE(block.find("previous_date, metric_updates, &refreshed_rows);"),
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
