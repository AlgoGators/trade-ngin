// executions.execution_type and instrument_id (migration 015), live_results.total_roll_costs (017)
// through the engine's own writers and readers (T-ROLLX commit 3): a STRATEGY fill and the two ROLL
// legs of one symbol-day share the sleeve's key, the legs' class and contract ids round-trip, the
// re-run sweep by type removes the legs and nothing else, the backtest writer names the column, and
// the previous total_roll_costs reads the latest row before a date. TRADE_NGIN_TEST_DSN only; a
// throwaway strategy id; every row deleted at the end.
#include <gtest/gtest.h>
#include <pqxx/pqxx>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {
std::string dsn() {
    const char* v = std::getenv("TRADE_NGIN_TEST_DSN");
    return v && *v ? std::string(v) : std::string();
}
const Timestamp kT = std::chrono::system_clock::from_time_t(1761609600 + 5 * 3600);  // 2025-10-28 05:00 UTC
ExecutionReport row(const std::string& exec_id, const std::string& order_id, Side side, double qty, double px,
                    ExecutionType type, const std::string& id) {
    ExecutionReport e;
    e.exec_id = exec_id;
    e.order_id = order_id;
    e.symbol = "NG.v.0";
    e.side = side;
    e.filled_quantity = Quantity(qty);
    e.fill_price = Price(px);
    e.fill_time = kT;
    e.commissions_fees = Decimal(1.5 * qty);
    e.total_transaction_costs = Decimal(1.5 * qty + 1.0);
    e.execution_type = type;
    e.instrument_id = id;
    return e;
}
}  // namespace

class ExecutionTypeDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        conn_ = dsn();
        if (conn_.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        db_ = std::make_shared<PostgresDatabase>(conn_);
        if (db_->connect().is_error()) GTEST_SKIP() << "database unreachable";
        pqxx::connection c(conn_);
        pqxx::work w(c);
        if (w.exec("SELECT count(*) FROM information_schema.columns WHERE table_schema='trading' AND table_name='executions' "
                   "AND column_name='execution_type'")[0][0].as<int>() == 0) {
            GTEST_SKIP() << "migration 015 not applied";
        }
        if (w.exec("SELECT count(*) FROM information_schema.columns WHERE table_schema='trading' AND table_name='live_results' "
                   "AND column_name='total_roll_costs'")[0][0].as<int>() == 0) {
            GTEST_SKIP() << "migration 017 not applied";
        }
        purge(w);
        w.commit();
    }
    void TearDown() override {
        if (!conn_.empty()) {
            pqxx::connection c(conn_);
            pqxx::work w(c);
            purge(w);
            w.commit();
        }
        if (db_) db_->disconnect();
    }
    static void purge(pqxx::work& w) {
        w.exec("DELETE FROM trading.executions WHERE portfolio_id = 'TROLLX_015_TEST'");
        w.exec("DELETE FROM trading.live_results WHERE portfolio_id = 'TROLLX_017_TEST'");
        w.exec("DELETE FROM backtest.executions WHERE run_id = 'TROLLX_015_RUN'");
        w.exec("DELETE FROM backtest.results WHERE run_id = 'TROLLX_015_RUN'");
    }
    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

TEST_F(ExecutionTypeDbTest, TheLegsAndTheFillShareTheDaysKeyAndTheSweepByTypeRemovesOnlyTheLegs) {
    const std::vector<ExecutionReport> rows = {
        row("EXEC_NG.v.0_20251028_RC", "ROLL_NG.v.0_20251028_RC", Side::SELL, 2, 3.376, ExecutionType::ROLL, "864"),
        row("EXEC_NG.v.0_20251028_RO", "ROLL_NG.v.0_20251028_RO", Side::BUY, 2, 3.965, ExecutionType::ROLL, "863"),
        row("EXEC_NG.v.0_20251028", "DAILY_NG.v.0_20251028", Side::BUY, 1, 3.822, ExecutionType::STRATEGY, ""),
    };
    ASSERT_TRUE(db_->store_executions(rows, "TROLLX_015_ID", "TREND", "TROLLX_015_TEST", "trading.executions").is_ok());
    pqxx::connection c(conn_);
    {
        pqxx::work w(c);
        const auto r = w.exec("SELECT exec_id, execution_type, coalesce(instrument_id, 'NULL') FROM trading.executions "
                              "WHERE portfolio_id = 'TROLLX_015_TEST' ORDER BY exec_id");
        ASSERT_EQ(r.size(), 3u);
        EXPECT_EQ(r[0][0].as<std::string>(), "EXEC_NG.v.0_20251028");
        EXPECT_EQ(r[0][1].as<std::string>(), "STRATEGY");
        EXPECT_EQ(r[0][2].as<std::string>(), "NULL");
        EXPECT_EQ(r[1][1].as<std::string>(), "ROLL");
        EXPECT_EQ(r[1][2].as<std::string>(), "864");
        EXPECT_EQ(r[2][2].as<std::string>(), "863");
    }
    ASSERT_TRUE(db_->delete_roll_executions(kT, "TREND", "TROLLX_015_TEST", "trading.executions").is_ok());
    {
        pqxx::work w(c);
        const auto r = w.exec("SELECT exec_id FROM trading.executions WHERE portfolio_id = 'TROLLX_015_TEST'");
        ASSERT_EQ(r.size(), 1u) << "the STRATEGY fill stays";
        EXPECT_EQ(r[0][0].as<std::string>(), "EXEC_NG.v.0_20251028");
    }
}

TEST_F(ExecutionTypeDbTest, TheBacktestWriterNamesTheClassAndTheContract) {
    {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        w.exec("INSERT INTO backtest.results (run_id, portfolio_id, start_date, end_date) VALUES ('TROLLX_015_RUN', 'TROLLX_015_TEST', "
               "'2025-10-27', '2025-10-28')");
        w.commit();
    }
    std::vector<ExecutionReport> rows = {
        row("RL-TREND-0", "RL-TREND-0", Side::SELL, 2, 3.376, ExecutionType::ROLL, "864"),
        row("RL-TREND-1", "RL-TREND-1", Side::BUY, 2, 3.965, ExecutionType::ROLL, "863"),
        row("EX-TREND-7", "PM-TREND-7", Side::BUY, 1, 3.822, ExecutionType::STRATEGY, ""),
    };
    ASSERT_TRUE(db_->store_backtest_executions_with_strategy(rows, "TROLLX_015_RUN", "TREND", "TROLLX_015_TEST",
                                                             "backtest.executions").is_ok());
    pqxx::connection c(conn_);
    pqxx::work w(c);
    const auto r = w.exec("SELECT execution_id, execution_type, coalesce(instrument_id, 'NULL') FROM backtest.executions "
                          "WHERE run_id = 'TROLLX_015_RUN' ORDER BY execution_id");
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0][1].as<std::string>(), "STRATEGY");
    EXPECT_EQ(r[1][1].as<std::string>(), "ROLL");
    EXPECT_EQ(r[1][2].as<std::string>(), "864");
    EXPECT_EQ(r[2][2].as<std::string>(), "863");
}

TEST_F(ExecutionTypeDbTest, ThePreviousTotalRollCostsReadsTheLatestRowBeforeTheDate) {
    {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        w.exec("INSERT INTO trading.live_results (strategy_id, portfolio_id, date, daily_roll_costs, total_roll_costs) VALUES "
               "('TROLLX_017_ID', 'TROLLX_017_TEST', '2025-10-27', 13.0, 26.5), ('TROLLX_017_ID', 'TROLLX_017_TEST', '2025-10-28', 0, 26.5)");
        w.commit();
    }
    auto before = db_->get_previous_total_roll_costs("TROLLX_017_ID", "TROLLX_017_TEST", kT, "trading.live_results");
    ASSERT_TRUE(before.is_ok()) << before.error()->what();
    EXPECT_DOUBLE_EQ(before.value(), 26.5) << "the 10-27 row, the latest before 10-28";
    auto none = db_->get_previous_total_roll_costs("TROLLX_017_NOBODY", "TROLLX_017_TEST", kT, "trading.live_results");
    ASSERT_TRUE(none.is_ok());
    EXPECT_DOUBLE_EQ(none.value(), 0.0);
}
