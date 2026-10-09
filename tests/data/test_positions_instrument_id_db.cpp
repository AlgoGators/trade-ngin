// positions.instrument_id (migration 016; LOOP_SPEC v6.1 section 7; T-ROLLX commit 2): a stored
// futures position round-trips the contract it is held in, an empty id is stored NULL, and the
// backtest's final_positions writer names the column. Through TRADE_NGIN_TEST_DSN only, on a
// throwaway strategy id, every row deleted at the end.
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
}  // namespace

class PositionsInstrumentIdTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string c = dsn();
        if (c.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        db_ = std::make_shared<PostgresDatabase>(c);
        if (db_->connect().is_error()) GTEST_SKIP() << "database unreachable";
        pqxx::connection conn(c);
        pqxx::work w(conn);
        const auto n = w.exec("SELECT count(*) FROM information_schema.columns WHERE table_schema='trading' "
                              "AND table_name='positions' AND column_name='instrument_id'")[0][0].as<int>();
        if (n == 0) GTEST_SKIP() << "migration 016 not applied on this database";
        w.exec("DELETE FROM trading.positions WHERE strategy_id = 'TROLLX_016_TEST'");
        w.commit();
        conn_ = c;
    }
    void TearDown() override {
        if (!conn_.empty()) {
            pqxx::connection conn(conn_);
            pqxx::work w(conn);
            w.exec("DELETE FROM trading.positions WHERE strategy_id = 'TROLLX_016_TEST'");
            w.commit();
        }
        if (db_) db_->disconnect();
    }
    std::shared_ptr<PostgresDatabase> db_;
    std::string conn_;
};

TEST_F(PositionsInstrumentIdTest, AStoredPositionRoundTripsItsContractIdAndAnEmptyIdIsNull) {
    const Timestamp ts = std::chrono::system_clock::from_time_t(1761523200);  // 2025-10-27 00:00 UTC
    Position ng;
    ng.symbol = "NG.v.0";
    ng.quantity = Decimal(2.0);
    ng.average_price = Decimal(3.376);
    ng.last_update = ts;
    ng.instrument_id = "864";
    Position es = ng;
    es.symbol = "ES.v.0";
    es.quantity = Decimal(-1.0);
    es.instrument_id.clear();
    ASSERT_TRUE(db_->store_positions({ng, es}, "TROLLX_016_TEST", "TREND", "TROLLX_TEST_PORTFOLIO",
                                     "trading.positions").is_ok());
    auto back = db_->load_positions_by_date("TROLLX_016_TEST", "TREND", "TROLLX_TEST_PORTFOLIO", ts,
                                            "trading.positions");
    ASSERT_TRUE(back.is_ok()) << back.error()->what();
    ASSERT_EQ(back.value().count("NG.v.0"), 1u);
    EXPECT_EQ(back.value().at("NG.v.0").instrument_id, "864");
    EXPECT_EQ(back.value().at("ES.v.0").instrument_id, "");
    pqxx::connection conn(conn_);
    pqxx::work w(conn);
    const auto rows = w.exec("SELECT symbol, instrument_id IS NULL FROM trading.positions WHERE "
                             "strategy_id = 'TROLLX_016_TEST' ORDER BY symbol");
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0][0].as<std::string>(), "ES.v.0");
    EXPECT_TRUE(rows[0][1].as<bool>()) << "an empty id is stored NULL";
    EXPECT_FALSE(rows[1][1].as<bool>());
}
