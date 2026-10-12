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
