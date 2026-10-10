// tests/live/test_settled_stamp.cpp
//
// trading.live_results.settled_at (migration 029; T-8D-2 R43, LOOP_SPEC sections 3.1 and 7.2).
//
// The run that finalizes a row stamps it: one UPDATE, after STEP 4 and the statistics refresh
// finished without error, on every earlier row of the key that carries no stamp. The no-prices
// skip and a finalize that warned stamp nothing; a failed level UPDATE stops the run on all three
// runners; the day's INSERT never names the column, so a re-run's delete and insert clears it.
//
// The three runners are `main()`s and cannot be linked into this binary, so their part is tested
// on the source, as tests/live/test_settled_statistics_block.cpp does for the block before it.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if __has_include("trade_ngin/live/settled_stamp.hpp")
#include "trade_ngin/live/settled_stamp.hpp"
#define TRADE_NGIN_HAS_SETTLED_STAMP 1
#endif

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

// Whether `needle` is followed by a withhold of the stamp before `limit` more characters.
bool withholds_after(const std::string& src, const std::string& needle, size_t limit = 400) {
    const auto at = src.find(needle);
    if (at == std::string::npos) return false;
    const auto w = src.find("settled_stamp.withhold(", at);
    return w != std::string::npos && w - at < limit;
}

const std::vector<std::string> kRunners = {"apps/strategies/live_portfolio_conservative.cpp",
                                           "apps/strategies/live_portfolio.cpp",
                                           "apps/strategies/live_equity_mean_reversion.cpp"};

const char* const kStep4 = "STEP 4: UPDATE Day T-1 live_results AND equity_curve";
const char* const kStatistics = "STEP 4b: THE STATISTICS THROUGH THE LAST SETTLED ROW (Day T-1)";
const char* const kStatisticsEnd = "Exception while computing the statistics through Day T-1";
const char* const kStamp = "STEP 4c: SETTLED_AT";
const char* const kStep5 = "STEP 5: LOAD UPDATED PREVIOUS DAY AGGREGATES";

// The stamp block of a runner: from its banner to STEP 5's.
std::string stamp_block(const std::string& src) {
    const auto begin = src.find(kStamp);
    const auto end = src.find(kStep5);
    if (begin == std::string::npos || end == std::string::npos || end < begin) return {};
    return src.substr(begin, end - begin);
}

}  // namespace

// ---- the statement ------------------------------------------------------------------------------

TEST(SettledStamp, TheStatementStampsEveryEarlierUnstampedRowOfTheKey) {
#ifdef TRADE_NGIN_HAS_SETTLED_STAMP
    EXPECT_EQ(trade_ngin::settled_stamp_sql("LIVE_TREND_FOLLOWING", "CONSERVATIVE_PORTFOLIO",
                                            "2026-04-25"),
              "UPDATE trading.live_results SET settled_at = now() "
              "WHERE strategy_id = 'LIVE_TREND_FOLLOWING' "
              "AND portfolio_id = 'CONSERVATIVE_PORTFOLIO' "
              "AND DATE(date) < '2026-04-25' AND settled_at IS NULL");
#else
    FAIL() << "include/trade_ngin/live/settled_stamp.hpp does not exist: no run stamps settled_at";
#endif
}

TEST(SettledStamp, TheGateIsOpenUntilAPathWithholdsAndKeepsTheFirstReason) {
#ifdef TRADE_NGIN_HAS_SETTLED_STAMP
    trade_ngin::SettledStampGate gate;
    EXPECT_TRUE(gate.open());
    EXPECT_EQ(trade_ngin::settled_stamp_log_line("2026-04-25", 1),
              "SETTLED_AT date=2026-04-25 stamped=1 row(s) dated before the run date");
    gate.withhold("no T-1 closes");
    gate.withhold("the Day T-1 statistics were not refreshed");
    EXPECT_FALSE(gate.open());
    EXPECT_EQ(gate.reason(), "no T-1 closes");
    EXPECT_EQ(trade_ngin::settled_stamp_withheld_log_line("2026-04-26", gate),
              "SETTLED_AT date=2026-04-26 stamped=none reason=\"no T-1 closes\": no row is "
              "settled by this run");
#else
    FAIL() << "include/trade_ngin/live/settled_stamp.hpp does not exist";
#endif
}

// ---- where each runner stamps -------------------------------------------------------------------

TEST(SettledStampRunners, OneStampAfterStepFourAndTheStatisticsRefreshOnEveryRunner) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const bool equity = runner.find("equity") != std::string::npos;

        const auto step4 = src.find(kStep4);
        const auto statistics = src.find(kStatistics);
        const auto statistics_end = src.find(kStatisticsEnd);
        const auto stamp = src.find(kStamp);
        const auto step5 = src.find(kStep5);
        ASSERT_NE(step4, std::string::npos) << runner;
        ASSERT_NE(statistics, std::string::npos) << runner;
        ASSERT_NE(statistics_end, std::string::npos) << runner;
        ASSERT_NE(step5, std::string::npos) << runner;
        ASSERT_NE(stamp, std::string::npos) << runner << ": no run of this book stamps settled_at";
        EXPECT_GT(stamp, step4) << runner;
        EXPECT_GT(stamp, statistics_end)
            << runner << ": the stamp is written before the statistics refresh has finished";
        EXPECT_LT(stamp, step5) << runner << ": the stamp is not written before today's row";

        // ONE statement in the whole runner, the helper's, guarded by the gate.
        EXPECT_EQ(count_of(src, "settled_stamp_sql("), 1u) << runner;
        EXPECT_EQ(count_of(src, "settled_at"), count_of(stamp_block(src), "settled_at") +
                                                   count_of(src.substr(0, stamp), "settled_at"))
            << runner << ": settled_at is written after the stamp block";
        EXPECT_EQ(count_of(src, "SET settled_at"), 0u)
            << runner << ": a second statement writes settled_at";
        const std::string block = stamp_block(src);
        const auto guard = block.find("if (settled_stamp.open()) {");
        const auto call = block.find("settled_stamp_sql(");
        ASSERT_NE(guard, std::string::npos) << runner;
        ASSERT_NE(call, std::string::npos) << runner;
        EXPECT_LT(guard, call) << runner;
        EXPECT_NE(block.find("db->execute_direct_query("), std::string::npos) << runner;
        // The book's own key and the run date.
        if (equity) {
            EXPECT_NE(block.find("settled_stamp_sql(kEquityStrategyId, portfolio_id, "
                                 "core::format_utc_date(now))"),
                      std::string::npos)
                << runner;
        } else {
            EXPECT_NE(block.find("settled_stamp_sql(\n                combined_strategy_id, "
                                 "coordinator_config.portfolio_id, core::format_utc_date(now))"),
                      std::string::npos)
                << runner;
        }
        EXPECT_NE(block.find("INFO(settled_stamp_withheld_log_line("), std::string::npos) << runner;
    }
}

TEST(SettledStampRunners, TheNoPricesSkipAndEveryFinalizeWarningWithholdTheStamp) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const bool equity = runner.find("equity") != std::string::npos;
        const auto step4 = src.find(kStep4);
        const auto stamp = src.find(kStamp);
        ASSERT_NE(step4, std::string::npos) << runner;
        ASSERT_NE(stamp, std::string::npos) << runner << ": no stamp block";
        const std::string finalize = src.substr(step4, stamp - step4);

        // The no-prices skip: the sizing read's own tests (LOOP_SPEC section 3.1).
        EXPECT_NE(finalize.find("if (previous_day_close_prices.empty() && !is_first_trading_day) {\n"
                                "            settled_stamp.withhold(\"no T-1 closes\");"),
                  std::string::npos)
            << runner << ": a day with no Day T-1 closes and a book held is stamped";
        if (!equity) {
            EXPECT_NE(finalize.find("if (two_days_ago_close_prices.empty()) {\n"
                                    "            settled_stamp.withhold(\"no T-2 closes\");"),
                      std::string::npos)
                << runner << ": a day with no Day T-2 closes is stamped";
        }
        // The finalize's WARN paths.
        EXPECT_TRUE(withholds_after(finalize, "WARN(\"LiveDataLoader failed to get yesterday's metrics: \""))
            << runner << ": an unreadable Day T-1 row is stamped";
        EXPECT_TRUE(withholds_after(finalize, "WARN(\"Failed to get yesterday's metrics: \""))
            << runner;
        EXPECT_TRUE(withholds_after(finalize, "WARN(\"Day T-1 live_results UPDATE matched 0 rows for \""))
            << runner << ": a level UPDATE that matched no row is stamped";
        EXPECT_NE(finalize.find("if (!t1_curve_point_written) {\n"
                                "                settled_stamp.withhold("),
                  std::string::npos)
            << runner << ": a Day T-1 whose equity-curve point was not rewritten is stamped";
        EXPECT_EQ(count_of(finalize, "t1_curve_point_written = true;"), 1u) << runner;
        // The statistics refresh: only its success branch lets the stamp through.
        EXPECT_NE(finalize.find("if (!t1_statistics_refreshed) {\n"
                                "            settled_stamp.withhold("),
                  std::string::npos)
            << runner << ": a Day T-1 whose statistics were not refreshed is stamped";
        EXPECT_EQ(count_of(finalize, "t1_statistics_refreshed = true;"), 1u) << runner;
        const auto refreshed = finalize.find("t1_statistics_refreshed = true;");
        const auto success = finalize.rfind("Successfully updated", refreshed);
        ASSERT_NE(success, std::string::npos) << runner;
        EXPECT_LT(refreshed - success, 250u)
            << runner << ": the flag is not set in the refresh's success branch";
        // Nothing reopens a gate.
        EXPECT_EQ(count_of(src, "settled_stamp = "), 0u) << runner;
        EXPECT_EQ(count_of(src, "SettledStampGate settled_stamp;"), 1u) << runner;
    }
}

TEST(SettledStampRunners, AFailedLevelUpdateStopsTheRunOnAllThreeRunners) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const auto failed = src.find("ERROR(\"Failed to update Day T-1 live_results: \"");
        ASSERT_NE(failed, std::string::npos) << runner;
        EXPECT_EQ(count_of(src, "ERROR(\"Failed to update Day T-1 live_results: \""), 1u) << runner;
        const auto zero_rows = src.find("} else if (update_result.value() == 0) {", failed);
        ASSERT_NE(zero_rows, std::string::npos) << runner;
        const auto exit_one = src.find("return 1;", failed);
        ASSERT_NE(exit_one, std::string::npos) << runner;
        EXPECT_LT(exit_one, zero_rows)
            << runner << ": a level UPDATE that failed logs ERROR and the run goes on to write "
                         "today's row beside an unfinalized Day T-1 (T-8D-2 R43: exit 1)";
    }
}

TEST(SettledStampRunners, TheTwoFuturesRunnersCarryTheSameStampAndTheSameGate) {
    const std::string conservative = read_source(kRunners[0]);
    const std::string base = read_source(kRunners[1]);
    if (conservative.empty() || base.empty()) {
        GTEST_SKIP() << "runner source not found from the test working directory";
    }
    const std::string a = stamp_block(conservative);
    ASSERT_FALSE(a.empty()) << "no stamp block";
    EXPECT_EQ(a, stamp_block(base));
    auto finalize = [](const std::string& src) {
        const auto begin = src.find(kStep4);
        const auto end = src.find(kStatistics);
        return begin == std::string::npos || end == std::string::npos ? std::string()
                                                                      : src.substr(begin, end - begin);
    };
    EXPECT_EQ(count_of(finalize(conservative), "settled_stamp.withhold("),
              count_of(finalize(base), "settled_stamp.withhold("));
    EXPECT_EQ(count_of(finalize(conservative), "settled_stamp.withhold("), 6u);
}

// ---- a re-run clears the stamp --------------------------------------------------------------------

TEST(SettledStampRerun, TheDaysInsertNeverNamesTheColumnAndTheRowIsDeletedFirst) {
    const std::string writer = read_source("src/data/postgres_database_extensions.cpp");
    const std::string manager = read_source("src/storage/live_results_manager.cpp");
    const std::string migration = read_source("migrations/029_live_results_settled_at.sql");
    if (writer.empty() || manager.empty()) {
        GTEST_SKIP() << "sources not found from the test working directory";
    }
    // The INSERT of the day's row: no settled_at, so the cell is the column's NULL.
    const auto begin = writer.find("Result<void> PostgresDatabase::store_live_results_complete(");
    const auto end = writer.find("Result<void> PostgresDatabase::store_live_run_metadata(");
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    EXPECT_EQ(writer.substr(begin, end - begin).find("settled_at"), std::string::npos);
    EXPECT_EQ(manager.find("settled_at"), std::string::npos);
    // The row of the date is deleted before it is written again (a re-run).
    const auto stale = manager.find("delete_stale_data(date)");
    const auto store = manager.find("store_live_results_complete(");
    ASSERT_NE(stale, std::string::npos);
    ASSERT_NE(store, std::string::npos);
    EXPECT_LT(stale, store);
    // The column has no default: an INSERT that omits it reads NULL.
    ASSERT_FALSE(migration.empty()) << "migrations/029_live_results_settled_at.sql does not exist";
    EXPECT_NE(migration.find("ADD COLUMN IF NOT EXISTS settled_at timestamptz;"), std::string::npos);
}

// ---- the sizing read's settled test -----------------------------------------------------------

TEST(SettledStampSizingRead, ARowCountsWhenItsStampIsSetOrTheNoBarDayRuleSettlesIt) {
    const std::string read = read_source("include/trade_ngin/live/live_sizing_read.hpp");
    const std::string loader = read_source("src/live/live_data_loader.cpp");
    if (read.empty() || loader.empty()) {
        GTEST_SKIP() << "sources not found from the test working directory";
    }
    EXPECT_NE(loader.find("\"(settled_at IS NOT NULL)::int AS settled_at_set \""), std::string::npos)
        << "the sizing history does not read whether a row is stamped";
    const auto test = read.find("inline bool sizing_history_row_settled(");
    ASSERT_NE(test, std::string::npos) << "the sizing read has no settled test on settled_at";
    const auto body_end = read.find("\n}\n", test);
    const std::string body = read.substr(test, body_end - test);
    const auto column = body.find("if (row.settled_at_set) return true;");
    const auto calendar = body.find("return !sizing_history_day_unsettled(row, calendar);");
    ASSERT_NE(column, std::string::npos);
    ASSERT_NE(calendar, std::string::npos) << "the calendar limb is one line of this function";
    EXPECT_LT(column, calendar);
    // The read's loop asks this test and nothing else.
    EXPECT_EQ(count_of(read, "if (!sizing_history_row_settled(row, calendar)) {"), 1u);
    EXPECT_EQ(count_of(read, "sizing_history_day_unsettled(row, calendar)"), 1u);
}
