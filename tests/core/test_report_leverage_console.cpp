// tests/core/test_report_leverage_console.cpp
//
// T-8D R24 as LOOP_SPEC v6.2 restates it, T-8D-2 R68: the console's leverage lines each name what
// they divide by. The futures runners' first line is the day's gross notional over the day's
// sizing capital E_t, the figure the row's config states as gross_leverage; their day-T line is
// the same notional over the account value. The equity runner prints one line, over the day's
// account value (its "(gross/current)" line divided by the configured capital).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "trade_ngin/core/report_leverage_console.hpp"
#include "trade_ngin/core/resolved_sleeves.hpp"

using namespace trade_ngin;

namespace {

const auto npos = std::string::npos;

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
    for (size_t at = text.find(needle); at != npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}

}  // namespace

TEST(ReportLeverageConsole, TheFirstFuturesLineIsOnTheSizingCapitalAndEqualsTheConfigFigure) {
    // A day whose sizing capital (480,000) is neither the configured 500,000 nor the account.
    const double gross = 801178.52;
    const std::optional<double> sizing =
        stored_config_sizing_capital(true, 480000.0, true, 495000.0);
    EXPECT_EQ(gross_leverage_console_on_sizing_capital(gross, sizing),
              "Gross Leverage (on the sizing capital): 1.67x\n");
    // The config of the same row states the same figure: one capital, one rule.
    const nlohmann::json config =
        futures_live_results_config_json("LIVE_X", {}, sizing, 3, gross, 0.0);
    ASSERT_TRUE(config.contains("gross_leverage"));
    EXPECT_EQ(config["gross_leverage"].get<double>(), *leverage_on(gross, sizing));
    EXPECT_DOUBLE_EQ(*leverage_on(gross, sizing), gross / 480000.0);
    // Not the configured capital: 801,178.52 / 500,000 prints 1.60x.
    EXPECT_NE(gross_leverage_console_on_sizing_capital(gross, sizing),
              gross_leverage_console_on_sizing_capital(gross, 500000.0));
}

TEST(ReportLeverageConsole, ARowWithNoRiskDetailReadsTheCapitalTheRunRead) {
    const std::optional<double> sizing = stored_config_sizing_capital(false, 0.0, true, 495000.0);
    EXPECT_EQ(gross_leverage_console_on_sizing_capital(990000.0, sizing),
              "Gross Leverage (on the sizing capital): 2.00x\n");
}

TEST(ReportLeverageConsole, ASizingHoldPrintsNoFigureAsTheConfigStoresNone) {
    const std::optional<double> none = stored_config_sizing_capital(false, 0.0, false, 0.0);
    EXPECT_EQ(
        gross_leverage_console_on_sizing_capital(801178.52, none),
        "Gross Leverage (on the sizing capital): n/a (no sizing capital was read this run)\n");
    EXPECT_FALSE(futures_live_results_config_json("LIVE_X", {}, none, 3, 801178.52, 0.0)
                     .contains("gross_leverage"));
    // A capital that is not positive has no figure either, in the line and in the config.
    EXPECT_FALSE(leverage_on(801178.52, 0.0));
    EXPECT_FALSE(futures_live_results_config_json("LIVE_X", {}, 0.0, 3, 801178.52, 0.0)
                     .contains("gross_leverage"));
}

TEST(ReportLeverageConsole, TheDayTFuturesLineIsOnTheAccountValueAndSaysSo) {
    EXPECT_EQ(gross_leverage_console_on_account_value(801178.52, 497239.3978),
              "Gross Leverage (on the account value): 1.61x\n");
    EXPECT_EQ(gross_leverage_console_on_account_value(801178.52, 0.0),
              "Gross Leverage (on the account value): n/a (the account value is not positive)\n");
}

TEST(ReportLeverageConsole, TheEquityLineDividesByTheDaysAccountValueItNames) {
    // 85,000 over an account of 104,000 is 0.82x; over the configured 100,000 it would read 0.85x.
    EXPECT_EQ(portfolio_leverage_console_on_account_value(85000.0, 104000.0),
              "Portfolio Leverage (gross / the day's account value): 0.82x\n");
    EXPECT_EQ(portfolio_leverage_console_on_account_value(85000.0, -1.0),
              "Portfolio Leverage (gross / the day's account value): n/a (the account value is not "
              "positive)\n");
}

// READS SOURCE (a labelled wiring and twin guard; the lines themselves are executed above).
TEST(ReportLeverageConsoleTwinSourceGuard, TheRunnersPrintEachLeverageLineThroughItsFunction) {
    const std::string first =
        "std::cout << trade_ngin::gross_leverage_console_on_sizing_capital(\n"
        "                         gross_notional,\n"
        "                         trade_ngin::stored_config_sizing_capital(\n"
        "                             one_pass_day.stores_detail(), one_pass_day.sizing_capital,\n"
        "                             !sizing_hold, sizing_capital_read.capital.capital))";
    const std::string day_t =
        "std::cout << trade_ngin::gross_leverage_console_on_account_value(gross_notional,\n"
        "                                                                         "
        "current_portfolio_value)";
    for (const char* runner : {"apps/strategies/live_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp"}) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty())
            GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(count_of(src, first), 1u);
        EXPECT_EQ(count_of(src, day_t), 1u);
        EXPECT_EQ(src.find("\"Gross Leverage: \""), npos) << "a leverage line with no denominator";
        EXPECT_EQ(src.find("(gross_notional / initial_capital)"), npos);
    }
    const std::string equity = read_source("apps/strategies/live_equity_mean_reversion.cpp");
    if (equity.empty())
        GTEST_SKIP() << "runner source not found from the test working directory";
    EXPECT_EQ(count_of(equity,
                       "trade_ngin::portfolio_leverage_console_on_account_value(\n"
                       "                         gross_notional, current_portfolio_value)"),
              1u);
    EXPECT_EQ(equity.find("Portfolio Leverage (gross/current)"), npos)
        << "the line that named the current value and divided by the configured capital";
    EXPECT_EQ(equity.find("\"Portfolio Leverage: \""), npos);
}
