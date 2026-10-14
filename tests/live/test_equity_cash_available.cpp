// tests/live/test_equity_cash_available.cpp
//
// HD 2026-10-14 (EMAIL_FEEDBACK section 16.6): an equity book's cash_available is the portfolio value
// less the unrealized P&L less the margin posted, in BOTH writers of the column: the day's insert and the
// next run's Day T-1 finalise UPDATE. The 2026-06-23 row: value 99,737.41, unrealized -71.39 (a loss, so
// it adds back), margin posted 5,933.47: cash 93,732.55.

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>

#include "trade_ngin/live/margin_columns.hpp"

using namespace trade_ngin;

TEST(EquityCashAvailable, TheFormulaIsValueLessUnrealizedLessMarginOnACashAccount) {
    // a gain of 126.68 held in shares: value 100,126.68 = 100,000 cash-and-shares at cost + 126.68
    EXPECT_DOUBLE_EQ(equity_cash_available(100126.68, 126.68, 5798.18, true), 100000.0 - 5798.18);
    // the 06-23 row's three stored figures: value - unrealized - margin
    EXPECT_NEAR(equity_cash_available(99737.41, -71.39, 5933.47, true), 99737.41 + 71.39 - 5933.47, 1e-9);
    // a margin account keeps value - margin
    EXPECT_DOUBLE_EQ(equity_cash_available(100126.68, 126.68, 5798.18, false), 100126.68 - 5798.18);
}

TEST(EquityCashAvailable, TheFinaliseUpdateWriterDropsTheUnrealizedTermOnACashAccount) {
    const std::string value = "100000.0 + (r) + 126.68";
    const std::string cash = equity_cash_available_sql(value, "126.68", true);
    EXPECT_EQ(cash, "cash_available = (100000.0 + (r) + 126.68) - (126.68) - COALESCE(margin_posted, 0.0) ");
    const std::string margin = equity_cash_available_sql(value, "126.68", false);
    EXPECT_EQ(margin, "cash_available = (100000.0 + (r) + 126.68) - COALESCE(margin_posted, 0.0) ");
}

// The runner hands both writers the one constant; READS SOURCE to keep the two calls in step.
TEST(EquityCashAvailableSource, BothWritersOfTheRunnerUseTheFormula) {
    for (const char* path : {"apps/strategies/live_equity_mean_reversion.cpp"}) {
        std::ifstream in(path);
        if (!in) GTEST_SKIP() << "runner source not found from the working directory";
        const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(src.find("equity_cash_available(current_portfolio_value, total_unrealized_pnl,"), std::string::npos);
        EXPECT_NE(src.find("equity_cash_available_sql(yesterday_finalised_value_sql,"), std::string::npos);
        EXPECT_EQ(src.find("{\"cash_available\", current_portfolio_value - total_posted_margin}"), std::string::npos);
    }
}
