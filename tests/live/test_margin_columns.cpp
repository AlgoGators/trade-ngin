// tests/live/test_margin_columns.cpp
//
// live/margin_columns.hpp -- the margin and leverage cells of a trading.live_results row
// (T-8D R21, section 3.14; POST_AUDIT_FIXES P-6).
//
// Two layers:
//   * the values the day's own run writes: the ratio and the cushion have no value on a day
//     with no posted margin and are then left out of the INSERT (a NULL cell, never 0 or -1);
//   * the Day T-1 finalize's SET fragment, run over a TEMP table shaped like the columns of
//     trading.live_results it touches, through TRADE_NGIN_TEST_DSN only. A temp table lives in
//     this session and is dropped with the aborted transaction: nothing in the database is
//     written. Reachability gate as in tests/data/test_futures_bar_dedup_db.cpp.

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "trade_ngin/live/margin_columns.hpp"

using namespace trade_ngin;

TEST(MarginColumns, TheRatioIsThePortfolioValueOverThePostedMargin) {
    const auto ratio = equity_to_margin_ratio_of(497239.3978, 45997.05);
    ASSERT_TRUE(ratio.has_value());
    EXPECT_DOUBLE_EQ(*ratio, 497239.3978 / 45997.05);
}

TEST(MarginColumns, TheRatioHasNoValueWithoutPostedMargin) {
    EXPECT_FALSE(equity_to_margin_ratio_of(99336.3158, 0.0).has_value());
    EXPECT_FALSE(equity_to_margin_ratio_of(99336.3158, -1.0).has_value());
}

TEST(MarginColumns, TheCushionIsTheShareOfTheValueAboveTheMaintenanceRequirement) {
    const auto cushion = margin_cushion_of(500000.0, 46000.0, 40000.0);
    ASSERT_TRUE(cushion.has_value());
    EXPECT_DOUBLE_EQ(*cushion, (500000.0 - 40000.0) / 500000.0);
    // A requirement of zero on a book that posts margin is a cushion of the whole value.
    const auto whole = margin_cushion_of(100000.0, 16000.0, 0.0);
    ASSERT_TRUE(whole.has_value());
    EXPECT_DOUBLE_EQ(*whole, 1.0);
}

TEST(MarginColumns, TheCushionHasNoValueWithoutPostedMarginAndIsNeverMinusOne) {
    const auto flat = margin_cushion_of(99336.3158, 0.0, 0.0);
    EXPECT_FALSE(flat.has_value());
    EXPECT_FALSE(margin_cushion_of(0.0, 46000.0, 40000.0).has_value());
}

TEST(MarginColumns, ACellWithoutAValueIsLeftOutOfTheDaysInsert) {
    std::unordered_map<std::string, double> metrics = {{"margin_posted", 0.0}};
    set_margin_cells(metrics, std::nullopt, std::nullopt);
    EXPECT_EQ(metrics.count("equity_to_margin_ratio"), 0u);
    EXPECT_EQ(metrics.count("margin_cushion"), 0u);
    EXPECT_EQ(metrics.size(), 1u);

    set_margin_cells(metrics, 10.81, 0.919);
    EXPECT_DOUBLE_EQ(metrics.at("equity_to_margin_ratio"), 10.81);
    EXPECT_DOUBLE_EQ(metrics.at("margin_cushion"), 0.919);

    // A later call without a value removes the earlier one: no stale figure is stored.
    set_margin_cells(metrics, std::nullopt, 0.5);
    EXPECT_EQ(metrics.count("equity_to_margin_ratio"), 0u);
    EXPECT_DOUBLE_EQ(metrics.at("margin_cushion"), 0.5);
}

TEST(MarginColumns, TheFinalizeFragmentAssignsTheFourCellsAndNothingElse) {
    const std::string sql = finalize_margin_columns_sql("V", "M");
    auto count = [&](const std::string& needle) {
        size_t n = 0;
        for (size_t p = sql.find(needle); p != std::string::npos; p = sql.find(needle, p + 1)) ++n;
        return n;
    };
    EXPECT_EQ(sql.rfind("portfolio_leverage = ", 0), 0u);
    EXPECT_EQ(count("net_leverage = "), 1u);
    EXPECT_EQ(count("equity_to_margin_ratio = "), 1u);
    EXPECT_EQ(count("margin_cushion = "), 1u);
    EXPECT_EQ(count(" = CASE"), 4u);
    EXPECT_EQ(sql.substr(sql.size() - 2), ", ");
    // No cell keeps what the day's own run wrote.
    EXPECT_EQ(count("IS NULL OR"), 0u);
    EXPECT_EQ(count("ELSE portfolio_leverage"), 0u);
    EXPECT_EQ(count("ELSE equity_to_margin_ratio"), 0u);
    // The value is rounded as current_portfolio_value stores it.
    EXPECT_GE(count("CAST((V) AS numeric(15,4))"), 1u);
}

namespace {

std::string discover_connection_string() {
    const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
    if (dsn && *dsn) return std::string(dsn);
    return {};
}

std::optional<double> cell(const pqxx::row& r, const char* column) {
    if (r[column].is_null()) return std::nullopt;
    return r[column].as<double>();
}

}  // namespace

class MarginColumnsFinalizeDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const bool require_db = [] {
            const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
            return v && std::string(v) == "1";
        }();
        const std::string conn = discover_connection_string();
        if (conn.empty()) {
            if (require_db) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set";
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        }
        try {
            c_ = std::make_unique<pqxx::connection>(conn);
        } catch (const std::exception& e) {
            if (require_db) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable: "
                                   << e.what();
            GTEST_SKIP() << "database unreachable";
        }
    }

    // The columns of trading.live_results the finalize reads and writes, with their types.
    static void create(pqxx::work& w) {
        w.exec("CREATE TEMP TABLE fake_live_results (id text PRIMARY KEY, "
               "current_portfolio_value numeric(15,4), gross_notional numeric, "
               "net_notional numeric, margin_posted double precision, "
               "cash_available double precision, equity_to_margin_ratio double precision, "
               "margin_cushion double precision, portfolio_leverage numeric(8,4), "
               "net_leverage numeric(8,4), gross_leverage numeric(8,4)) ON COMMIT DROP");
    }

    // The finalize: the value settled, the four cells and the cash on it, in one statement.
    // `with_gross_leverage` adds the clause of a book that stores gross_leverage (the equity
    // book).
    static void finalize(pqxx::work& w, const std::string& id, const std::string& value_sql,
                         const std::string& maintenance_sql, bool with_gross_leverage = false) {
        w.exec("UPDATE fake_live_results SET current_portfolio_value = " + value_sql + ", " +
               finalize_margin_columns_sql(value_sql, maintenance_sql) +
               (with_gross_leverage ? finalize_gross_leverage_sql(value_sql) : std::string()) +
               "cash_available = " + value_sql + " - COALESCE(margin_posted, 0.0) WHERE id = '" +
               id + "'");
    }

    std::unique_ptr<pqxx::connection> c_;
};

TEST_F(MarginColumnsFinalizeDbTest, APositionedRowEndsOnTheFinalisedValueFromItsOwnStoredCells) {
    pqxx::work w(*c_);
    create(w);
    // CONSERVATIVE 2026-04-24 as the day's own run wrote it: value 497,239.3978.
    w.exec("INSERT INTO fake_live_results VALUES ('fut', 497239.3978, 793465.000000, "
           "203940.000000, 45997.05, 451242.347804, 10.813756, 0.919311, 1.5952, 0.4100)");
    // The finalize adds the day's P&L; the unrounded expression has more than four decimals.
    const std::string value_sql = "497239.3978 + (1234.567891 - 0.0)";
    finalize(w, "fut", value_sql, "41815.500000");
    const pqxx::result res = w.exec("SELECT * FROM fake_live_results WHERE id = 'fut'");
    ASSERT_EQ(res.size(), 1u);
    const auto r = res[0];

    const double value = 498473.9657;  // 498,473.965691 as numeric(15,4) stores it
    EXPECT_DOUBLE_EQ(r["current_portfolio_value"].as<double>(), value);
    EXPECT_DOUBLE_EQ(*cell(r, "equity_to_margin_ratio"), value / 45997.05);
    EXPECT_DOUBLE_EQ(*cell(r, "margin_cushion"), (value - 41815.5) / value);
    // numeric(8,4): 793465 / 498473.9657 = 1.59178...; 203940 / 498473.9657 = 0.40912...
    EXPECT_DOUBLE_EQ(*cell(r, "portfolio_leverage"), std::round(793465.0 / value * 1e4) / 1e4);
    EXPECT_DOUBLE_EQ(*cell(r, "net_leverage"), std::round(203940.0 / value * 1e4) / 1e4);
    // The stored inputs did not move.
    EXPECT_DOUBLE_EQ(r["margin_posted"].as<double>(), 45997.05);
    EXPECT_DOUBLE_EQ(r["gross_notional"].as<double>(), 793465.0);
    EXPECT_DOUBLE_EQ(r["net_notional"].as<double>(), 203940.0);
}

TEST_F(MarginColumnsFinalizeDbTest, ARowWithNoMarginEndsWithANullRatioAndANullCushion) {
    pqxx::work w(*c_);
    create(w);
    // A flat day as it was stored before: the ratio 0 and the cushion -1.
    w.exec("INSERT INTO fake_live_results VALUES ('flat', 99336.3158, 0.000000, 0.000000, 0, "
           "99336.315768, 0, -1, 0.0000, 0.0000)");
    // A flat day as it is stored now: both cells NULL. The finalize must not write 0 over them.
    w.exec("INSERT INTO fake_live_results VALUES ('null', 99336.3158, 0.000000, 0.000000, 0, "
           "99336.315768, NULL, NULL, 0.0000, 0.0000)");
    for (const std::string id : {"flat", "null"}) {
        finalize(w, id, "99336.3158 + 0.0", "COALESCE(margin_posted, 0.0)");
        const pqxx::result res = w.exec("SELECT * FROM fake_live_results WHERE id = '" + id + "'");
        ASSERT_EQ(res.size(), 1u);
        const auto r = res[0];
        EXPECT_FALSE(cell(r, "equity_to_margin_ratio").has_value()) << id;
        EXPECT_FALSE(cell(r, "margin_cushion").has_value()) << id;
        EXPECT_DOUBLE_EQ(*cell(r, "portfolio_leverage"), 0.0) << id;
        EXPECT_DOUBLE_EQ(*cell(r, "net_leverage"), 0.0) << id;
        EXPECT_DOUBLE_EQ(r["cash_available"].as<double>(), 99336.3158) << id;
    }
}

TEST_F(MarginColumnsFinalizeDbTest, TheMaintenanceRequirementMayBeAnExpressionOverTheRow) {
    pqxx::work w(*c_);
    create(w);
    // EQUITY_MR 2026-06-15 as the day's own run wrote it; its maintenance is its posted margin.
    w.exec("INSERT INTO fake_live_results VALUES ('eq', 99327.9760, 15899.203254, 15899.203254, "
           "15899.203254, 83428.772729, 1, 0.839932, 0.1601, 0.1604)");
    finalize(w, "eq", "100000.000000 + (-214.132000)", "COALESCE(margin_posted, 0.0)");
    const pqxx::result res = w.exec("SELECT * FROM fake_live_results WHERE id = 'eq'");
    ASSERT_EQ(res.size(), 1u);
    const auto r = res[0];
    const double value = 99785.868;
    EXPECT_DOUBLE_EQ(*cell(r, "equity_to_margin_ratio"), value / 15899.203254);
    EXPECT_DOUBLE_EQ(*cell(r, "margin_cushion"), (value - 15899.203254) / value);
    EXPECT_DOUBLE_EQ(*cell(r, "portfolio_leverage"), 0.1593);
    EXPECT_DOUBLE_EQ(*cell(r, "net_leverage"), 0.1593);
}

TEST_F(MarginColumnsFinalizeDbTest, AValueOfZeroDoesNotFailTheStatement) {
    pqxx::work w(*c_);
    create(w);
    w.exec("INSERT INTO fake_live_results VALUES ('zero', 0.0, 1000.0, 1000.0, 500.0, 0, 0, 0, "
           "0, 0)");
    EXPECT_NO_THROW(finalize(w, "zero", "0.0", "400.0"));
    const pqxx::result res = w.exec("SELECT * FROM fake_live_results WHERE id = 'zero'");
    ASSERT_EQ(res.size(), 1u);
    const auto r = res[0];
    EXPECT_DOUBLE_EQ(*cell(r, "portfolio_leverage"), 0.0);
    EXPECT_DOUBLE_EQ(*cell(r, "equity_to_margin_ratio"), 0.0);
    EXPECT_FALSE(cell(r, "margin_cushion").has_value());
}

// T-8D R22: the equity book's gross_leverage is its gross notional over the finalised value;
// a futures row, whose finalize carries no such clause, keeps the column NULL.
TEST(MarginColumns, TheGrossLeverageClauseAssignsThatOneCell) {
    const std::string sql = finalize_gross_leverage_sql("V");
    EXPECT_EQ(sql, "gross_leverage = CASE WHEN CAST((V) AS numeric(15,4)) > 0 THEN gross_notional "
                   "/ CAST((V) AS numeric(15,4)) ELSE 0.0 END, ");
    EXPECT_EQ(finalize_margin_columns_sql("V", "M").find("gross_leverage"), std::string::npos);
}

TEST_F(MarginColumnsFinalizeDbTest, TheEquityRowsGrossLeverageIsRecomputedAndAFuturesRowsStaysNull) {
    pqxx::work w(*c_);
    create(w);
    // EQUITY_MR 2026-06-15 with the risk report's leverage figures (0.1604) as stored before.
    w.exec("INSERT INTO fake_live_results VALUES ('eq', 99327.9760, 15899.203254, 15899.203254, "
           "16042.838059, 83285.137941, 6.19, 1, 0.1601, 0.1604, 0.1604)");
    w.exec("INSERT INTO fake_live_results VALUES ('fut', 497239.3978, 793465.000000, "
           "203940.000000, 45997.05, 451242.347804, 10.813756, 0.919311, 1.5952, 0.4100, NULL)");
    finalize(w, "eq", "100000.000000 + (-214.132000)", "0.0", true);
    finalize(w, "fut", "497239.3978 + 0.0", "40135.010000");

    const pqxx::result eq = w.exec("SELECT * FROM fake_live_results WHERE id = 'eq'");
    ASSERT_EQ(eq.size(), 1u);
    // 15899.203254 / 99785.8680 = 0.15933...
    EXPECT_DOUBLE_EQ(*cell(eq[0], "gross_leverage"), 0.1593);
    EXPECT_DOUBLE_EQ(*cell(eq[0], "net_leverage"), 0.1593);
    EXPECT_DOUBLE_EQ(*cell(eq[0], "portfolio_leverage"), 0.1593);
    EXPECT_DOUBLE_EQ(*cell(eq[0], "margin_cushion"), 1.0);

    const pqxx::result fut = w.exec("SELECT * FROM fake_live_results WHERE id = 'fut'");
    ASSERT_EQ(fut.size(), 1u);
    EXPECT_FALSE(cell(fut[0], "gross_leverage").has_value());
    EXPECT_DOUBLE_EQ(*cell(fut[0], "portfolio_leverage"), 1.5957);
}
