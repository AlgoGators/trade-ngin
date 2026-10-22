// K3 storage (T-7b-2 8b, migration 013): every executions writer stores netting_adjustment.
//
// store_executions (every live runner), store_backtest_executions and
// store_backtest_executions_with_strategy (both backtest paths, the single-row statement and the
// batched one above 100 rows) must name the column and bind the report's value, so a netted row
// reads back its 8-decimal adjustment (negative included) and an un-netted row reads 0, never
// NULL (NULL means "written before migration 013"). The value is bound as a double, exactly as
// the four cost columns beside it are, so the column reads like them (-0.50653208999999999 in
// NUMERIC) and a full cross's total_transaction_costs - netting_adjustment is exactly 0 in SQL.
//
// Needs a database migrated with 013 (the gate applies it to the session clone first).
// Writes only under the probe identities below and removes them in TearDown.
// Reachability gate as tests/data/test_delete_stale_executions_scope_db.cpp.

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

constexpr const char* kStrategyId = "K3_NETTING_PROBE_ID";
constexpr const char* kPortfolio = "K3_NETTING_PROBE_PORTFOLIO";
constexpr const char* kRunId = "K3_NETTING_PROBE_RUN";

std::string dsn() {
    const char* v = std::getenv("TRADE_NGIN_TEST_DSN");
    return (v && *v) ? std::string(v) : std::string();
}

Timestamp date_at(int y, int m, int d) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = 5;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

ExecutionReport make_exec(const std::string& id, Side side, const char* cost, const char* adj) {
    ExecutionReport e;
    e.symbol = "6C.v.0";
    e.order_id = "DAILY_6C.v.0_2026-04-29";
    e.exec_id = id;
    e.side = side;
    e.filled_quantity = Quantity(1.0);
    e.fill_price = Price(0.7324);
    e.fill_time = date_at(2026, 4, 29);
    e.commissions_fees = Decimal(1.5);
    e.implicit_price_impact = Decimal(0.00002473);
    e.slippage_market_impact = Decimal(std::stod(cost) - 1.5);
    e.total_transaction_costs = Decimal(std::stod(cost));
    e.netting_adjustment = Decimal(std::stod(adj));
    e.is_partial = false;
    return e;
}

}  // namespace

class ExecutionsNettingColumnTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* req = std::getenv("TRADE_NGIN_REQUIRE_DB");
        const bool require_db = req && std::string(req) == "1";
        conn_ = dsn();
        if (conn_.empty()) {
            if (require_db) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set";
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        }
        db_ = std::make_shared<PostgresDatabase>(conn_);
        if (db_->connect().is_error() || !db_->is_connected()) {
            if (require_db) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable";
            GTEST_SKIP() << "database unreachable";
        }
        purge();
        pqxx::connection c(conn_);
        pqxx::work t(c);
        t.exec("INSERT INTO backtest.results (run_id) VALUES (" + t.quote(kRunId) + ")");
        t.commit();
    }
    void TearDown() override {
        if (db_ && db_->is_connected()) purge();
    }
    void purge() {
        try {
            pqxx::connection c(conn_);
            pqxx::work t(c);
            t.exec("DELETE FROM trading.executions WHERE strategy_id = " + t.quote(kStrategyId));
            t.exec("DELETE FROM backtest.executions WHERE run_id = " + t.quote(kRunId));
            t.exec("DELETE FROM backtest.results WHERE run_id = " + t.quote(kRunId));
            t.commit();
        } catch (const std::exception&) {
        }
    }
    std::string trading_value(const std::string& strategy_name) {
        pqxx::connection c(conn_);
        pqxx::work t(c);
        auto r = t.exec("SELECT coalesce(trim_scale(round(netting_adjustment, 8))::text, 'NULL') "
                        "FROM trading.executions "
                        "WHERE strategy_id = " + t.quote(kStrategyId) +
                        " AND strategy_name = " + t.quote(strategy_name));
        return r.empty() ? "MISSING" : r[0][0].c_str();
    }
    std::vector<std::string> backtest_values(const std::string& strategy_id) {
        pqxx::connection c(conn_);
        pqxx::work t(c);
        auto r = t.exec("SELECT coalesce(netting_adjustment::text, 'NULL') "
                        "FROM backtest.executions "
                        "WHERE run_id = " + t.quote(kRunId) + " AND strategy_id = " +
                        t.quote(strategy_id) + " ORDER BY execution_id");
        std::vector<std::string> out;
        for (const auto& row : r) out.emplace_back(row[0].c_str());
        return out;
    }

    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

TEST_F(ExecutionsNettingColumnTest, LiveRowsStoreTheirExactAdjustment) {
    // C9h basechain 2026-04-29: both sleeves sell 1 6C; the account's C(2) makes it a debit.
    std::vector<ExecutionReport> tf{
        make_exec("EXEC_K3_TF", Side::SELL, "3.97287664", "-0.50653209")};
    std::vector<ExecutionReport> ff{
        make_exec("EXEC_K3_FF", Side::SELL, "3.97287664", "-0.50653208")};
    std::vector<ExecutionReport> one{make_exec("EXEC_K3_ONE", Side::BUY, "2.87279481", "0")};
    ASSERT_FALSE(db_->store_executions(tf, kStrategyId, "K3_TF", kPortfolio, "trading.executions")
                     .is_error());
    ASSERT_FALSE(db_->store_executions(ff, kStrategyId, "K3_FF", kPortfolio, "trading.executions")
                     .is_error());
    ASSERT_FALSE(db_->store_executions(one, kStrategyId, "K3_ONE", kPortfolio, "trading.executions")
                     .is_error());
    EXPECT_EQ(trading_value("K3_TF"), "-0.50653209");
    EXPECT_EQ(trading_value("K3_FF"), "-0.50653208");
    EXPECT_EQ(trading_value("K3_ONE"), "0") << "an un-netted row is 0, not NULL";

    // C9h basechain 2026-04-24: a full cross, the adjustment is the row's own cost.
    std::vector<ExecutionReport> x{make_exec("EXEC_K3_X", Side::BUY, "3.94919180", "3.94919180")};
    ASSERT_FALSE(db_->store_executions(x, kStrategyId, "K3_X", kPortfolio, "trading.executions")
                     .is_error());
    pqxx::connection c(conn_);
    pqxx::work t(c);
    auto r = t.exec("SELECT (total_transaction_costs - netting_adjustment = 0)::text "
                    "FROM trading.executions WHERE strategy_id = " + t.quote(kStrategyId) +
                    " AND strategy_name = 'K3_X'");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_STREQ(r[0][0].c_str(), "true") << "a full cross's net cost is exactly 0 in SQL";
}

TEST_F(ExecutionsNettingColumnTest, BacktestRowsStoreTheirAdjustmentOnEveryWriterPath) {
    std::vector<ExecutionReport> small{make_exec("EX-A-0", Side::BUY, "3.94919180", "3.9491918"),
                                       make_exec("EX-A-1", Side::SELL, "3.94919180", "0")};
    ASSERT_FALSE(db_->store_backtest_executions_with_strategy(small, kRunId, "K3_BT_A", kPortfolio,
                                                              "backtest.executions")
                     .is_error());
    EXPECT_EQ(backtest_values("K3_BT_A"), (std::vector<std::string>{"3.9491918", "0"}));

    std::vector<ExecutionReport> batch;  // > 100 rows: the batched statement
    for (int i = 0; i < 120; ++i) {
        batch.push_back(make_exec("EX-B-" + std::to_string(1000 + i), Side::SELL, "3.97287664",
                                  i == 7 ? "-0.50653208" : "0"));
    }
    ASSERT_FALSE(db_->store_backtest_executions_with_strategy(batch, kRunId, "K3_BT_B", kPortfolio,
                                                              "backtest.executions")
                     .is_error());
    auto vb = backtest_values("K3_BT_B");
    ASSERT_EQ(vb.size(), 120u);
    EXPECT_EQ(vb[7], "-0.50653208");
    EXPECT_EQ(vb[8], "0");
    // store_backtest_executions (no strategy id) names no strategy_id, which backtest.executions
    // requires (NOT NULL), so it cannot insert into this table at all; it gets the column for
    // symmetry and is not exercised here.
}

// T-NETTING fix round: a ROLL leg and a BORROW row are never netted. A row of either kind that
// carries a netting adjustment is REFUSED by the writer (an error naming the row), not stored and
// not corrected to 0; the same row with adjustment 0 is stored.
TEST_F(ExecutionsNettingColumnTest, ARollOrBorrowRowWithAnAdjustmentIsRefusedNotStored) {
    for (const ExecutionType type : {ExecutionType::ROLL, ExecutionType::BORROW}) {
        auto bad = make_exec("EXEC_K3_UNNETTED", Side::SELL, "3.97287664", "0.25");
        bad.execution_type = type;
        const auto refused = db_->store_executions({bad}, kStrategyId, "K3_UNNETTED", kPortfolio,
                                                   "trading.executions");
        ASSERT_TRUE(refused.is_error()) << to_string(type);
        EXPECT_NE(std::string(refused.error()->what()).find("NETTING_REFUSED"), std::string::npos)
            << refused.error()->what();
        EXPECT_NE(std::string(refused.error()->what()).find("EXEC_K3_UNNETTED"), std::string::npos)
            << "the refusal names the row: " << refused.error()->what();
        EXPECT_EQ(trading_value("K3_UNNETTED"), "MISSING") << "nothing was stored";

        const auto refused_bt = db_->store_backtest_executions_with_strategy(
            {bad}, kRunId, "K3_UNNETTED", kPortfolio, "backtest.executions");
        EXPECT_TRUE(refused_bt.is_error()) << to_string(type);
        EXPECT_TRUE(backtest_values("K3_UNNETTED").empty()) << "nothing was stored";
    }
    auto good = make_exec("EXEC_K3_ROLL_OK", Side::SELL, "3.97287664", "0");
    good.execution_type = ExecutionType::ROLL;
    ASSERT_FALSE(db_->store_executions({good}, kStrategyId, "K3_UNNETTED", kPortfolio,
                                       "trading.executions")
                     .is_error());
    EXPECT_EQ(trading_value("K3_UNNETTED"), "0");
}
