// The results writers against the real tables with a value that is not finite (T-8D R12,
// T-8D-2 R54). Before the guard std::to_string put `nan` or `inf` into the statement unquoted,
// Postgres read it as a column name, and the whole statement failed: a backtest lost its
// results row (and with it the run), a live day its row or its Day T-1 refresh. Now the cell is
// stored without a value and the row is kept.
//
// The same writers, and the backtest curve, the backtest position rows and the live curve's
// Day T-1 refresh, store a finite value as the double they were handed (D-16): the cell reads
// back bit for bit where the column is double precision, and rounded once, from the full value,
// where it is numeric(p,s).
//
// Database-backed: runs only with TRADE_NGIN_TEST_DSN, on probe ids it deletes before and after.

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
        // The curve's rows reference the run's results row: they go first.
        w.exec("DELETE FROM backtest.equity_curve WHERE run_id = " + w.quote(kRun));
        w.exec("DELETE FROM backtest.final_positions WHERE run_id = " + w.quote(kRun));
        w.exec("DELETE FROM backtest.results WHERE run_id = " + w.quote(kRun));
        w.exec("DELETE FROM trading.live_results WHERE strategy_id = " + w.quote(kStrategy));
        w.exec("DELETE FROM trading.equity_curve WHERE strategy_id = " + w.quote(kStrategy));
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

    // One cell of the probe row as the double its text reads to (Postgres prints a double
    // precision cell as the shortest text that reads back, and a numeric cell exactly).
    double number(const std::string& table, const std::string& column, const std::string& where) {
        return std::strtod(cell(table, column, where).c_str(), nullptr);
    }

    static uint64_t bits_of(double value) {
        uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
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

// D-16, backtest.results: every metric cell is the double handed in, not its six decimals.
TEST_F(FiniteStatementDbTest, ABacktestSummaryStoresEachMetricAsTheDoubleHandedIn) {
    const double third = 1.0 / 3.0;
    const double tiny = std::numeric_limits<double>::denorm_min();
    const std::unordered_map<std::string, double> metrics = {
        {"total_return", third},          {"sharpe_ratio", 1.2052345678912345},
        {"sortino_ratio", 1e-9},          {"max_drawdown", 0.0062415123456789},
        {"calmar_ratio", tiny},           {"volatility", 0.1},
        {"total_trades", 3.0},            {"transaction_costs", 123456789.123456789},
        {"roll_costs", 123.45678949},     {"total_roll_fills", 4.0}};
    auto stored = db_->store_backtest_summary(kRun, at_utc(2024, 1, 1, 0), at_utc(2026, 1, 1, 0),
                                              metrics, kPortfolio, "backtest.results");
    ASSERT_TRUE(stored.is_ok()) << (stored.is_error() ? stored.error()->what() : "");
    const std::string where = "run_id = '" + std::string(kRun) + "'";
    for (const auto& [column, value] : metrics) {
        EXPECT_EQ(bits_of(number("backtest.results", column, where)), bits_of(value))
            << column << " stored " << cell("backtest.results", column, where);
    }
    EXPECT_EQ(cell("backtest.results", "total_return", where), "0.3333333333333333");
    EXPECT_EQ(cell("backtest.results", "transaction_costs", where), "123456789.12345679");
}

// D-16, backtest.equity_curve: a point's level is the double handed in.
TEST_F(FiniteStatementDbTest, ABacktestCurvePointStoresTheLevelHandedIn) {
    const std::vector<std::pair<Timestamp, double>> points = {
        {at_utc(2026, 5, 4, 0), 500000.0},
        {at_utc(2026, 5, 5, 0), 495186.14168412345},
        {at_utc(2026, 5, 6, 0), 1500000.0 / 3.0 + 1.0 / 3.0}};
    // A curve row references its run's results row.
    auto run = db_->store_backtest_summary(kRun, at_utc(2026, 5, 4, 0), at_utc(2026, 5, 6, 0),
                                           {{"total_return", 0.25}}, kPortfolio,
                                           "backtest.results");
    ASSERT_TRUE(run.is_ok()) << (run.is_error() ? run.error()->what() : "");
    auto stored = db_->store_backtest_equity_curve_batch(kRun, points, kPortfolio,
                                                         "backtest.equity_curve");
    ASSERT_TRUE(stored.is_ok()) << (stored.is_error() ? stored.error()->what() : "");
    const char* days[] = {"2026-05-04", "2026-05-05", "2026-05-06"};
    for (size_t i = 0; i < points.size(); ++i) {
        const std::string where = "run_id = '" + std::string(kRun) + "' AND DATE(timestamp) = '" +
                                  days[i] + "'";
        EXPECT_EQ(bits_of(number("backtest.equity_curve", "equity", where)),
                  bits_of(points[i].second))
            << days[i] << " stored " << cell("backtest.equity_curve", "equity", where);
    }
}

// D-16, backtest.final_positions: a 6J-sized average price keeps every digit the engine held
// (six decimals kept about four significant digits), and so do the two P&L cells.
TEST_F(FiniteStatementDbTest, ABacktestPositionRowStoresItsFourNumbersAsHeld) {
    const Position held("6J.v.0", Quantity(3.0), Price(0.00624151), Decimal(1234.56789012),
                        Decimal(-0.33333333), at_utc(2026, 5, 4, 0));
    auto stored = db_->store_backtest_positions({held}, std::string(kRun) + "|PROBE", kPortfolio,
                                                "backtest.final_positions");
    ASSERT_TRUE(stored.is_ok()) << (stored.is_error() ? stored.error()->what() : "");
    const std::string where = "run_id = '" + std::string(kRun) + "'";
    EXPECT_EQ(cell("backtest.final_positions", "quantity", where), "3");
    EXPECT_EQ(cell("backtest.final_positions", "average_price", where), "0.00624151");
    EXPECT_EQ(cell("backtest.final_positions", "unrealized_pnl", where), "1234.56789012");
    EXPECT_EQ(cell("backtest.final_positions", "realized_pnl", where), "-0.33333333");
    EXPECT_EQ(bits_of(number("backtest.final_positions", "average_price", where)),
              bits_of(static_cast<double>(held.average_price)));
    EXPECT_EQ(bits_of(number("backtest.final_positions", "unrealized_pnl", where)),
              bits_of(static_cast<double>(held.unrealized_pnl)));
    EXPECT_EQ(bits_of(number("backtest.final_positions", "realized_pnl", where)),
              bits_of(static_cast<double>(held.realized_pnl)));
}

// D-16, trading.live_results: a double precision cell is the double handed in; a numeric(p,s)
// cell is that double rounded ONCE to the column's scale (through six decimals 1.23454999 went
// to 1.234550 and then to 1.2346); the Day T-1 UPDATE writes the same way.
TEST_F(FiniteStatementDbTest, ALiveRowStoresTheValueHandedInOnTheInsertAndOnTheUpdate) {
    const Timestamp day = at_utc(2026, 5, 4, 5);
    const double third = 1.0 / 3.0;
    auto stored = db_->store_live_results_complete(
        kStrategy, day,
        {{"sharpe_ratio", third},                  // double precision
         {"total_transaction_costs", 123.45678949},  // numeric(20,8)
         {"total_pnl", 1.23454999},                // numeric(15,4)
         {"gross_notional", 793465.1234567891}},   // numeric
        {{"total_days", 3}}, nlohmann::json(), kPortfolio, "trading.live_results");
    ASSERT_TRUE(stored.is_ok()) << (stored.is_error() ? stored.error()->what() : "");
    const std::string where = "strategy_id = '" + std::string(kStrategy) + "'";
    EXPECT_EQ(bits_of(number("trading.live_results", "sharpe_ratio", where)), bits_of(third));
    EXPECT_EQ(cell("trading.live_results", "total_transaction_costs", where), "123.45678949");
    EXPECT_EQ(cell("trading.live_results", "total_pnl", where), "1.2345");
    EXPECT_EQ(bits_of(number("trading.live_results", "gross_notional", where)),
              bits_of(793465.1234567891));

    size_t rows = 0;
    auto updated = db_->update_live_results(
        kStrategy, day,
        {{"sortino_ratio", 2.0 / 3.0}, {"total_transaction_costs", 0.00000051},
         {"total_pnl", -7.00004999}},
        kPortfolio, "trading.live_results", &rows);
    ASSERT_TRUE(updated.is_ok()) << (updated.is_error() ? updated.error()->what() : "");
    EXPECT_EQ(rows, 1u);
    EXPECT_EQ(bits_of(number("trading.live_results", "sortino_ratio", where)),
              bits_of(2.0 / 3.0));
    EXPECT_EQ(cell("trading.live_results", "total_transaction_costs", where), "0.00000051");
    EXPECT_EQ(cell("trading.live_results", "total_pnl", where), "-7.0000");
}

// D-16, trading.equity_curve: the Day T-1 refresh of the curve stores the level handed in, as
// the day's own INSERT (a bound parameter) always did.
TEST_F(FiniteStatementDbTest, TheLiveCurveRefreshStoresTheLevelHandedIn) {
    const Timestamp day = at_utc(2026, 5, 4, 5);
    auto stored = db_->store_trading_equity_curve(kStrategy, day, 500000.0, kPortfolio);
    ASSERT_TRUE(stored.is_ok()) << (stored.is_error() ? stored.error()->what() : "");
    const double level = 495186.14168412345;
    auto updated = db_->update_live_equity_curve(kStrategy, day, level, kPortfolio);
    ASSERT_TRUE(updated.is_ok()) << (updated.is_error() ? updated.error()->what() : "");
    const std::string where = "strategy_id = '" + std::string(kStrategy) + "'";
    EXPECT_EQ(bits_of(number("trading.equity_curve", "equity", where)), bits_of(level))
        << "stored " << cell("trading.equity_curve", "equity", where);
}
