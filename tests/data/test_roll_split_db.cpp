// Migration 040 through the engine's own writers: a per-contract roll's
// closing leg stores its split in trading.executions (the live writer, and the re-run's replace of the
// day) and in backtest.executions (the small and the batched statement), every other row NULL; the
// results row stores the two totals as typed cells. TRADE_NGIN_TEST_DSN only, a database with 040
// applied; test-only ids; every row deleted at the end.
#include <gtest/gtest.h>
#include <pqxx/pqxx>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "trade_ngin/backtest/backtest_statistics_columns.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/roll_series.hpp"

using namespace trade_ngin;

namespace {
std::string dsn() {
    const char* v = std::getenv("TRADE_NGIN_TEST_DSN");
    return v && *v ? std::string(v) : std::string();
}
const Timestamp kT = std::chrono::system_clock::from_time_t(1764115200);  // 2025-11-26 00:00 UTC
roll_series::RollLegCost leg_cost(const std::string&, double, double) {
    return roll_series::RollLegCost{2.51, 0.004, 20.11, 22.62};
}
// The worked example's roll: long 1 silver, December 50.960 to March 51.630, December's move -0.215.
std::vector<ExecutionReport> silver_legs(const std::string& rc, const std::string& ro) {
    auto legs = roll_series::make_roll_legs("SI.v.0", 1.0, 50.960, 51.630, "SIZ5_ID", "SIH6_ID", kT, rc,
                                            "ROLL_" + rc, ro, "ROLL_" + ro, leg_cost);
    roll_series::attach_roll_split(legs, 1.0, 50.960 - 51.175, 5000.0);
    return legs;
}
ExecutionReport fill(const std::string& id) {
    ExecutionReport e;
    e.exec_id = id;
    e.order_id = "O_" + id;
    e.symbol = "ZN.v.0";
    e.side = Side::BUY;
    e.filled_quantity = Quantity(2.0);
    e.fill_price = Price(110.25);
    e.fill_time = kT;
    e.commissions_fees = Decimal(5.02);
    e.total_transaction_costs = Decimal(30.1);
    return e;
}
}  // namespace

class RollSplitDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        conn_ = dsn();
        if (conn_.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        db_ = std::make_shared<PostgresDatabase>(conn_);
        if (db_->connect().is_error()) GTEST_SKIP() << "database unreachable";
        pqxx::connection c(conn_);
        pqxx::work w(c);
        if (w.exec("SELECT count(*) FROM information_schema.columns WHERE column_name IN ('roll_bar_move', "
                   "'roll_gap_points', 'roll_gap_value', 'roll_bar_moves', 'roll_gap_values')")[0][0].as<int>() != 8) {
            GTEST_SKIP() << "migration 040 not applied";
        }
        purge(w);
        w.commit();
    }
    void TearDown() override {
        if (!conn_.empty() && db_) {
            pqxx::connection c(conn_);
            pqxx::work w(c);
            purge(w);
            w.commit();
        }
        if (db_) db_->disconnect();
    }
    static void purge(pqxx::work& w) {
        w.exec("DELETE FROM trading.executions WHERE portfolio_id = 'R4B_040_TEST'");
        w.exec("DELETE FROM backtest.executions WHERE run_id LIKE 'R4B_040_RUN%'");
        w.exec("DELETE FROM backtest.results WHERE run_id LIKE 'R4B_040_RUN%'");
    }
    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

TEST_F(RollSplitDbTest, TheLiveWriterStoresTheSplitOnTheClosingLegOnly) {
    auto rows = silver_legs("EXEC_SI.v.0_20251125_RC", "EXEC_SI.v.0_20251125_RO");
    rows.push_back(fill("EXEC_ZN.v.0_20251126"));
    ASSERT_TRUE(db_->store_executions(rows, "R4B_040_ID", "TREND", "R4B_040_TEST", "trading.executions").is_ok());
    pqxx::connection c(conn_);
    pqxx::work w(c);
    const auto r = w.exec("SELECT exec_id, coalesce(roll_bar_move::text, 'NULL'), coalesce(roll_gap_points::text, 'NULL'), "
                          "coalesce(roll_gap_value::text, 'NULL') FROM trading.executions WHERE portfolio_id = "
                          "'R4B_040_TEST' ORDER BY exec_id");
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0][0].as<std::string>(), "EXEC_SI.v.0_20251125_RC");
    EXPECT_NEAR(std::stod(r[0][1].as<std::string>()), -1075.0, 1e-6);
    EXPECT_NEAR(std::stod(r[0][2].as<std::string>()), 0.670, 1e-9);
    EXPECT_NEAR(std::stod(r[0][3].as<std::string>()), 3350.0, 1e-6);
    for (int i : {1, 2}) {
        EXPECT_EQ(r[i][1].as<std::string>(), "NULL") << r[i][0].as<std::string>();
        EXPECT_EQ(r[i][2].as<std::string>(), "NULL");
        EXPECT_EQ(r[i][3].as<std::string>(), "NULL");
    }
}

// The re-run's replace of the day sweeps the day's rows and writes the legs once: one split, not two.
TEST_F(RollSplitDbTest, ARerunReplacesTheDayAndStoresOneSplit) {
    const auto legs = silver_legs("EXEC_SI.v.0_20251125_RC", "EXEC_SI.v.0_20251125_RO");
    for (int run = 0; run < 2; ++run) {
        ASSERT_TRUE(db_->replace_roll_day_executions({{"TREND", legs}}, "R4B_040_ID", "R4B_040_TEST", kT,
                                                     "trading.executions")
                        .is_ok())
            << "run " << run;
    }
    pqxx::connection c(conn_);
    pqxx::work w(c);
    const auto r = w.exec("SELECT count(*), count(roll_bar_move), sum(roll_bar_move) FROM trading.executions WHERE "
                          "portfolio_id = 'R4B_040_TEST'");
    EXPECT_EQ(r[0][0].as<int>(), 2);
    EXPECT_EQ(r[0][1].as<int>(), 1) << "one roll, one set of values";
    EXPECT_NEAR(r[0][2].as<double>(), -1075.0, 1e-6);
}

TEST_F(RollSplitDbTest, TheBacktestWriterStoresTheSplitInTheSmallAndTheBatchedStatement) {
    for (const int extra : {1, 150}) {
        SCOPED_TRACE(extra);
        const std::string run = "R4B_040_RUN_" + std::to_string(extra);
        {
            pqxx::connection c(conn_);
            pqxx::work w(c);
            w.exec("INSERT INTO backtest.results (run_id, portfolio_id, start_date, end_date) VALUES (" + w.quote(run) +
                   ", 'R4B_040_TEST', '2025-11-25', '2025-11-26')");
            w.commit();
        }
        auto rows = silver_legs("RL-TREND-0", "RL-TREND-1");
        for (int i = 0; i < extra; ++i) rows.push_back(fill("EX-TREND-" + std::to_string(i)));
        ASSERT_TRUE(db_->store_backtest_executions_with_strategy(rows, run, "TREND", "R4B_040_TEST", "backtest.executions")
                        .is_ok());
        pqxx::connection c(conn_);
        pqxx::work w(c);
        const auto r = w.exec("SELECT count(*), count(roll_bar_move), count(roll_gap_points), count(roll_gap_value), "
                              "max(roll_bar_move), max(roll_gap_points), max(roll_gap_value), "
                              "bool_and(roll_bar_move IS NULL OR execution_id = 'RL-TREND-0') FROM backtest.executions "
                              "WHERE run_id = " + w.quote(run));
        EXPECT_EQ(r[0][0].as<int>(), extra + 2);
        EXPECT_EQ(r[0][1].as<int>(), 1);
        EXPECT_EQ(r[0][2].as<int>(), 1);
        EXPECT_EQ(r[0][3].as<int>(), 1);
        EXPECT_NEAR(r[0][4].as<double>(), -1075.0, 1e-9);
        EXPECT_NEAR(r[0][5].as<double>(), 0.670, 1e-12);
        EXPECT_NEAR(r[0][6].as<double>(), 3350.0, 1e-9);
        EXPECT_TRUE(r[0][7].as<bool>()) << "only the closing leg";
    }
}

TEST_F(RollSplitDbTest, TheResultsRowStoresTheTotalsOnlyWhenHanded) {
    const std::unordered_map<std::string, double> metrics = {{"total_return", 0.1}};
    ASSERT_TRUE(db_->store_backtest_summary("R4B_040_RUN_ON", kT, kT, metrics, "R4B_040_TEST", "backtest.results",
                                            roll_split_summary_cells(-1075.0, 3350.0))
                    .is_ok());
    ASSERT_TRUE(db_->store_backtest_summary("R4B_040_RUN_OFF", kT, kT, metrics, "R4B_040_TEST", "backtest.results",
                                            roll_split_summary_cells(std::nullopt, std::nullopt))
                    .is_ok());
    pqxx::connection c(conn_);
    pqxx::work w(c);
    const auto r = w.exec("SELECT run_id, coalesce(roll_bar_moves::text, 'NULL'), coalesce(roll_gap_values::text, 'NULL') "
                          "FROM backtest.results WHERE run_id IN ('R4B_040_RUN_ON', 'R4B_040_RUN_OFF') ORDER BY run_id");
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0][0].as<std::string>(), "R4B_040_RUN_OFF");
    EXPECT_EQ(r[0][1].as<std::string>(), "NULL");
    EXPECT_EQ(r[0][2].as<std::string>(), "NULL");
    EXPECT_EQ(r[1][1].as<std::string>(), "-1075");
    EXPECT_EQ(r[1][2].as<std::string>(), "3350");
}
