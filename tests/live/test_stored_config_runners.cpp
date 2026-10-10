// tests/live/test_stored_config_runners.cpp
//
// T-8D R24 and R25, T-8D-2 R53 and R73, LOOP_SPEC sections 7.5 and 7.5.1 -- what the runners
// record about their own configuration: trading.live_results.config and the portfolio_config of
// the two run-metadata tables.
//
// The runners are `main()`s and cannot be linked into this binary, so what is tested here is the
// structure in the source (the approach of tests/live/test_settled_statistics_block.cpp): each
// runner hands the values it resolved to the builders of core/resolved_sleeves.hpp, whose JSON
// is tested in tests/core/test_resolved_sleeves.cpp.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string read_source(const std::string& relative) {
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
}

size_t count_of(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

const std::vector<std::string> kFuturesLive = {"apps/strategies/live_portfolio_conservative.cpp",
                                               "apps/strategies/live_portfolio.cpp"};
const char* const kEquityLive = "apps/strategies/live_equity_mean_reversion.cpp";
const std::vector<std::string> kFuturesBacktests = {"apps/backtest/bt_portfolio_conservative.cpp",
                                                    "apps/backtest/bt_portfolio.cpp"};
const char* const kEquityBacktest = "apps/backtest/bt_equity_mean_reversion.cpp";

}  // namespace

// R25: a sleeve whose risk_target resolves to 0.25 (BASE's FAST sleeve) is stored as 0.25. Each
// futures runner collects the idm and risk_target it hands to each strategy it builds, on both
// branches of its factory, and stores that list; it no longer writes the string from literals.
TEST(StoredConfigRunners, TheFuturesRunnersStoreEachSleevesResolvedValues) {
    for (const auto& runner : kFuturesLive) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(count_of(src, "resolved_sleeves.push_back(\n"
                                "                    {strategy_name, trend_config.idm, "
                                "trend_config.risk_target, allocation});"),
                  2u)
            << runner << ": both factory branches record what the strategy is built with";
        EXPECT_NE(src.find("futures_live_results_config_json(\n"
                           "                combined_strategy_id, resolved_sleeves,"),
                  std::string::npos)
            << runner << ": the stored string is not built from the resolved sleeves";
        EXPECT_EQ(src.find("report_config_json[\"risk_target\"] = 0.2"), std::string::npos)
            << runner << ": the string is still written from a literal risk target";
        EXPECT_EQ(count_of(src, "INFO(trade_ngin::resolved_sleeves_log_line("), 1u) << runner;
    }
}

// R24: capital_allocation and gross_leverage are on the day's sizing capital: risk_detail's on a
// row that stores it, else the capital the run read, and none on a sizing hold.
TEST(StoredConfigRunners, TheFuturesRunnersStateTheDaysSizingCapital) {
    for (const auto& runner : kFuturesLive) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_NE(src.find("stored_config_sizing_capital(\n"
                           "                    one_pass_day.stores_detail(), "
                           "one_pass_day.sizing_capital, !sizing_hold,\n"
                           "                    sizing_capital_read.capital.capital)"),
                  std::string::npos)
            << runner;
        EXPECT_EQ(src.find("report_config_json[\"capital_allocation\"] = initial_capital"),
                  std::string::npos)
            << runner << ": capital_allocation is still the configured capital";
        EXPECT_EQ(src.find("report_config_json[\"gross_leverage\"] = gross_notional / "
                           "initial_capital"),
                  std::string::npos)
            << runner << ": gross_leverage is still on the configured capital";
    }
}

// R73: config["strategy_type"] is the id the row is keyed on, on all three runners.
TEST(StoredConfigRunners, StrategyTypeIsTheRowsStrategyIdOnEveryRunner) {
    for (const auto& runner : kFuturesLive) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_NE(src.find("coordinator_config.strategy_id = combined_strategy_id;"),
                  std::string::npos)
            << runner;
        EXPECT_NE(src.find("futures_live_results_config_json(\n"
                           "                combined_strategy_id,"),
                  std::string::npos)
            << runner;
    }
    const std::string equity = read_source(kEquityLive);
    if (equity.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
    EXPECT_NE(equity.find("coordinator_config.strategy_id = kEquityStrategyId;"),
              std::string::npos);
    EXPECT_NE(equity.find("config_json[\"strategy_type\"] = kEquityStrategyId;"),
              std::string::npos);
    EXPECT_NE(equity.find("config_json[trade_ngin::kSleevesKey] = "
                          "trade_ngin::resolved_sleeves_json(resolved_sleeves);"),
              std::string::npos);
    // The equity book has no sizing capital: its two capital keys stay on its configured capital.
    EXPECT_NE(equity.find("config_json[\"capital_allocation\"] = mr_config.capital_allocation;"),
              std::string::npos);
    EXPECT_NE(equity.find("config_json[\"portfolio_leverage\"] = gross_notional / initial_capital;"),
              std::string::npos);
}

// LOOP_SPEC 7.5.1: portfolio_config.statistics_K in both run-metadata tables, from the config
// value the statistics are annualised with; total_capital stays and no sizing_capital key joins it.
TEST(StoredConfigRunners, BothRunMetadataTablesRecordTheSessionsAYearOfTheRunsSeries) {
    std::vector<std::pair<std::string, std::string>> writers;
    for (const auto& runner : kFuturesLive) {
        writers.push_back({runner, "portfolio_config_json[trade_ngin::kStatisticsKKey] =\n"
                                   "            app_config.statistics.futures_sessions_per_year;"});
    }
    writers.push_back({kEquityLive, "portfolio_config_json[trade_ngin::kStatisticsKKey] =\n"
                                    "                app_config.statistics.equity_sessions_per_year;"});
    for (const auto& runner : kFuturesBacktests) {
        writers.push_back({runner, "app_config.backtest.frozen_end_date, backtest_results.warmup_days,\n"
                                   "                app_config.statistics.futures_sessions_per_year);"});
    }
    writers.push_back({kEquityBacktest,
                       "app_config.backtest.frozen_end_date, backtest_results.warmup_days,\n"
                       "                    app_config.statistics.equity_sessions_per_year);"});
    for (const auto& [runner, needle] : writers) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(count_of(src, needle), 1u) << runner;
        EXPECT_EQ(src.find("311.0574"), std::string::npos) << runner << ": a second constant";
        EXPECT_EQ(src.find("portfolio_config_json[\"sizing_capital\"]"), std::string::npos) << runner;
        EXPECT_EQ(src.find("config_json[\"sizing_capital\"]"), std::string::npos) << runner;
    }
}

// R53: the futures backtests record each sleeve's resolved idm and risk_target beside the
// strategy_allocations they already record, and the window rule and the warm-up.
TEST(StoredConfigRunners, TheFuturesBacktestsRecordTheResolvedSleevesAndTheWindow) {
    for (const auto& runner : kFuturesBacktests) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(count_of(src, "resolved_sleeves.push_back(\n"
                                "                    {strategy_id, trend_config.idm, "
                                "trend_config.risk_target, allocation});"),
                  2u)
            << runner;
        EXPECT_NE(src.find("portfolio_config_json[trade_ngin::kSleevesKey] =\n"
                           "                trade_ngin::resolved_sleeves_json(resolved_sleeves, false);"),
                  std::string::npos)
            << runner;
        EXPECT_NE(src.find("portfolio_config_json, app_config.backtest.lookback_years,"),
                  std::string::npos)
            << runner;
        EXPECT_EQ(count_of(src, "INFO(trade_ngin::resolved_sleeves_log_line("), 1u) << runner;
    }
}
