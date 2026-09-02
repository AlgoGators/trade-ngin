// E2-F4 end-to-end: can one book's stale-execution cleanup delete another book's rows?
//
// delete_stale_executions() used to key on (DATE(execution_time), strategy_name, order_id)
// with NO portfolio predicate, while trading.executions is keyed
// (portfolio_id, strategy_id, strategy_name, date, exec_id). order_id is
// portfolio-independent -- ExecutionManager builds it as "DAILY_<symbol>_<date>"
// (execution_manager.cpp:147) -- and TREND_FOLLOWING is enabled-live in BOTH the base and
// conservative books. So a conservative run's pre-insert cleanup could delete
// BASE_PORTFOLIO's rows for the same symbol and date, and vice versa. The survivor was
// whichever book ran last, and a DELETE leaves no trace of what it removed.
//
// It never fired only because two call sites passed table_name into the strategy_name slot,
// so the predicate matched nothing. Fixing THAT alone would have armed THIS -- which is why
// the two changes ship together and why portfolio_id is a required parameter rather than a
// defaulted one.
//
// A mock can only prove the argument reaches the call. Whether the DELETE actually spares
// the other portfolio's rows is a property of the SQL, so it needs a real server: write two
// rows that differ ONLY by portfolio_id, delete one book's, and assert the other survives.
//
// Writes touch trading.executions only under the scratch identities below, which no live or
// backtest book uses; every row is removed in TearDown.
//
// Reachability gate matches tests/data/test_db_transaction_atomicity.cpp:
//   * TRADE_NGIN_REQUIRE_DB=1 -- unreachable database FAILS rather than skips.
//   * unset -- skip (local dev without a server).

#include <gtest/gtest.h>
#include <pqxx/pqxx>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {

// Deliberately unlike any configured book, but sharing strategy_name and order_id across
// two portfolios -- which is exactly the collision the live books have for TREND_FOLLOWING.
constexpr const char* kStrategyId = "F4_SCOPE_PROBE_ID";
constexpr const char* kStrategyName = "F4_SCOPE_PROBE_NAME";
constexpr const char* kPortfolioA = "F4_SCOPE_PROBE_PORTFOLIO_A";
constexpr const char* kPortfolioB = "F4_SCOPE_PROBE_PORTFOLIO_B";
constexpr const char* kSharedOrderId = "DAILY_F4PROBE_2026-05-04";

std::string discover_connection_string() {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        fs::path candidate = dir / "config" / "defaults.json";
        if (fs::exists(candidate)) {
            try {
                std::ifstream in(candidate);
                nlohmann::json j = nlohmann::json::parse(in);
                const auto& d = j.at("database");
                return "postgresql://" + d.at("username").get<std::string>() + ":" +
                       d.at("password").get<std::string>() + "@" +
                       d.at("host").get<std::string>() + ":" + d.at("port").get<std::string>() +
                       "/" + d.at("name").get<std::string>();
            } catch (const std::exception&) {
                return {};
            }
        }
        dir = dir.parent_path();
    }
    return {};
}

Timestamp date_at(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

ExecutionReport make_exec(const std::string& symbol) {
    ExecutionReport e;
    e.symbol = symbol;
    e.order_id = kSharedOrderId;
    e.exec_id = "F4_SCOPE_PROBE_EXEC";
    e.side = Side::BUY;
    e.filled_quantity = Quantity(1.0);
    e.fill_price = Price(100.0);
    e.fill_time = date_at(2026, 5, 4);
    e.commissions_fees = Decimal(1.0);
    e.implicit_price_impact = Decimal(0.0);
    e.slippage_market_impact = Decimal(0.0);
    e.total_transaction_costs = Decimal(1.0);
    e.is_partial = false;
    return e;
}

}  // namespace

class DeleteStaleExecutionsScopeTest : public ::testing::Test {
protected:
    void SetUp() override {
        const bool require_db = [] {
            const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
            return v && std::string(v) == "1";
        }();

        conn_ = discover_connection_string();
        if (conn_.empty()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but config/defaults.json is not reachable, "
                          "so the E2-F4 portfolio scoping goes unverified";
            }
            GTEST_SKIP() << "config/defaults.json not reachable; no database to exercise";
        }
        db_ = std::make_shared<PostgresDatabase>(conn_);
        auto connected = db_->connect();
        if (connected.is_error() || !db_->is_connected()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable, so the "
                          "E2-F4 portfolio scoping goes unverified";
            }
            GTEST_SKIP() << "database unreachable; skipping";
        }
        purge();
    }

    void TearDown() override {
        if (db_ && db_->is_connected()) purge();
    }

    void purge() {
        try {
            pqxx::connection c(conn_);
            pqxx::work txn(c);
            txn.exec("DELETE FROM trading.executions WHERE strategy_id = " +
                     txn.quote(kStrategyId));
            txn.commit();
        } catch (const std::exception&) {
            // Best-effort cleanup; a failure here must not mask the assertion above.
        }
    }

    int count_for(const std::string& portfolio) {
        pqxx::connection c(conn_);
        pqxx::work txn(c);
        auto r = txn.exec("SELECT COUNT(*) FROM trading.executions WHERE strategy_id = " +
                          txn.quote(kStrategyId) + " AND portfolio_id = " + txn.quote(portfolio));
        return r[0][0].as<int>();
    }

    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

// THE REGRESSION. Two rows differing only by portfolio_id; deleting one must spare the other.
TEST_F(DeleteStaleExecutionsScopeTest, DeletingOnePortfolioLeavesTheOtherIntact) {
    std::vector<ExecutionReport> execs{make_exec("F4PROBE")};

    ASSERT_FALSE(
        db_->store_executions(execs, kStrategyId, kStrategyName, kPortfolioA, "trading.executions")
            .is_error());
    ASSERT_FALSE(
        db_->store_executions(execs, kStrategyId, kStrategyName, kPortfolioB, "trading.executions")
            .is_error());

    ASSERT_EQ(count_for(kPortfolioA), 1) << "setup: portfolio A row did not persist";
    ASSERT_EQ(count_for(kPortfolioB), 1) << "setup: portfolio B row did not persist";

    // Portfolio A cleans up its own stale executions, using the SAME order_id, strategy_name
    // and date that portfolio B's row carries.
    auto del = db_->delete_stale_executions({kSharedOrderId}, date_at(2026, 5, 4), kStrategyName,
                                            kPortfolioA, "trading.executions");
    ASSERT_FALSE(del.is_error()) << del.error()->what();

    EXPECT_EQ(count_for(kPortfolioA), 0)
        << "The delete did not remove its OWN portfolio's row, so the cleanup no longer works "
           "and a re-run will collide on executions_pkey.";
    EXPECT_EQ(count_for(kPortfolioB), 1)
        << "The delete reached across into another portfolio. order_id is portfolio-"
           "independent and TREND_FOLLOWING runs in both the base and conservative books, so "
           "without the portfolio predicate a conservative run silently destroys BASE's "
           "executions for the same symbol and date -- with no trace of what it took.";
}

// The cleanup must still do its job: a re-run of the same date has to be able to re-insert.
TEST_F(DeleteStaleExecutionsScopeTest, DeleteThenReinsertSucceedsForTheSameKey) {
    std::vector<ExecutionReport> execs{make_exec("F4PROBE")};

    ASSERT_FALSE(
        db_->store_executions(execs, kStrategyId, kStrategyName, kPortfolioA, "trading.executions")
            .is_error());
    ASSERT_EQ(count_for(kPortfolioA), 1);

    auto del = db_->delete_stale_executions({kSharedOrderId}, date_at(2026, 5, 4), kStrategyName,
                                            kPortfolioA, "trading.executions");
    ASSERT_FALSE(del.is_error()) << del.error()->what();

    auto again =
        db_->store_executions(execs, kStrategyId, kStrategyName, kPortfolioA, "trading.executions");
    EXPECT_FALSE(again.is_error())
        << "Re-insert after cleanup failed, so the date is not re-runnable -- which breaks "
           "idempotency (protocol 3f) and the replay-the-missed-day remedy for a run gap.";
    EXPECT_EQ(count_for(kPortfolioA), 1) << "Re-insert should leave exactly one row, not two.";
}
