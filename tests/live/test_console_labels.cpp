// tests/live/test_console_labels.cpp
//
// T-8D-2 R68: the live runners' console lines name what they print. The text of each line is
// built by a function of include/trade_ngin/live/console_labels.hpp, called here with values;
// the runners are `main()`s, so that they print each line through its function is a labelled
// source guard at the end of each block.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "trade_ngin/live/console_labels.hpp"

using namespace trade_ngin;

namespace {

const auto npos = std::string::npos;

const char* const kFuturesRunners[] = {"apps/strategies/live_portfolio_conservative.cpp",
                                       "apps/strategies/live_portfolio.cpp"};
const char* const kEquityRunner = "apps/strategies/live_equity_mean_reversion.cpp";

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

// ---------------------------------------------------------------------------------------------
// The Daily Position Report's two lines.
// ---------------------------------------------------------------------------------------------

TEST(ConsolePositionReport, PrintsTheDateAndTheSymbolsInTheBook) {
    EXPECT_EQ(position_report_head_console(2026, 4, 24, 36),
              "Date: 2026-04-24\nSymbols in book: 36\n");
    EXPECT_EQ(position_report_head_console(2026, 12, 5, 0),
              "Date: 2026-12-05\nSymbols in book: 0\n");
    EXPECT_EQ(position_report_head_console(2026, 4, 24, 36).find("Total Positions"), npos)
        << "the count is of symbols in the book map, flat ones included, not of positions";
}

// What the leak did: the date set the stream's fill to '0' and it stayed, so the padded columns
// printed afterwards came out as "0000Symbol". A stream that prints the head keeps its fill.
TEST(ConsolePositionReport, LeavesTheFillOfTheStreamThatPrintsIt) {
    std::ostringstream out;
    out << position_report_head_console(2026, 4, 24, 36);
    EXPECT_EQ(out.fill(), ' ');
    out.str("");
    out << std::setw(10) << "Symbol";
    EXPECT_EQ(out.str(), "    Symbol");
}

// READS SOURCE (a labelled wiring and twin guard; the lines themselves are executed above).
TEST(ConsolePositionReportTwinSourceGuard,
     TheRunnersPrintTheHeadThroughItsFunctionAndNoEmptyTable) {
    const std::string call = "std::cout << trade_ngin::position_report_head_console(";
    for (const std::string runner : {std::string(kFuturesRunners[0]),
                                     std::string(kFuturesRunners[1]), std::string(kEquityRunner)}) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty())
            GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(count_of(src, call), 1u);
        EXPECT_EQ(src.find("std::cout << \"Date: \""), npos) << "the date still sets the fill";
        EXPECT_EQ(src.find("\"Total Positions: \""), npos);
        EXPECT_EQ(src.find("\"Mkt Price\""), npos) << "the header of a table with no rows";
        EXPECT_EQ(src.find("\"Unreal PnL\""), npos);
    }
}

// ---------------------------------------------------------------------------------------------
// Strategy Metrics: the snapshot reporter's lines.
// ---------------------------------------------------------------------------------------------

// The futures runners: the reporter's leverage is over the sizing capital the snapshot was given;
// they print no reporter volatility (the overlay's expected risk stands in its place).
TEST(ConsoleReporterLines, TheFuturesLinesNameTheReporterAndTheSizingCapital) {
    const ReporterConsoleFigures figures{0.1551, 1.1112, -0.4321, 0.9645, 0.0312};
    EXPECT_EQ(reporter_console(figures, "the sizing capital", false),
              "Gross Leverage (reporter, on the sizing capital): 1.11\n"
              "Net Leverage (reporter, on the sizing capital): -0.43\n"
              "Max Correlation (reporter): 0.96\n"
              "Jump Risk (99th, reporter): 0.03\n");
}

// The equity runner: its volatility line is the stored portfolio_var as a percent, named as the
// reporter's ex-ante figure; its leverage is over the configured capital.
TEST(ConsoleReporterLines, TheEquityLinesNameTheExAnteVolAndTheConfiguredCapital) {
    const ReporterConsoleFigures figures{0.1924, 0.0841, 0.0841, 0.5, 0.0123};
    const std::string text = reporter_console(figures, "the configured capital", true);
    EXPECT_EQ(text,
              "Ex-ante vol (reporter): 19.24%\n"
              "Gross Leverage (reporter, on the configured capital): 0.08\n"
              "Net Leverage (reporter, on the configured capital): 0.08\n"
              "Max Correlation (reporter): 0.50\n"
              "Jump Risk (99th, reporter): 0.01\n");
    EXPECT_EQ(text.find("Volatility"), npos) << "portfolio_var is not the account's volatility";
}

TEST(ConsoleReporterLines, AFailedEvaluationPrintsNotAvailableUnderTheSameNames) {
    EXPECT_EQ(reporter_console(std::nullopt, "the sizing capital", false),
              "Gross Leverage (reporter, on the sizing capital): N/A\n"
              "Net Leverage (reporter, on the sizing capital): N/A\n"
              "Max Correlation (reporter): N/A\n"
              "Jump Risk (99th, reporter): N/A\n");
    EXPECT_EQ(reporter_console(std::nullopt, "the configured capital", true)
                  .rfind("Ex-ante vol (reporter): N/A\n", 0),
              0u);
}

// READS SOURCE (a labelled wiring and twin guard; the lines themselves are executed above).
TEST(ConsoleReporterLinesTwinSourceGuard, TheRunnersPrintTheReporterThroughItsFunction) {
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty())
            GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(count_of(src,
                           "trade_ngin::reporter_console(reporter_figures, "
                           "\"the sizing capital\", false)"),
                  1u);
        // The capital the label names is the capital the snapshot was built on.
        EXPECT_EQ(
            count_of(src, "snapshot_risk_config.capital = Decimal(portfolio->sizing_capital());"),
            1u);
        EXPECT_EQ(src.find("\"Gross Leverage (Risk): "), npos);
        EXPECT_EQ(src.find("\"Net Leverage: "), npos);
        EXPECT_EQ(src.find("\"Max Correlation: "), npos);
        EXPECT_EQ(src.find("\"Jump Risk (99th): "), npos);
    }
    const std::string equity = read_source(kEquityRunner);
    if (equity.empty())
        GTEST_SKIP() << "runner source not found from the test working directory";
    EXPECT_EQ(count_of(equity,
                       "trade_ngin::reporter_console(reporter_figures, "
                       "\"the configured capital\", true)"),
              1u);
    EXPECT_EQ(count_of(equity, "risk_config.capital = Decimal(initial_capital);"), 1u);
    EXPECT_EQ(count_of(equity, "trade_ngin::RiskManager snapshot_rm(risk_config);"), 1u);
    EXPECT_EQ(equity.find("\"Volatility: "), npos);
    EXPECT_EQ(equity.find("\"Gross Leverage: "), npos);
    EXPECT_EQ(equity.find("\"Net Leverage: "), npos);
}

// ---------------------------------------------------------------------------------------------
// The day-T lines.
// ---------------------------------------------------------------------------------------------

TEST(ConsoleDayTLines, RealizedIsNamedGrossOfCosts) {
    EXPECT_EQ(realized_pnl_console(5570.56), "Realized P&L (gross of costs): $5570.56\n");
    EXPECT_EQ(realized_pnl_console(-12.345), "Realized P&L (gross of costs): $-12.35\n");
    EXPECT_EQ(realized_pnl_console(0.0), "Realized P&L (gross of costs): $0.00\n");
}

// The futures day-T return: a day's costs of 2.05 over 504,530 is -0.000406 percent, which
// prints -0.00%; the label says why it is that small.
TEST(ConsoleDayTLines, TheFuturesDailyReturnSaysItIsTheDaysCostsOnly) {
    EXPECT_EQ(daily_return_console_costs_only(-2.05 / 504530.0 * 100.0),
              "Daily Return (day T: costs only): -0.00%\n");
    EXPECT_EQ(daily_return_console_costs_only(-0.0149),
              "Daily Return (day T: costs only): -0.01%\n");
}

// The equity day-T return carries the day's realized P&L and the change in the mark, so it is
// never named costs only.
TEST(ConsoleDayTLines, TheEquityDailyReturnSaysWhatItHoldsAndNeverCostsOnly) {
    const std::string line = daily_return_console_as_written(0.2213);
    EXPECT_EQ(line,
              "Daily Return (day T as written: realized less costs, plus the change in the "
              "mark): 0.22%\n");
    EXPECT_EQ(line.find("costs only"), npos);
}

// READS SOURCE (a labelled wiring and twin guard; the lines themselves are executed above). It
// also pins what makes each label true: the futures day-T P&L is the day's costs and nothing
// else, the equity day-T P&L is the realized net of costs plus the change in the mark.
TEST(ConsoleDayTLinesTwinSourceGuard, EachRunnerPrintsTheLineThatIsTrueOfItsOwnDayTReturn) {
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty())
            GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(count_of(src, "trade_ngin::realized_pnl_console(total_realized_pnl)"), 1u);
        EXPECT_EQ(count_of(src, "trade_ngin::daily_return_console_costs_only(daily_return)"), 1u);
        EXPECT_EQ(count_of(src, "double daily_pnl = daily_pnl_for_today;"), 1u);
        EXPECT_NE(src.find("double daily_pnl_for_today =\n"
                           "            -total_daily_transaction_costs;"),
                  npos);
        EXPECT_EQ(src.find("std::cout << \"Realized P&L: $\""), npos);
        EXPECT_EQ(src.find("std::cout << \"Daily Return: \""), npos);
    }
    const std::string equity = read_source(kEquityRunner);
    if (equity.empty())
        GTEST_SKIP() << "runner source not found from the test working directory";
    EXPECT_EQ(count_of(equity, "trade_ngin::realized_pnl_console(total_realized_pnl)"), 1u);
    EXPECT_EQ(count_of(equity, "trade_ngin::daily_return_console_as_written(daily_return)"), 1u);
    EXPECT_EQ(equity.find("daily_return_console_costs_only"), npos);
    EXPECT_EQ(count_of(equity, "double daily_pnl = daily_pnl_for_today + daily_unrealized_pnl;"),
              1u);
    EXPECT_EQ(
        count_of(equity,
                 "double daily_pnl_for_today = daily_realized_pnl - total_daily_commissions;"),
        1u);
    EXPECT_EQ(equity.find("std::cout << \"Realized P&L: $\""), npos);
    EXPECT_EQ(equity.find("std::cout << \"Daily Return: \""), npos);
}

// ---------------------------------------------------------------------------------------------
// Current Forecasts.
// ---------------------------------------------------------------------------------------------

// A two-sleeve book: each sleeve has its own line for a symbol, with its own forecast and its
// own target, beside the quantity that sleeve's row stores. The sleeve target is what the
// sleeve asked for before the portfolio acted; the stored quantity is what is held: on M2K the
// trend sleeve asked for 4 and the book stores 2.
TEST(ConsoleCurrentForecasts, EverySleeveHasItsOwnLineWithTheStoredQuantityBesideItsTarget) {
    const std::vector<ForecastConsoleRow> rows = {
        {"TREND_FOLLOWING", "M2K", 12.1077, 4.0, 2.0},
        {"TREND_FOLLOWING", "ZF", -3.5, -2.0, 0.0},
        {"TREND_FOLLOWING_FAST", "M2K", -20.0, -1.0, -1.0},
    };
    EXPECT_EQ(
        current_forecasts_console("Forecast", rows, 2),
        "\n======= Current Forecasts =======\n"
        "              Sleeve |     Symbol |     Forecast | Sleeve target (before portfolio) | "
        "Stored quantity\n"
        "--------------------------------------------------------------------------------"
        "---------------------\n"
        "     TREND_FOLLOWING |        M2K |      12.1077 |                             4.00 | "
        "           2.00\n"
        "     TREND_FOLLOWING |         ZF |      -3.5000 |                            -2.00 | "
        "           0.00\n"
        "TREND_FOLLOWING_FAST |        M2K |     -20.0000 |                            -1.00 | "
        "          -1.00\n");
}

// No column is named "Position": the sleeve's own target is not the position held.
TEST(ConsoleCurrentForecasts, TheHeaderNamesTheSleeveTargetAndNoPosition) {
    const std::string table = current_forecasts_console("Forecast", {}, 2);
    EXPECT_NE(table.find("Sleeve target (before portfolio)"), npos);
    EXPECT_NE(table.find("Stored quantity"), npos);
    EXPECT_EQ(table.find("Position"), npos);
    // The rule is as wide as the header.
    const auto header_at = table.find("Sleeve |");
    const auto header_end = table.find('\n', header_at);
    const auto rule_end = table.find('\n', header_end + 1);
    EXPECT_EQ(rule_end - header_end - 1, header_end - header_at);
}

// The equity book: the sleeve's figure is a z-score and the column says so; fractional shares
// print at the scale the stored column holds.
TEST(ConsoleCurrentForecasts, TheEquityTableNamesTheZScoreAndPrintsSharesAtSixDecimals) {
    const std::string table = current_forecasts_console(
        "z-score", {{"EQUITY_MEAN_REVERSION", "TMUS", -2.3456, 17.612345, 17.612345}}, 6);
    EXPECT_NE(table.find("|      z-score |"), npos);
    EXPECT_EQ(table.find("Forecast |"), npos);
    EXPECT_NE(table.find("|      -2.3456 |"), npos);
    EXPECT_EQ(count_of(table, "17.612345"), 2u);
}

// A quantity that rounds to zero prints as zero (the parent printed "-0.00"); a sleeve with no
// reading of its own prints n/a and keeps its stored quantity.
TEST(ConsoleCurrentForecasts, AZeroQuantityHasNoSignAndASleeveWithNoReadingSaysSo) {
    const std::string table = current_forecasts_console(
        "Forecast", {{"A", "ES", 0.0, -0.0001, -0.0}, {"B", "ES", std::nullopt, std::nullopt, 3.0}},
        2);
    EXPECT_EQ(table.find("-0.00"), npos) << table;
    EXPECT_NE(table.find("     A |         ES |       0.0000 |                             0.00 | "
                         "           0.00\n"),
              npos)
        << table;
    EXPECT_NE(table.find("     B |         ES |          n/a |                              n/a | "
                         "           3.00\n"),
              npos)
        << table;
}

// READS SOURCE (a labelled wiring and twin guard; the table itself is executed above).
TEST(ConsoleCurrentForecastsTwinSourceGuard, TheRunnersPrintEverySleeveAndTheStoredBook) {
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty())
            GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(count_of(src,
                           "trade_ngin::current_forecasts_console(\"Forecast\", "
                           "forecast_rows, 2)"),
                  1u);
        // Every sleeve, and the book the runner stores.
        EXPECT_NE(src.find("for (size_t i = 0; i < strategies.size(); ++i) {\n"
                           "            const auto trend_sleeve ="),
                  npos);
        EXPECT_NE(src.find("const auto stored_sleeve = strategy_positions_map.find("
                           "strategy_names[i]);"),
                  npos);
        EXPECT_EQ(src.find("tf_strategy_typed"), npos) << "the first sleeve only";
        EXPECT_EQ(src.find("<< \"Position\""), npos);
    }
    const std::string equity = read_source(kEquityRunner);
    if (equity.empty())
        GTEST_SKIP() << "runner source not found from the test working directory";
    EXPECT_EQ(count_of(equity,
                       "trade_ngin::current_forecasts_console(\"z-score\", "
                       "forecast_rows, 6)"),
              1u);
    EXPECT_NE(equity.find("trade_ngin::account_book_of_rows(positions_to_save);"), npos);
    EXPECT_EQ(equity.find("<< \"Forecast\""), npos);
    EXPECT_EQ(equity.find("<< \"Position\""), npos);
}

// ---------------------------------------------------------------------------------------------
// The backtest apps' summary.
// ---------------------------------------------------------------------------------------------

// READS SOURCE (a labelled twin guard: the three labels are bare literals inside each app's
// main, shown by the summary block of a backtest run's log). The win rate and the trade count
// are figures of CLOSING fills (T-8D R11) and the volatility is annualised (R8); each app says so.
TEST(BacktestConsoleLabelsTwinSourceGuard, TheBacktestAppsNameTheClosingFillFiguresAndTheVolatility) {
    for (const char* app :
         {"apps/backtest/bt_portfolio_conservative.cpp", "apps/backtest/bt_portfolio.cpp",
          "apps/backtest/bt_equity_mean_reversion.cpp", "apps/backtest/bt_bpgv_rotation.cpp"}) {
        SCOPED_TRACE(app);
        const std::string src = read_source(app);
        if (src.empty())
            GTEST_SKIP() << "app source not found from the test working directory";
        EXPECT_EQ(count_of(src, "std::cout << \"Volatility (annualised): \""), 1u);
        EXPECT_EQ(count_of(src,
                           "std::cout << \"Trade win rate (closing fills): \" << "
                           "(backtest_results.win_rate * 100.0)"),
                  1u);
        EXPECT_EQ(
            count_of(src, "std::cout << \"Closing fills: \" << backtest_results.total_trades"), 1u);
        EXPECT_EQ(src.find("\"Volatility: \""), npos);
        EXPECT_EQ(src.find("\"Win Rate: \""), npos);
        EXPECT_EQ(src.find("\"Total Trades: \""), npos);
    }
}
