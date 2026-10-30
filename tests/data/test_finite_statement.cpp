// The numbers of a results statement, guarded: a double that is not
// finite is never formatted into a statement. The three builders the writers call
// (PostgresDatabase::store_backtest_summary, update_live_results, store_live_results_complete),
// executed on their own, and the three writers themselves on a database that was never
// connected, where a NOT NULL column that is not finite refuses the row before anything is
// sent; test_finite_statement_db.cpp runs the writers against the tables.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/finite_statement.hpp"
#include "trade_ngin/data/postgres_database.hpp"
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

// The text of a stored number reads back as the same double, bit for bit.
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

// A value that is not finite is no value.
TEST(FiniteStatement, ANumberThatIsNotFiniteIsNoValue) {
    EXPECT_FALSE(finite_sql_number(kNan).has_value());
    EXPECT_FALSE(finite_sql_number(kInf).has_value());
    EXPECT_FALSE(finite_sql_number(-kInf).has_value());
    EXPECT_FALSE(live_results_number(kNan).has_value());
    EXPECT_FALSE(live_results_number(kInf).has_value());
    EXPECT_FALSE(live_results_number(-kInf).has_value());
}

// The typed cells of migrations 036 and 037 carry the same text: it reads back as the same
// double (not ten significant digits).
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
// order; a metric that is not finite, or not handed in, is DEFAULT and is named. (The builder
// alone: the writer refuses the row before it when a NOT NULL metric, transaction_costs among
// them, is not finite.)
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

// trading.live_results, the day's INSERT: a value that is not finite is written NULL by name.
// Left out, its cell would take the column's default, and ten nullable columns of the table
// default to 0.
TEST(FiniteStatement, TheLiveInsertWritesNullByNameForAValueThatIsNotFinite) {
    std::string columns = "strategy_id", values = "'S'";
    std::vector<std::string> without;
    append_live_results_double_columns({{"daily_pnl", 10.0}}, columns, values, &without);
    EXPECT_EQ(columns, "strategy_id, daily_pnl");
    EXPECT_EQ(values, "'S', 10");
    EXPECT_TRUE(without.empty());

    append_live_results_double_columns({{"profit_factor", kNan}}, columns, values, &without);
    EXPECT_EQ(columns, "strategy_id, daily_pnl, profit_factor");
    EXPECT_EQ(values, "'S', 10, NULL");
    EXPECT_EQ(without, std::vector<std::string>{"profit_factor"});

    append_live_results_double_columns({{"daily_return", -kInf}}, columns, values, &without);
    EXPECT_EQ(columns, "strategy_id, daily_pnl, profit_factor, daily_return");
    EXPECT_EQ(values, "'S', 10, NULL, NULL");
    EXPECT_EQ(without, (std::vector<std::string>{"profit_factor", "daily_return"}));
    EXPECT_EQ(values.find("nan"), std::string::npos);
    EXPECT_EQ(values.find("inf"), std::string::npos);
}

// The refusal text: every NOT NULL column handed a value that is not finite, with the value,
// in the list's order; nothing for a finite value, a column not handed in, or a nullable column.
TEST(FiniteStatement, ANotNullColumnThatIsNotFiniteIsNamedWithItsValueInTheRefusal) {
    const auto& live = live_results_not_null_columns();
    const auto& backtest = backtest_results_not_null_metrics();
    EXPECT_EQ(live, (std::vector<std::string>{"daily_roll_costs", "total_roll_costs"}));
    EXPECT_EQ(backtest,
              (std::vector<std::string>{"transaction_costs", "roll_costs", "total_roll_fills"}));
    EXPECT_EQ(non_finite_text(kNan), "nan");
    EXPECT_EQ(non_finite_text(-kNan), "nan");
    EXPECT_EQ(non_finite_text(kInf), "inf");
    EXPECT_EQ(non_finite_text(-kInf), "-inf");

    EXPECT_EQ(non_finite_not_null_refusal("T INSERT", {{"daily_roll_costs", 1.5}}, live), "");
    EXPECT_EQ(non_finite_not_null_refusal("T INSERT", {{"daily_pnl", 1.5}}, live), "");
    EXPECT_EQ(non_finite_not_null_refusal("T INSERT", {{"sharpe_ratio", kNan}, {"daily_pnl", kInf}},
                                          live),
              "");
    EXPECT_EQ(non_finite_not_null_refusal("T INSERT", {}, live), "");

    EXPECT_EQ(non_finite_not_null_refusal("trading.live_results INSERT for S on D",
                                          {{"daily_roll_costs", kNan}, {"daily_pnl", 2.0}}, live),
              "REFUSED trading.live_results INSERT for S on D: NOT NULL column(s) not finite: "
              "daily_roll_costs=nan; no statement is sent, no row is stored or changed");
    EXPECT_EQ(non_finite_not_null_refusal(
                  "B", {{"total_roll_fills", -kInf}, {"transaction_costs", kInf}, {"roll_costs", kNan},
                        {"sharpe_ratio", kNan}},
                  backtest),
              "REFUSED B: NOT NULL column(s) not finite: transaction_costs=inf, roll_costs=nan, "
              "total_roll_fills=-inf; no statement is sent, no row is stored or changed");
}

namespace {
// The three values that are not finite, with the text a refusal names each by.
const std::vector<std::pair<double, std::string>> kNotFinite = {
    {kNan, "nan"}, {kInf, "inf"}, {-kInf, "-inf"}};
// The NOT NULL number columns the two statements can be handed (migrations 017 and 018), written
// out here so that the tests do not read the list the writer reads.
const std::vector<std::string> kLiveNotNull = {"daily_roll_costs", "total_roll_costs"};
const std::vector<std::string> kBacktestNotNull = {"transaction_costs", "roll_costs",
                                                   "total_roll_fills"};

// A database that was never connected: any statement would have to pass validate_connection,
// which returns CONNECTION_ERROR "Not connected to database". A refusal comes before it, so an
// error that names the column proves no statement was built or sent.
PostgresDatabase never_connected() {
    return PostgresDatabase("host=127.0.0.1 port=1 dbname=none_never_connected");
}

void expect_refusal(const Result<void>& result, const std::string& column,
                    const std::string& value_text) {
    ASSERT_TRUE(result.is_error()) << column << " " << value_text;
    EXPECT_EQ(result.error()->code(), ErrorCode::DATABASE_ERROR) << column << " " << value_text;
    const std::string what = result.error()->what();
    EXPECT_NE(what.find("REFUSED"), std::string::npos) << what;
    EXPECT_NE(what.find(column + "=" + value_text), std::string::npos) << what;
    EXPECT_EQ(what.find("Not connected"), std::string::npos) << what;
}
}  // namespace

// The live day's INSERT (store_live_results_complete), executed: each NOT NULL column of
// trading.live_results, handed NaN, +inf and -inf, refuses the row with DATABASE_ERROR (the code
// the failed statement returned) and the column and value named. A finite value in the same
// columns goes on to the connection check.
TEST(FiniteStatementWriter, TheLiveInsertRefusesANotNullColumnThatIsNotFinite) {
    auto db = never_connected();
    const Timestamp day = std::chrono::system_clock::from_time_t(1777870800);  // 2026-05-04
    for (const auto& column : kLiveNotNull) {
        for (const auto& [value, text] : kNotFinite) {
            auto stored = db.store_live_results_complete(
                "PROBE", day, {{"daily_pnl", 10.0}, {column, value}, {"sortino_ratio", 1.25}},
                {{"total_days", 3}}, nlohmann::json(), "PROBE_PORTFOLIO", "trading.live_results");
            expect_refusal(stored, column, text);
            EXPECT_NE(std::string(stored.error()->what()).find("trading.live_results INSERT"),
                      std::string::npos);
        }
    }
    auto finite = db.store_live_results_complete(
        "PROBE", day, {{"daily_roll_costs", 1.0}, {"total_roll_costs", 2.0}, {"sharpe_ratio", kNan}},
        {}, nlohmann::json(), "PROBE_PORTFOLIO", "trading.live_results");
    ASSERT_TRUE(finite.is_error());
    EXPECT_EQ(finite.error()->code(), ErrorCode::CONNECTION_ERROR);
}

// The Day T-1 UPDATE (update_live_results), executed: the same refusal, the same line, and
// rows_affected stays 0.
TEST(FiniteStatementWriter, TheLiveUpdateRefusesANotNullColumnThatIsNotFinite) {
    auto db = never_connected();
    const Timestamp day = std::chrono::system_clock::from_time_t(1777870800);
    for (const auto& column : kLiveNotNull) {
        for (const auto& [value, text] : kNotFinite) {
            size_t rows = 7;
            auto updated = db.update_live_results("PROBE", day,
                                                  {{column, value}, {"sortino_ratio", 2.5}},
                                                  "PROBE_PORTFOLIO", "trading.live_results", &rows);
            expect_refusal(updated, column, text);
            EXPECT_EQ(rows, 0u);
            EXPECT_NE(std::string(updated.error()->what()).find("trading.live_results UPDATE"),
                      std::string::npos);
        }
    }
    auto finite = db.update_live_results("PROBE", day,
                                         {{"total_roll_costs", 2.0}, {"sortino_ratio", kInf}},
                                         "PROBE_PORTFOLIO", "trading.live_results");
    ASSERT_TRUE(finite.is_error());
    EXPECT_EQ(finite.error()->code(), ErrorCode::CONNECTION_ERROR);
}

// The backtest summary INSERT (store_backtest_summary), executed: each NOT NULL metric of
// backtest.results refuses the row. A NOT NULL metric that is not handed in refuses nothing.
TEST(FiniteStatementWriter, TheBacktestSummaryRefusesANotNullMetricThatIsNotFinite) {
    auto db = never_connected();
    const Timestamp start = std::chrono::system_clock::from_time_t(1704067200);  // 2024-01-01
    const Timestamp end = std::chrono::system_clock::from_time_t(1767225600);    // 2026-01-01
    for (const auto& column : kBacktestNotNull) {
        for (const auto& [value, text] : kNotFinite) {
            std::unordered_map<std::string, double> metrics = {
                {"total_return", 0.25}, {"transaction_costs", 12.5}, {"roll_costs", 1.5},
                {"total_roll_fills", 4.0}};
            metrics[column] = value;
            auto stored = db.store_backtest_summary("PROBE_RUN", start, end, metrics,
                                                    "PROBE_PORTFOLIO", "backtest.results");
            expect_refusal(stored, column, text);
            EXPECT_NE(std::string(stored.error()->what())
                          .find("backtest.results INSERT for run_id PROBE_RUN"),
                      std::string::npos);
        }
    }
    auto absent = db.store_backtest_summary("PROBE_RUN", start, end,
                                            {{"total_return", 0.25}, {"sharpe_ratio", kNan}},
                                            "PROBE_PORTFOLIO", "backtest.results");
    ASSERT_TRUE(absent.is_error());
    EXPECT_EQ(absent.error()->code(), ErrorCode::CONNECTION_ERROR);
}
