// The results writers against the real tables with a value that is not finite (T-8D R12,
// T-8D-2 R54). Before the guard std::to_string put `nan` or `inf` into the statement unquoted,
// Postgres read it as a column name, and the whole statement failed: a backtest lost its
// results row (and with it the run), a live day its row or its Day T-1 refresh. Now the cell is
// stored without a value and the row is kept.
//
// Database-backed: runs only with TRADE_NGIN_TEST_DSN, on probe ids it deletes before and after.

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {

constexpr const char* kRun = "T8B_FINITE_PROBE_RUN";
constexpr const char* kStrategy = "T8B_FINITE_PROBE_STRATEGY";
constexpr const char* kPortfolio = "T8B_FINITE_PROBE_PORTFOLIO";
const double kNan = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

Timestamp at_utc(int y, int m, int d, int hh) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = hh;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

}  // namespace

class FiniteStatementDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const bool require_db = [] {
            const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
            return v && std::string(v) == "1";
        }();
        const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
        conn_ = dsn ? std::string(dsn) : std::string();
        if (conn_.empty()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set, so the "
                          "writers' guard against a value that is not finite goes unverified";
            }
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set; no database to exercise";
        }
        db_ = std::make_shared<PostgresDatabase>(conn_);
        auto connected = db_->connect();
        if (connected.is_error() || !db_->is_connected()) {
            if (require_db) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable";
            GTEST_SKIP() << "database unreachable; skipping";
        }
        purge();
    }

    void TearDown() override {
        if (!conn_.empty()) purge();
        if (db_ && db_->is_connected()) db_->disconnect();
    }

    void purge() {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        w.exec("DELETE FROM backtest.results WHERE run_id = " + w.quote(kRun));
        w.exec("DELETE FROM trading.live_results WHERE strategy_id = " + w.quote(kStrategy));
        w.commit();
    }

    // One cell of the probe row as text, "NULL" for a NULL.
    std::string cell(const std::string& table, const std::string& column, const std::string& where) {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        auto r = w.exec("SELECT " + column + "::text FROM " + table + " WHERE " + where);
        EXPECT_EQ(r.size(), 1u) << table << " " << where;
        if (r.empty()) return "<no row>";
        return r[0][0].is_null() ? "NULL" : r[0][0].as<std::string>();
    }

    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

// T-8D-2 R54: a backtest metric that is not finite does not fail the results INSERT.
TEST_F(FiniteStatementDbTest, ABacktestMetricThatIsNotFiniteIsStoredNullAndTheRowIsKept) {
    const std::unordered_map<std::string, double> metrics = {
        {"total_return", 0.25},     {"sharpe_ratio", kNan},     {"sortino_ratio", kInf},
        {"max_drawdown", 0.1},      {"calmar_ratio", -kInf},    {"volatility", 0.2},
        {"total_trades", 3.0},      {"win_rate", 0.5},          {"profit_factor", kNan},
        {"avg_win", 1.0},           {"avg_loss", 1.0},          {"max_win", 1.0},
        {"max_loss", 1.0},          {"avg_holding_period", 2.0}, {"var_95", 0.01},
        {"cvar_95", 0.02},          {"beta", 0.0},              {"correlation", 0.0},
        {"downside_volatility", 0.1}, {"transaction_costs", 12.5}, {"roll_costs", kNan},
        {"total_roll_fills", 4.0}};
    auto stored = db_->store_backtest_summary(kRun, at_utc(2024, 1, 1, 0), at_utc(2026, 1, 1, 0),
                                              metrics, kPortfolio, "backtest.results");
    ASSERT_TRUE(stored.is_ok()) << (stored.is_error() ? stored.error()->what() : "");

    const std::string where = "run_id = '" + std::string(kRun) + "'";
    EXPECT_EQ(cell("backtest.results", "sharpe_ratio", where), "NULL");
    EXPECT_EQ(cell("backtest.results", "sortino_ratio", where), "NULL");
    EXPECT_EQ(cell("backtest.results", "calmar_ratio", where), "NULL");
    EXPECT_EQ(cell("backtest.results", "profit_factor", where), "NULL");
    // The finite cells are the values handed in.
    EXPECT_EQ(cell("backtest.results", "total_return", where), "0.25");
    EXPECT_EQ(cell("backtest.results", "total_trades", where), "3");
    EXPECT_EQ(cell("backtest.results", "transaction_costs", where), "12.5");
    // A column with a default (018) takes it.
    EXPECT_EQ(cell("backtest.results", "roll_costs", where), "0");
    EXPECT_EQ(cell("backtest.results", "total_roll_fills", where), "4");
}

// T-8D R12 (b): a metric that is not handed in is no measured 0; its cell is NULL.
TEST_F(FiniteStatementDbTest, ABacktestMetricThatIsNotHandedInIsStoredNullNotZero) {
    auto stored = db_->store_backtest_summary(kRun, at_utc(2024, 1, 1, 0), at_utc(2026, 1, 1, 0),
                                              {{"total_return", 0.25}}, kPortfolio,
                                              "backtest.results");
    ASSERT_TRUE(stored.is_ok()) << (stored.is_error() ? stored.error()->what() : "");
    const std::string where = "run_id = '" + std::string(kRun) + "'";
    EXPECT_EQ(cell("backtest.results", "total_return", where), "0.25");
    EXPECT_EQ(cell("backtest.results", "sharpe_ratio", where), "NULL");
    EXPECT_EQ(cell("backtest.results", "profit_factor", where), "NULL");
    EXPECT_EQ(cell("backtest.results", "transaction_costs", where), "0");
}

// T-8D R12: the live row's INSERT leaves the cell out; the Day T-1 UPDATE writes NULL over a
// stored value.
TEST_F(FiniteStatementDbTest, ALiveValueThatIsNotFiniteIsNullOnTheInsertAndOnTheUpdate) {
    const Timestamp day = at_utc(2026, 5, 4, 5);
    auto stored = db_->store_live_results_complete(
        kStrategy, day, {{"daily_pnl", 10.0}, {"profit_factor", kNan}, {"sortino_ratio", 1.25}},
        {{"total_days", 3}}, nlohmann::json(), kPortfolio, "trading.live_results");
    ASSERT_TRUE(stored.is_ok()) << (stored.is_error() ? stored.error()->what() : "");
    const std::string where = "strategy_id = '" + std::string(kStrategy) + "'";
    EXPECT_EQ(cell("trading.live_results", "profit_factor", where), "NULL");
    EXPECT_EQ(cell("trading.live_results", "sortino_ratio", where), "1.25");
    EXPECT_EQ(cell("trading.live_results", "total_days", where), "3");

    size_t rows = 0;
    auto updated = db_->update_live_results(kStrategy, day,
                                            {{"sortino_ratio", kInf}, {"profit_factor", 2.5}},
                                            kPortfolio, "trading.live_results", &rows);
    ASSERT_TRUE(updated.is_ok()) << (updated.is_error() ? updated.error()->what() : "");
    EXPECT_EQ(rows, 1u);
    EXPECT_EQ(cell("trading.live_results", "sortino_ratio", where), "NULL");
    EXPECT_EQ(cell("trading.live_results", "profit_factor", where), "2.5");
}
