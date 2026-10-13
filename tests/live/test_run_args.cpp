// tests/live/test_run_args.cpp
//
// T-EMAIL P2-5 (HD ruling Q5): a live run with no date is refused with the usage and exit code 2.
// The runners print dateless_run_message and return kDatelessRunExitCode before they read the
// clock, open the logger or touch the database; the real binaries were run without a date in the
// T-EMAIL report (they print this text and exit 2).

#include <gtest/gtest.h>

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
