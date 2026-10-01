#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {

std::string owned_dsn() {
    const char* raw = std::getenv("BACKTEST_POSITION_TEST_DSN");
    if (raw == nullptr) throw std::runtime_error("BACKTEST_POSITION_TEST_DSN is required");
    const std::string dsn(raw);
    if (!dsn.starts_with("host=/tmp/algolens-repair-pg-") ||
        dsn.find(" dbname=algolens_test_") == std::string::npos ||
        dsn.find("hostaddr") != std::string::npos || dsn.find("service=") != std::string::npos) {
        throw std::runtime_error("BACKTEST_POSITION_TEST_DSN is not an owned disposable database");
    }
    return dsn;
}

Timestamp day() {
    return std::chrono::system_clock::from_time_t(1'767'225'600);  // 2026-01-01 UTC
}

Position position(const std::string& symbol, double quantity) {
    Position result;
    result.symbol = symbol;
    result.quantity = Decimal(quantity);
    result.average_price = Decimal(100.0);
    result.last_update = day();
    return result;
}

class BacktestPositionReplacePg : public ::testing::Test {
protected:
    void SetUp() override {
        dsn_ = owned_dsn();
        pqxx::connection connection(dsn_);
        pqxx::work transaction(connection);
        transaction.exec("CREATE SCHEMA IF NOT EXISTS backtest");
        transaction.exec("DROP TABLE IF EXISTS backtest.final_positions");
        transaction.exec(R"SQL(
            CREATE TABLE backtest.final_positions (
                run_id text NOT NULL,
                portfolio_id text NOT NULL,
                strategy_id text NOT NULL,
                date date NOT NULL,
                symbol text NOT NULL,
                quantity double precision NOT NULL,
                average_price double precision NOT NULL,
                unrealized_pnl double precision NOT NULL,
                realized_pnl double precision NOT NULL,
                last_update timestamp without time zone NOT NULL,
                updated_at timestamp without time zone NOT NULL,
                PRIMARY KEY (run_id, portfolio_id, strategy_id, date, symbol)
            )
        )SQL");
        transaction.commit();
        db_ = std::make_unique<PostgresDatabase>(dsn_);
        ASSERT_TRUE(db_->connect().is_ok());
    }

    long count(const std::string& strategy) {
        pqxx::connection connection(dsn_);
        pqxx::read_transaction transaction(connection);
        return transaction.exec_params(
            "SELECT count(*) FROM backtest.final_positions WHERE run_id=$1 "
            "AND portfolio_id=$2 AND strategy_id=$3 AND date=$4::date",
            "RUN", "BOOK", strategy, "2026-01-01")[0][0].as<long>();
    }

    std::string dsn_;
    std::unique_ptr<PostgresDatabase> db_;
};

TEST_F(BacktestPositionReplacePg, EmptyReplacementDeletesStaleRows) {
    ASSERT_TRUE(db_->replace_backtest_positions_for_date(
        {position("AAA", 5.0)}, "RUN", "SLEEVE", "BOOK", day()).is_ok());
    ASSERT_EQ(count("SLEEVE"), 1);

    ASSERT_TRUE(db_->replace_backtest_positions_for_date(
        {}, "RUN", "SLEEVE", "BOOK", day()).is_ok());
    EXPECT_EQ(count("SLEEVE"), 0);
}

TEST_F(BacktestPositionReplacePg, ReplacementDropsSymbolsMissingFromRerun) {
    ASSERT_TRUE(db_->replace_backtest_positions_for_date(
        {position("AAA", 5.0), position("BBB", 3.0)},
        "RUN", "SLEEVE", "BOOK", day()).is_ok());
    ASSERT_EQ(count("SLEEVE"), 2);

    ASSERT_TRUE(db_->replace_backtest_positions_for_date(
        {position("AAA", 7.0)}, "RUN", "SLEEVE", "BOOK", day()).is_ok());
    EXPECT_EQ(count("SLEEVE"), 1);
}

TEST_F(BacktestPositionReplacePg, EmptyIdentityFailsBeforeMutation) {
    EXPECT_TRUE(db_->replace_backtest_positions_for_date(
        {}, "RUN", "SLEEVE", "", day()).is_error());
}

}  // namespace
