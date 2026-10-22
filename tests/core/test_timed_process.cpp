// tests/core/test_timed_process.cpp
//
// T-DATED R4 (HD 2026-10-10): the report's chart process is run with a time limit. These cases
// EXECUTE core::run_with_timeout against stand-in processes: one that finishes, one that fails,
// one that sleeps past the limit, and one that sleeps while a grandchild keeps the output open
// (the shape that stopped a live run for ever: the reader never saw the end of the output).

#include <gtest/gtest.h>

#include <chrono>

#include "trade_ngin/core/timed_process.hpp"

using trade_ngin::core::run_with_timeout;
using std::chrono::milliseconds;

namespace {
long elapsed_ms(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

TEST(TimedProcess, AProcessThatFinishesReturnsItsOutputAndACleanStatus) {
    const auto r = run_with_timeout("echo chart-done", milliseconds(5000));
    EXPECT_TRUE(r.started);
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.status, 0);
    EXPECT_EQ(r.output, "chart-done\n");
}

TEST(TimedProcess, AProcessThatFailsIsNotATimeoutAndItsStatusIsNotZero) {
    const auto r = run_with_timeout("echo broken 2>&1; exit 3", milliseconds(5000));
    EXPECT_TRUE(r.started);
    EXPECT_FALSE(r.timed_out);
    EXPECT_NE(r.status, 0);
    EXPECT_EQ(r.output, "broken\n");
}

TEST(TimedProcess, AProcessThatSleepsPastTheLimitIsKilledAndTheCallerGoesOn) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = run_with_timeout("sleep 30", milliseconds(300));
    EXPECT_TRUE(r.started);
    EXPECT_TRUE(r.timed_out);
    EXPECT_LT(elapsed_ms(t0), 5000) << "the caller waited for the sleeping process";
}

TEST(TimedProcess, AGrandchildHoldingTheOutputOpenDoesNotHoldTheCaller) {
    // The shell exits at once; its background child keeps the pipe's write end. Reading to the
    // end of the output, as the run used to, would wait the full 30 seconds.
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = run_with_timeout("(sleep 30) & echo started", milliseconds(300));
    EXPECT_TRUE(r.started);
    EXPECT_TRUE(r.timed_out);
    EXPECT_LT(elapsed_ms(t0), 5000);
    EXPECT_EQ(r.output, "started\n");
}
