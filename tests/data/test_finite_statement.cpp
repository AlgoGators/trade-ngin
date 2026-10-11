// The numbers of a results statement, guarded (T-8D R12, T-8D-2 R54): a double that is not
// finite is never formatted into a statement. The three builders the writers call
// (PostgresDatabase::store_backtest_summary, update_live_results, store_live_results_complete),
// executed on their own; test_finite_statement_db.cpp runs the writers against the tables.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/data/finite_statement.hpp"

using namespace trade_ngin;

namespace {
const double kNan = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();
}  // namespace

TEST(FiniteStatement, AFiniteNumberIsItsToStringTextAndANonFiniteOneIsNoValue) {
    EXPECT_EQ(finite_sql_number(1.5), std::optional<std::string>("1.500000"));
    EXPECT_EQ(finite_sql_number(0.0), std::optional<std::string>("0.000000"));
    EXPECT_EQ(finite_sql_number(-2.25), std::optional<std::string>(std::to_string(-2.25)));
    EXPECT_FALSE(finite_sql_number(kNan).has_value());
    EXPECT_FALSE(finite_sql_number(kInf).has_value());
    EXPECT_FALSE(finite_sql_number(-kInf).has_value());
}

// backtest.results: a finite run's VALUES are the text they were (std::to_string of each metric
// in the column order); a metric that is not finite, or not handed in, is DEFAULT and is named.
TEST(FiniteStatement, TheBacktestSummaryValuesKeepFiniteMetricsAndDefaultTheRest) {
    const std::vector<std::string> names = {"total_return", "sharpe_ratio", "sortino_ratio",
                                            "calmar_ratio", "transaction_costs"};
    std::vector<std::string> without;
    EXPECT_EQ(backtest_summary_metric_values(names,
                                             {{"total_return", 0.616818},
                                              {"sharpe_ratio", 1.20523},
                                              {"sortino_ratio", 1.786723},
                                              {"calmar_ratio", 2.306329},
                                              {"transaction_costs", 9876.5}},
                                             &without),
              "0.616818, 1.205230, 1.786723, 2.306329, 9876.500000");
    EXPECT_TRUE(without.empty());

    const std::string guarded = backtest_summary_metric_values(
        names,
        {{"total_return", 0.1}, {"sharpe_ratio", kNan}, {"sortino_ratio", kInf}, {"transaction_costs", -kInf}},
        &without);
    EXPECT_EQ(guarded, "0.100000, DEFAULT, DEFAULT, DEFAULT, DEFAULT");
    EXPECT_EQ(without, (std::vector<std::string>{"sharpe_ratio", "sortino_ratio", "calmar_ratio",
                                                 "transaction_costs"}));
    EXPECT_EQ(guarded.find("nan"), std::string::npos);
    EXPECT_EQ(guarded.find("inf"), std::string::npos);
    EXPECT_EQ(joined_columns(without), "sharpe_ratio, sortino_ratio, calmar_ratio, transaction_costs");
}

// trading.live_results, the Day T-1 UPDATE: a value that is not finite is assigned NULL.
TEST(FiniteStatement, TheLiveUpdateAssignsNullToAValueThatIsNotFinite) {
    std::vector<std::string> without;
    EXPECT_EQ(live_results_update_assignments({{"sharpe_ratio", 1.5}}, &without),
              "sharpe_ratio = 1.500000");
    EXPECT_TRUE(without.empty());
    EXPECT_EQ(live_results_update_assignments({{"profit_factor", kNan}}, &without),
              "profit_factor = NULL");
    EXPECT_EQ(without, std::vector<std::string>{"profit_factor"});
    EXPECT_EQ(live_results_update_assignments({}, &without), "");

    without.clear();
    const std::string mixed =
        live_results_update_assignments({{"volatility", 12.5}, {"sortino_ratio", kInf}}, &without);
    EXPECT_NE(mixed.find("volatility = 12.500000"), std::string::npos) << mixed;
    EXPECT_NE(mixed.find("sortino_ratio = NULL"), std::string::npos) << mixed;
    EXPECT_NE(mixed.find(", "), std::string::npos) << mixed;
    EXPECT_EQ(without, std::vector<std::string>{"sortino_ratio"});
}

// trading.live_results, the day's INSERT: a value that is not finite is left out of the column
// list and of the values, so its cell stays NULL.
TEST(FiniteStatement, TheLiveInsertLeavesOutAValueThatIsNotFinite) {
    std::string columns = "strategy_id", values = "'S'";
    std::vector<std::string> without;
    append_live_results_double_columns({{"daily_pnl", 10.0}}, columns, values, &without);
    EXPECT_EQ(columns, "strategy_id, daily_pnl");
    EXPECT_EQ(values, "'S', 10.000000");
    EXPECT_TRUE(without.empty());

    append_live_results_double_columns({{"profit_factor", kNan}, {"margin_cushion", -kInf}}, columns,
                                       values, &without);
    EXPECT_EQ(columns, "strategy_id, daily_pnl");
    EXPECT_EQ(values, "'S', 10.000000");
    ASSERT_EQ(without.size(), 2u);
}
