// The live roll state read from the stored rows (LOOP_SPEC v6.2 section 6.5; T-ROLLX-FIX commit 4):
//   B8  a re-run of a date reads the contract its own stored legs rolled out of
//       (get_stored_roll_contracts): the first run re-wrote the T-1 positions row.
//   B4  total_roll_costs continues from the latest stored live_results row across the days whose
//       results write failed: those days stored their ROLL legs and no row
//       (get_previous_total_roll_costs).
//   C5  a sleeve's executions of a roll day are replaced in ONE transaction
//       (replace_roll_day_executions, T-ROLLX-FIX commit 5): a re-store that fails leaves the legs
//       the run before stored; a sweep committed apart from the insert deleted them.
// Through TRADE_NGIN_TEST_DSN only, on throwaway ids, every row deleted at the end.
#include <gtest/gtest.h>
#include <pqxx/pqxx>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {
std::string dsn() {
    const char* v = std::getenv("TRADE_NGIN_TEST_DSN");
    return v && *v ? std::string(v) : std::string();
}
Timestamp utc(int y, int m, int d) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}
const std::string kId = "TROLLX_ROLLSTATE_TEST";
const std::string kPortfolio = "TROLLX_ROLLSTATE_PORTFOLIO";
}  // namespace

class RollStateDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string c = dsn();
        if (c.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        db_ = std::make_shared<PostgresDatabase>(c);
        if (db_->connect().is_error()) GTEST_SKIP() << "database unreachable";
        conn_ = c;
        wipe();
    }
    void TearDown() override {
        if (!conn_.empty()) wipe();
        if (db_) db_->disconnect();
    }
    void wipe() {
        pqxx::connection conn(conn_);
        pqxx::work w(conn);
        w.exec("DELETE FROM trading.executions WHERE strategy_id = '" + kId + "'");
        w.exec("DELETE FROM trading.live_results WHERE strategy_id = '" + kId + "'");
        w.commit();
    }
    // One stored execution row of the book, as the live runner stores it.
    void leg(const std::string& date, const std::string& exec_id, const std::string& symbol,
             const std::string& type, const std::string& instrument, double cost,
             const std::string& sleeve = "TREND_FOLLOWING") {
        pqxx::connection conn(conn_);
        pqxx::work w(conn);
        w.exec("INSERT INTO trading.executions (exec_id, order_id, symbol, side, quantity, price, "
               "execution_time, commissions_fees, is_partial, strategy_id, strategy_name, date, "
               "portfolio_id, total_transaction_costs, execution_type, instrument_id) VALUES ($1, $1, "
               "$2, 'SELL', 1, 100, $3::timestamptz, 0, false, $4, $5, $3::date, $6, $7, $8, $9)",
               pqxx::params{exec_id, symbol, date, kId, sleeve, kPortfolio, cost, type, instrument});
        w.commit();
    }
    void results_row(const std::string& date, double daily, double total) {
        pqxx::connection conn(conn_);
        pqxx::work w(conn);
        w.exec("INSERT INTO trading.live_results (strategy_id, portfolio_id, date, daily_roll_costs, "
               "total_roll_costs) VALUES ($1, $2, $3::date, $4, $5)",
               pqxx::params{kId, kPortfolio, date, daily, total});
        w.commit();
    }
    std::shared_ptr<PostgresDatabase> db_;
    std::string conn_;
};

// B8: the legs stored for the date name the contract each symbol was rolled out of; two rolls of one
// symbol legged on one run give the earlier one's (the ids sort by confirming date); other dates,
// STRATEGY rows and opening legs are not read.
TEST_F(RollStateDbTest, TheStoredClosingLegsOfTheDateNameTheContractRolledOutOf) {
    leg("2026-04-29", "EXEC_ZM.v.0_20260427_RC", "ZM.v.0", "ROLL", "42282755", 8.37);
    leg("2026-04-29", "EXEC_ZM.v.0_20260427_RO", "ZM.v.0", "ROLL", "748931", 8.35);
    leg("2026-04-29", "EXEC_ZC.v.0_20260428_RC", "ZC.v.0", "ROLL", "K", 5.0);
    leg("2026-04-29", "EXEC_ZC.v.0_20260428_RO", "ZC.v.0", "ROLL", "N", 5.0);
    leg("2026-04-29", "EXEC_ZC.v.0_20260423_RC", "ZC.v.0", "ROLL", "H", 5.0);
    leg("2026-04-29", "EXEC_ZC.v.0_20260423_RO", "ZC.v.0", "ROLL", "K", 5.0);
    leg("2026-04-29", "EXEC_6A.v.0_20260428", "6A.v.0", "STRATEGY", "", 2.0);
    leg("2026-04-28", "EXEC_ZS.v.0_20260427_RC", "ZS.v.0", "ROLL", "S1", 4.0);
    auto got = db_->get_stored_roll_contracts(kId, kPortfolio, utc(2026, 4, 29), "trading.executions");
    ASSERT_TRUE(got.is_ok()) << got.error()->what();
    ASSERT_EQ(got.value().size(), 2u);
    EXPECT_EQ(got.value().at("ZM.v.0"), "42282755");
    EXPECT_EQ(got.value().at("ZC.v.0"), "H") << "the earlier roll's outgoing contract";
    auto none = db_->get_stored_roll_contracts(kId, kPortfolio, utc(2026, 4, 30), "trading.executions");
    ASSERT_TRUE(none.is_ok());
    EXPECT_TRUE(none.value().empty()) << "a first run of a date has no stored leg";
}

// B4, the adversary's replay: rows to 04-27 (total 0.00); the 04-28 run legged four rolls (68.56) and
// the 04-29 run one (35.62) and both live_results writes failed. The 04-30 run continues from the
// latest stored row: 0.00 + 68.56 + 35.62 = 104.18. Reading the 04-27 row alone restarted at 0.00.
TEST_F(RollStateDbTest, TheRollCostTotalContinuesAcrossDaysWhoseResultsWriteFailed) {
    results_row("2026-04-26", 0.0, 0.0);
    results_row("2026-04-27", 0.0, 0.0);
    leg("2026-04-28", "EXEC_MBT.v.0_20260427_RC", "MBT.v.0", "ROLL", "A", 34.28);
    leg("2026-04-28", "EXEC_MBT.v.0_20260427_RO", "MBT.v.0", "ROLL", "B", 34.28);
    leg("2026-04-28", "EXEC_6A.v.0_20260427", "6A.v.0", "STRATEGY", "", 9.99);
    leg("2026-04-29", "EXEC_ZS.v.0_20260428_RC", "ZS.v.0", "ROLL", "A", 17.80);
    leg("2026-04-29", "EXEC_ZS.v.0_20260428_RO", "ZS.v.0", "ROLL", "B", 17.82);
    auto total = db_->get_previous_total_roll_costs(kId, kPortfolio, utc(2026, 4, 30),
                                                    "trading.live_results", "trading.executions");
    ASSERT_TRUE(total.is_ok()) << total.error()->what();
    EXPECT_NEAR(total.value(), 104.18, 1e-9) << "0.00 on the 04-27 row plus the legs of 04-28 and 04-29";
    // With the rows stored, the same reading: the latest row carries the legs up to its date.
    results_row("2026-04-28", 68.56, 68.56);
    results_row("2026-04-29", 35.62, 104.18);
    total = db_->get_previous_total_roll_costs(kId, kPortfolio, utc(2026, 4, 30),
                                               "trading.live_results", "trading.executions");
    ASSERT_TRUE(total.is_ok());
    EXPECT_NEAR(total.value(), 104.18, 1e-9);
    // A re-run of 04-29 reads the 04-28 row and no leg dated 04-29.
    total = db_->get_previous_total_roll_costs(kId, kPortfolio, utc(2026, 4, 29),
                                               "trading.live_results", "trading.executions");
    ASSERT_TRUE(total.is_ok());
    EXPECT_NEAR(total.value(), 68.56, 1e-9);
}

namespace {
ExecutionReport report(const std::string& exec_id, const std::string& symbol, Side side, double price,
                       ExecutionType type, const std::string& instrument, double cost) {
    ExecutionReport e;
    e.exec_id = exec_id;
    e.order_id = exec_id;
    e.symbol = symbol;
    e.side = side;
    e.filled_quantity = Decimal(1.0);
    e.fill_price = Decimal(price);
    e.fill_time = utc(2026, 4, 29);
    e.total_transaction_costs = Decimal(cost);
    e.commissions_fees = Decimal(cost);
    e.execution_type = type;
    e.instrument_id = instrument;
    return e;
}
}  // namespace

// C5 (finding 1): the re-run of a roll day. The first run stored ZM's two legs dated 04-29 (the late
// roll confirmed 04-27) and a STRATEGY fill; a leg of another roll the re-run no longer books is
// there too. The re-run's replace stores its rows: the stale leg is swept, the two legs and the fill
// are stored once each, another sleeve's and another day's rows are untouched.
TEST_F(RollStateDbTest, AReplaceStoresTheRunsRowsAndSweepsTheDaysOtherRollRows) {
    leg("2026-04-29", "EXEC_ZM.v.0_20260427_RC", "ZM.v.0", "ROLL", "42282755", 8.08);
    leg("2026-04-29", "EXEC_ZM.v.0_20260427_RO", "ZM.v.0", "ROLL", "748931", 8.06);
    leg("2026-04-29", "EXEC_ZC.v.0_20260428_RC", "ZC.v.0", "ROLL", "K", 5.0);  // no longer rolled
    leg("2026-04-29", "EXEC_6A.v.0_20260428", "6A.v.0", "STRATEGY", "", 2.0);
    leg("2026-04-29", "EXEC_ZS.v.0_20260428_RC", "ZS.v.0", "ROLL", "S1", 4.0, "TREND_FOLLOWING_FAST");
    leg("2026-04-28", "EXEC_ZL.v.0_20260427_RC", "ZL.v.0", "ROLL", "L1", 4.0);
    const std::vector<ExecutionReport> rerun = {
        report("EXEC_ZM.v.0_20260427_RC", "ZM.v.0", Side::SELL, 332.20, ExecutionType::ROLL, "42282755", 8.08),
        report("EXEC_ZM.v.0_20260427_RO", "ZM.v.0", Side::BUY, 321.10, ExecutionType::ROLL, "748931", 8.06),
        report("EXEC_6A.v.0_20260428", "6A.v.0", Side::BUY, 0.71, ExecutionType::STRATEGY, "", 2.0)};
    auto replaced = db_->replace_roll_day_executions({{"TREND_FOLLOWING", rerun}}, kId, kPortfolio,
                                                     utc(2026, 4, 29), "trading.executions");
    ASSERT_TRUE(replaced.is_ok()) << replaced.error()->what();
    pqxx::connection conn(conn_);
    pqxx::work w(conn);
    const auto rows = w.exec("SELECT exec_id, strategy_name, date::text FROM trading.executions WHERE "
                             "strategy_id = $1 ORDER BY date, strategy_name, exec_id",
                             pqxx::params{kId});
    std::vector<std::string> got;
    for (const auto& r : rows) {
        got.push_back(r[2].as<std::string>() + " " + r[1].as<std::string>() + " " + r[0].as<std::string>());
    }
    const std::vector<std::string> want = {
        "2026-04-28 TREND_FOLLOWING EXEC_ZL.v.0_20260427_RC",
        "2026-04-29 TREND_FOLLOWING EXEC_6A.v.0_20260428",
        "2026-04-29 TREND_FOLLOWING EXEC_ZM.v.0_20260427_RC",
        "2026-04-29 TREND_FOLLOWING EXEC_ZM.v.0_20260427_RO",
        "2026-04-29 TREND_FOLLOWING_FAST EXEC_ZS.v.0_20260428_RC"};
    EXPECT_EQ(got, want);
}

// C5 (finding 1): a re-run whose store fails (here its third row is refused) leaves the first
// run's legs. The Day T-1 row was re-written by the first run, so those legs are the only record
// of the contract the book held before them (get_stored_roll_contracts); with the sweep committed
// apart from the insert they were deleted and the next run found a re-written row and no leg.
TEST_F(RollStateDbTest, AFailedReplaceLeavesTheLegsTheRunBeforeStored) {
    leg("2026-04-29", "EXEC_ZM.v.0_20260427_RC", "ZM.v.0", "ROLL", "42282755", 8.08);
    leg("2026-04-29", "EXEC_ZM.v.0_20260427_RO", "ZM.v.0", "ROLL", "748931", 8.06);
    const std::vector<ExecutionReport> rerun = {
        report("EXEC_ZM.v.0_20260427_RC", "ZM.v.0", Side::SELL, 332.20, ExecutionType::ROLL, "42282755", 8.08),
        report("EXEC_ZM.v.0_20260427_RO", "ZM.v.0", Side::BUY, 321.10, ExecutionType::ROLL, "748931", 8.06),
        report(std::string(60, 'X'), "6A.v.0", Side::BUY, 0.71, ExecutionType::STRATEGY, "", 2.0)};
    auto replaced = db_->replace_roll_day_executions({{"TREND_FOLLOWING", rerun}}, kId, kPortfolio,
                                                     utc(2026, 4, 29), "trading.executions");
    EXPECT_TRUE(replaced.is_error()) << "an exec_id of 60 characters is refused";
    auto got = db_->get_stored_roll_contracts(kId, kPortfolio, utc(2026, 4, 29), "trading.executions");
    ASSERT_TRUE(got.is_ok()) << got.error()->what();
    ASSERT_EQ(got.value().count("ZM.v.0"), 1u) << "the first run's legs are gone: the state is lost";
    EXPECT_EQ(got.value().at("ZM.v.0"), "42282755");
    pqxx::connection conn(conn_);
    pqxx::work w(conn);
    EXPECT_EQ(w.query_value<int>("SELECT count(*) FROM trading.executions WHERE strategy_id = " +
                                 w.quote(kId)),
              2);
}

// C5 (finding 2): the live runner loads the classifier's history before its window with
// get_market_data and converts it to bars. A range with no bar at all (a symbol list with no
// history that far back) is an EMPTY history: the load returns a zero-row table and the conversion
// an empty vector. The empty table was built on null arrays and the runner died on it (exit 139, no
// message, nothing written). In a child process with its own connection, so a crash is a failed
// test and not a dead test binary.
TEST(K01HistoryDbTest, ABarRangeWithNoRowIsAnEmptyHistory) {
    const std::string c = dsn();
    if (c.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
    {
        PostgresDatabase probe(c);
        if (probe.connect().is_error()) GTEST_SKIP() << "database unreachable";
        probe.disconnect();
    }
    EXPECT_EXIT(
        {
            PostgresDatabase db(c);
            if (db.connect().is_error()) std::_Exit(2);
            auto table = db.get_market_data({"ZM.v.0", "ZS.v.0"}, utc(1971, 1, 4), utc(1971, 5, 3),
                                            AssetClass::FUTURES, DataFrequency::DAILY, "ohlcv");
            if (table.is_error()) std::_Exit(3);
            if (table.value()->num_rows() != 0) std::_Exit(4);
            auto bars = DataConversionUtils::arrow_table_to_bars(table.value());
            std::_Exit(bars.is_ok() && bars.value().empty() ? 0 : 5);
        },
        ::testing::ExitedWithCode(0), "");
}

// C6 (the lead's fix B): every sleeve with ROLL legs is stored in the SAME transaction. Two sleeves
// roll on the run; the second one's store is refused. No row of either sleeve is committed: the
// state is the state before the run, for every sleeve. (One transaction a sleeve left the first
// sleeve's rows behind a run that then stopped.)
TEST_F(RollStateDbTest, AFailedReplaceLeavesNoRowOfAnySleeve) {
    const std::vector<ExecutionReport> first = {
        report("EXEC_ZM.v.0_20260427_RC", "ZM.v.0", Side::SELL, 332.20, ExecutionType::ROLL, "42282755", 8.08),
        report("EXEC_ZM.v.0_20260427_RO", "ZM.v.0", Side::BUY, 321.10, ExecutionType::ROLL, "748931", 8.06)};
    const std::vector<ExecutionReport> second = {
        report("EXEC_ZS.v.0_20260428_RC", "ZS.v.0", Side::SELL, 1040.0, ExecutionType::ROLL, "S1", 4.0),
        report(std::string(60, 'X'), "ZS.v.0", Side::BUY, 1050.0, ExecutionType::ROLL, "S2", 4.0)};
    auto replaced = db_->replace_roll_day_executions(
        {{"TREND_FOLLOWING", first}, {"TREND_FOLLOWING_FAST", second}}, kId, kPortfolio, utc(2026, 4, 29),
        "trading.executions");
    EXPECT_TRUE(replaced.is_error());
    {
        pqxx::connection conn(conn_);
        pqxx::work w(conn);
        EXPECT_EQ(w.query_value<int>("SELECT count(*) FROM trading.executions WHERE strategy_id = " +
                                     w.quote(kId)),
                  0)
            << "a sleeve's rows were committed by a store that failed";
    }
    // With both sleeves' rows valid the same call stores all four.
    auto good = second;
    good[1].exec_id = good[1].order_id = "EXEC_ZS.v.0_20260428_RO";
    ASSERT_TRUE(db_->replace_roll_day_executions({{"TREND_FOLLOWING", first}, {"TREND_FOLLOWING_FAST", good}}, kId,
                                                 kPortfolio, utc(2026, 4, 29), "trading.executions")
                    .is_ok());
    pqxx::connection conn(conn_);
    pqxx::work w(conn);
    EXPECT_EQ(w.query_value<int>("SELECT count(*) FROM trading.executions WHERE strategy_id = " + w.quote(kId)), 4);
}

// C6 (finding 4): the stored rows a late roll's settlement reads. Every sleeve's rows of the book
// dated strictly after the first date and strictly before the second, with their quantity and
// daily realised P&L; other books, other dates and the two bounds are not read.
TEST_F(RollStateDbTest, TheStoredRealisedRowsBetweenTwoDatesAreRead) {
    {
        pqxx::connection conn(conn_);
        pqxx::work w(conn);
        w.exec("DELETE FROM trading.positions WHERE strategy_id = '" + kId + "'");
        auto row = [&](const std::string& date, const std::string& symbol, const std::string& sleeve, double q,
                       double pnl, const std::string& portfolio) {
            w.exec("INSERT INTO trading.positions (symbol, quantity, average_price, daily_unrealized_pnl, "
                   "daily_realized_pnl, last_update, updated_at, strategy_id, strategy_name, date, portfolio_id) "
                   "VALUES ($1, $2, 100, 0, $3, $4::timestamptz, now(), $5, $6, $4::date, $7)",
                   pqxx::params{symbol, q, pnl, date, kId, sleeve, portfolio});
        };
        row("2026-04-20", "ZM.v.0", "TREND_FOLLOWING", 1, 11.0, kPortfolio);       // the lower bound
        row("2026-04-27", "ZM.v.0", "TREND_FOLLOWING", 1, 0.0, kPortfolio);
        row("2026-04-28", "ZM.v.0", "TREND_FOLLOWING", 1, 770.0, kPortfolio);
        row("2026-04-28", "ZM.v.0", "TREND_FOLLOWING_FAST", -2, -1540.0, kPortfolio);
        row("2026-04-28", "ZS.v.0", "TREND_FOLLOWING", 3, 45.5, kPortfolio);
        row("2026-04-28", "ZM.v.0", "TREND_FOLLOWING", 1, 5.0, "TROLLX_OTHER_PORTFOLIO");
        row("2026-04-29", "ZM.v.0", "TREND_FOLLOWING", 1, 22.0, kPortfolio);       // the upper bound
        w.commit();
    }
    auto got = db_->get_stored_realised_rows(kId, kPortfolio, "2026-04-20", "2026-04-29", "trading.positions");
    {
        pqxx::connection conn(conn_);
        pqxx::work w(conn);
        w.exec("DELETE FROM trading.positions WHERE strategy_id = '" + kId + "'");
        w.commit();
    }
    ASSERT_TRUE(got.is_ok()) << got.error()->what();
    std::vector<std::string> rows;
    for (const auto& r : got.value()) {
        rows.push_back(r.date + " " + r.symbol + " " + r.sleeve + " " + std::to_string(r.quantity) + " " +
                       std::to_string(r.realised));
    }
    const std::vector<std::string> want = {
        "2026-04-27 ZM.v.0 TREND_FOLLOWING 1.000000 0.000000",
        "2026-04-28 ZM.v.0 TREND_FOLLOWING 1.000000 770.000000",
        "2026-04-28 ZM.v.0 TREND_FOLLOWING_FAST -2.000000 -1540.000000",
        "2026-04-28 ZS.v.0 TREND_FOLLOWING 3.000000 45.500000",
    };
    EXPECT_EQ(rows, want);
    EXPECT_TRUE(db_->get_stored_realised_rows(kId, kPortfolio, "2026-04-20", "2026-04-29", "trading.positions; --")
                    .is_error())
        << "the table name is validated";
}
