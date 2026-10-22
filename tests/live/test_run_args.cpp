// tests/live/test_run_args.cpp
//
// T-EMAIL P2-5 (HD ruling Q5): a live run with no date is refused with the usage and exit code 2.
// The runners print dateless_run_message and return kDatelessRunExitCode before they read the
// clock, open the logger or touch the database; the real binaries were run without a date in the
// T-EMAIL report (they print this text and exit 2).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "trade_ngin/live/run_args.hpp"

using namespace trade_ngin::live;

TEST(RunArgs, ADatelessRunExitsTwoWhichIsNotTheRiskModuleOrTheEmailCode) {
    EXPECT_EQ(kDatelessRunExitCode, 2);
    EXPECT_NE(kDatelessRunExitCode, 0);
    EXPECT_NE(kDatelessRunExitCode, 1);
    EXPECT_NE(kDatelessRunExitCode, 3);
    EXPECT_NE(kDatelessRunExitCode, 4);
}

TEST(RunArgs, TheMessageSaysNothingWasRunOrSentAndShowsTheUsage) {
    const std::string text = dateless_run_message("live_portfolio_conservative");
    EXPECT_NE(text.find("nothing was run and nothing was sent"), std::string::npos);
    EXPECT_NE(text.find("Usage: live_portfolio_conservative YYYY-MM-DD [--send-email]"),
              std::string::npos);
}

// The rejoin (2026-10-22): main's --date and the positional date name the same run; two different
// dates are refused (exit 1) with this message, never the later one taken. The real binaries were
// run both ways in the rejoin audit (evidence/REJOIN/run_args/).
TEST(RunArgs, TwoDifferentRunDatesNameBoth) {
    const std::string text = two_run_dates_message("2026-10-21", "2026-10-22");
    EXPECT_NE(text.find("Two run dates were given (2026-10-21 and 2026-10-22)"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("give one, positionally or as --date"), std::string::npos) << text;
}

// READS SOURCE (a labelled wiring guard; the runners are main()s and cannot be linked here). Both
// futures runners refuse a second, different date before they take it, and return 1.
TEST(RunArgs, BothFuturesRunnersRefuseASecondDifferentRunDateBeforeTakingIt) {
    namespace fs = std::filesystem;
    for (const char* runner : {"apps/strategies/live_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp"}) {
        SCOPED_TRACE(runner);
        std::string src;
        for (fs::path dir = fs::current_path(); !dir.empty(); dir = dir.parent_path()) {
            if (fs::exists(dir / runner)) {
                std::ifstream in(dir / runner);
                std::ostringstream ss;
                ss << in.rdbuf();
                src = ss.str();
                break;
            }
            if (dir == dir.parent_path()) break;
        }
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const auto guard = src.find("if (!run_date_arg.empty() && run_date_arg != arg) {");
        ASSERT_NE(guard, std::string::npos);
        const auto message = src.find("two_run_dates_message(run_date_arg, arg)", guard);
        const auto refuse = src.find("return 1;", guard);
        const auto take = src.find("run_date_arg = arg;", guard);
        ASSERT_NE(message, std::string::npos);
        ASSERT_NE(take, std::string::npos);
        EXPECT_LT(message, take);
        EXPECT_LT(refuse, take);
    }
}
