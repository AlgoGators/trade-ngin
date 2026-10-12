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
