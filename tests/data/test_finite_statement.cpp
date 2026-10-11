// The numbers of a results statement, guarded (T-8D R12, T-8D-2 R54): a double that is not
// finite is never formatted into a statement. The three builders the writers call
// (PostgresDatabase::store_backtest_summary, update_live_results, store_live_results_complete),
// executed on their own; test_finite_statement_db.cpp runs the writers against the tables.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/data/finite_statement.hpp"
#include "trade_ngin/live/live_statistics_columns.hpp"

using namespace trade_ngin;

namespace {
const double kNan = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

// The bits of a double, so that 0.0 and -0.0 are told apart.
uint64_t bits_of(double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// Hand-chosen doubles: values six decimals cannot hold (a third, a 6J-sized price, a billionth,
// nine digits either side of the point), the two zeros, the smallest denormal, the smallest
// normal and the largest double.
const std::vector<double> kRoundTripTable = {
    0.1,
    1.0 / 3.0,
    1e-9,
    123456789.123456789,
    -0.0,
    0.0,
    std::numeric_limits<double>::denorm_min(),
    std::numeric_limits<double>::min(),
    std::numeric_limits<double>::max(),
    0.0062415123456789,
    495186.141684,
    123.45678949,
    -1.0274012134567891,
    -2.25,
};
}  // namespace

// D-16: the text of a stored number reads back as the same double, bit for bit.
TEST(FiniteStatement, AFiniteNumberIsTextThatReadsBackAsTheSameDoubleBitForBit) {
    for (const double value : kRoundTripTable) {
        const auto text = finite_sql_number(value);
        ASSERT_TRUE(text.has_value()) << value;
        EXPECT_EQ(bits_of(std::strtod(text->c_str(), nullptr)), bits_of(value))
            << "text " << *text << " of " << std::setprecision(17) << value;
    }
    // The text is the shortest of 15, 16 or 17 significant digits that reads back.
    EXPECT_EQ(finite_sql_number(0.1), std::optional<std::string>("0.1"));
    EXPECT_EQ(finite_sql_number(1.5), std::optional<std::string>("1.5"));
    EXPECT_EQ(finite_sql_number(0.0), std::optional<std::string>("0"));
    EXPECT_EQ(finite_sql_number(-0.0), std::optional<std::string>("-0"));
    EXPECT_EQ(finite_sql_number(1.0 / 3.0), std::optional<std::string>("0.3333333333333333"));
    EXPECT_EQ(finite_sql_number(1e-9), std::optional<std::string>("1e-09"));
    EXPECT_EQ(finite_sql_number(123456789.123456789),
              std::optional<std::string>("123456789.12345679"));
    EXPECT_EQ(finite_sql_number(0.0062415123456789),
              std::optional<std::string>("0.0062415123456789"));
    EXPECT_EQ(finite_sql_number(std::numeric_limits<double>::denorm_min()),
              std::optional<std::string>("4.94065645841247e-324"));
}

// T-8D R12: a value that is not finite is still no value.
TEST(FiniteStatement, ANumberThatIsNotFiniteIsNoValue) {
    EXPECT_FALSE(finite_sql_number(kNan).has_value());
    EXPECT_FALSE(finite_sql_number(kInf).has_value());
    EXPECT_FALSE(finite_sql_number(-kInf).has_value());
    EXPECT_FALSE(live_results_number(kNan).has_value());
    EXPECT_FALSE(live_results_number(kInf).has_value());
    EXPECT_FALSE(live_results_number(-kInf).has_value());
}

// The typed cells of migrations 030 and 031 carry the same text: it reads back as the same
// double (it was ten significant digits).
TEST(FiniteStatement, ATypedCellsNumberReadsBackAsTheSameDoubleBitForBit) {
    for (const double value : kRoundTripTable) {
        const auto text = live_results_number(value);
        ASSERT_TRUE(text.has_value()) << value;
        EXPECT_EQ(bits_of(std::strtod(text->c_str(), nullptr)), bits_of(value))
            << "text " << *text << " of " << std::setprecision(17) << value;
        EXPECT_EQ(text, finite_sql_number(value));
    }
}

// backtest.results: a finite run's VALUES are the round-trip text of each metric in the column
// order; a metric that is not finite, or not handed in, is DEFAULT and is named.
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
              "0.616818, 1.20523, 1.786723, 2.306329, 9876.5");
    EXPECT_TRUE(without.empty());

    const std::string guarded = backtest_summary_metric_values(
        names,
        {{"total_return", 0.1}, {"sharpe_ratio", kNan}, {"sortino_ratio", kInf}, {"transaction_costs", -kInf}},
        &without);
    EXPECT_EQ(guarded, "0.1, DEFAULT, DEFAULT, DEFAULT, DEFAULT");
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
              "sharpe_ratio = 1.5");
    EXPECT_TRUE(without.empty());
    // A cumulative cost keeps the decimals six places dropped.
    EXPECT_EQ(live_results_update_assignments({{"total_transaction_costs", 123.45678949}}, &without),
              "total_transaction_costs = 123.45678949");
    EXPECT_TRUE(without.empty());
    EXPECT_EQ(live_results_update_assignments({{"profit_factor", kNan}}, &without),
              "profit_factor = NULL");
    EXPECT_EQ(without, std::vector<std::string>{"profit_factor"});
    EXPECT_EQ(live_results_update_assignments({}, &without), "");

    without.clear();
    const std::string mixed =
        live_results_update_assignments({{"volatility", 12.5}, {"sortino_ratio", kInf}}, &without);
    EXPECT_NE(mixed.find("volatility = 12.5"), std::string::npos) << mixed;
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
    EXPECT_EQ(values, "'S', 10");
    EXPECT_TRUE(without.empty());

    append_live_results_double_columns({{"profit_factor", kNan}, {"margin_cushion", -kInf}}, columns,
                                       values, &without);
    EXPECT_EQ(columns, "strategy_id, daily_pnl");
    EXPECT_EQ(values, "'S', 10");
    ASSERT_EQ(without.size(), 2u);
}
