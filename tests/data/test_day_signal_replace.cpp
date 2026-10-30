// tests/data/test_day_signal_replace.cpp
//
// A live re-run of a date rewrites the sleeve's trading.signals rows of that day as a set, in
// the transaction of the store. An upsert a symbol with no delete would keep the earlier run's
// row of a symbol the re-run no longer produces.
//
// The first tests EXECUTE replace_day_signals_in, the function PostgresDatabase::store_signals
// runs on its pqxx transaction, against an in-memory stand-in: no database is needed, and the
// stand-in's store can be made to fail. The fixture below runs PostgresDatabase::store_signals
// itself on a real server (TRADE_NGIN_TEST_DSN; skipped visibly without one), under portfolio ids
// no book uses; every row it writes is removed in SetUp and TearDown.

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/day_signal_replace.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {

struct Row {
    int id;  ///< the row's identity: a row written again keeps it
    std::string day, time, portfolio, strategy_id, sleeve, symbol;
    double signal;
    bool operator==(const Row& o) const {
        return std::tie(id, day, time, portfolio, strategy_id, sleeve, symbol, signal) ==
               std::tie(o.id, o.day, o.time, o.portfolio, o.strategy_id, o.sleeve, o.symbol,
                        o.signal);
    }
    bool operator<(const Row& o) const {
        return id < o.id;
    }
};

// A transaction over a table of rows: every step works on a pending copy, commit() makes it the
// table, and a stand-in that is never committed leaves the table as it was (a rollback). The
// store is an upsert on the row's key, as the writer's statement is.
struct StandInTxn {
    std::vector<Row>& table;
    std::vector<Row> pending;
    std::string time;            ///< the run's time of day
    Row scope{};                 ///< what the sweep was asked for; the stored rows carry it
    std::string fail_store_for;  ///< the symbol whose store fails; empty: none
    int next_id{100};
    int sweeps{0};
    bool committed{false};

    StandInTxn(std::vector<Row>& t, std::string run_time)
        : table(t), pending(t), time(std::move(run_time)) {}

    bool in_scope(const Row& r) const {
        return r.day == scope.day && r.portfolio == scope.portfolio &&
               r.strategy_id == scope.strategy_id && r.sleeve == scope.sleeve;
    }
    void sweep(const std::string& strategy_id, const std::string& sleeve,
               const std::string& portfolio, const std::string& day,
               const std::vector<std::string>& symbols) {
        ++sweeps;
        scope = {0, day, time, portfolio, strategy_id, sleeve, "", 0.0};
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [&](const Row& r) {
                                         const bool written =
                                             r.time == time &&
                                             std::find(symbols.begin(), symbols.end(), r.symbol) !=
                                                 symbols.end();
                                         return in_scope(r) && !written;
                                     }),
                      pending.end());
    }
    Result<void> store(const std::string& symbol, double signal) {
        if (symbol == fail_store_for) {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "the store failed", "StandInTxn");
        }
        for (auto& r : pending) {
            if (in_scope(r) && r.time == time && r.symbol == symbol) {
                r.signal = signal;
                return Result<void>();
            }
        }
        pending.push_back({next_id++, scope.day, time, scope.portfolio, scope.strategy_id,
                           scope.sleeve, symbol, signal});
        return Result<void>();
    }
    void commit() {
        table = pending;
        committed = true;
    }
};

const std::string kDay = "2026-04-25";
const std::string kRun = "05:00";
const std::string kBook = "CONSERVATIVE_PORTFOLIO";
const std::string kId = "LIVE_TREND_FOLLOWING";
const std::string kSleeve = "TREND_FOLLOWING";

std::vector<Row> first_run() {
    return {{1, "2026-04-24", kRun, kBook, kId, kSleeve, "A", 1.0},
            {2, kDay, kRun, kBook, kId, kSleeve, "A", 1.0},
            {3, kDay, kRun, kBook, kId, kSleeve, "B", 2.0},  // the re-run no longer produces it
            {4, kDay, kRun, kBook, kId, "TREND_FOLLOWING_FAST", "B", 4.0},
            {5, kDay, kRun, "BASE_PORTFOLIO", kId, kSleeve, "B", 5.0},
            {6, kDay, kRun, kBook, "LIVE_OTHER", kSleeve, "B", 6.0}};
}

std::vector<Row> sorted(std::vector<Row> rows) {
    std::sort(rows.begin(), rows.end());
    return rows;
}

}  // namespace

//   | the sleeve's rows of 04-25 | first run | re-run produces | upsert          | replace      |
//   | A (row 2)                  | 1.0       | 3.0             | 3.0, row 2      | 3.0, row 2   |
//   | B (row 3)                  | 2.0       | nothing         | 2.0 (stale)     | gone         |
TEST(DaySignalReplace, ASymbolTheReRunNoLongerProducesIsGoneAndNothingElseIsTouched) {
    std::vector<Row> table = first_run();
    StandInTxn txn(table, kRun);
    const auto replaced = replace_day_signals_in(txn, {{"A", 3.0}}, kId, kSleeve, kBook, kDay);
    ASSERT_TRUE(replaced.is_ok());
    EXPECT_TRUE(txn.committed);
    EXPECT_EQ(txn.sweeps, 1);

    const std::vector<Row> want = {
        {1, "2026-04-24", kRun, kBook, kId, kSleeve, "A", 1.0},
        {2, kDay, kRun, kBook, kId, kSleeve, "A", 3.0},  // the same row, this run's value
        {4, kDay, kRun, kBook, kId, "TREND_FOLLOWING_FAST", "B", 4.0},
        {5, kDay, kRun, "BASE_PORTFOLIO", kId, kSleeve, "B", 5.0},
        {6, kDay, kRun, kBook, "LIVE_OTHER", kSleeve, "B", 6.0}};
    EXPECT_EQ(sorted(table), want)
        << "another day, another sleeve, another portfolio and another strategy id stay, and "
           "the row written again keeps its identity";
}

// An earlier run of the day stored its rows at another time of day (a run on the wall clock):
// the upsert's key holds the timestamp, so an upsert keeps both rows of the symbol.
//
//   | the sleeve's rows of 04-25 | earlier run at 17:00 | re-run at 05:00 | after              |
//   | A                          | row 2, 1.0           | 3.0             | one row: 05:00 3.0 |
//   | B                          | row 3, 2.0           | nothing         | none               |
TEST(DaySignalReplace, ARowOfTheDayAtAnotherTimeOfDayIsSweptSoASymbolHasOneRowADay) {
    std::vector<Row> table = {{2, kDay, "17:00", kBook, kId, kSleeve, "A", 1.0},
                              {3, kDay, "17:00", kBook, kId, kSleeve, "B", 2.0}};
    StandInTxn txn(table, kRun);
    ASSERT_TRUE(replace_day_signals_in(txn, {{"A", 3.0}}, kId, kSleeve, kBook, kDay).is_ok());
    const std::vector<Row> want = {{100, kDay, kRun, kBook, kId, kSleeve, "A", 3.0}};
    EXPECT_EQ(table, want);
}

TEST(DaySignalReplace, AFirstRunOfADayStoresItsRowsAndRemovesNone) {
    std::vector<Row> table = {{1, "2026-04-24", kRun, kBook, kId, kSleeve, "A", 1.0}};
    StandInTxn txn(table, kRun);
    ASSERT_TRUE(
        replace_day_signals_in(txn, {{"A", 3.0}, {"B", 2.0}}, kId, kSleeve, kBook, kDay).is_ok());
    ASSERT_EQ(table.size(), 3u);
    EXPECT_EQ(table[0], (Row{1, "2026-04-24", kRun, kBook, kId, kSleeve, "A", 1.0}));
    std::map<std::string, double> stored;
    for (size_t i = 1; i < table.size(); ++i) {
        EXPECT_EQ(table[i].day, kDay);
        stored[table[i].symbol] = table[i].signal;
    }
    const std::map<std::string, double> want = {{"A", 3.0}, {"B", 2.0}};
    EXPECT_EQ(stored, want);
}

TEST(DaySignalReplace, AStoreThatFailsCommitsNothingSoTheDaysRowsAreTheEarlierRuns) {
    std::vector<Row> table = first_run();
    const std::vector<Row> before = table;
    StandInTxn txn(table, kRun);
    txn.fail_store_for = "A";
    const auto replaced = replace_day_signals_in(txn, {{"A", 3.0}}, kId, kSleeve, kBook, kDay);
    ASSERT_TRUE(replaced.is_error());
    EXPECT_EQ(std::string(replaced.error()->what()), "the store failed");
    EXPECT_FALSE(txn.committed);
    EXPECT_EQ(table, before) << "the sweep was not committed apart from the store";
}

// The statement the sweep issues: scoped by the portfolio, the strategy id, the sleeve and the
// day, and sparing exactly the rows this run writes (its symbols at its timestamp).
TEST(DaySignalReplace, TheSweepStatementIsScopedToTheSleevesDayAndSparesTheRowsTheRunWrites) {
    EXPECT_EQ(day_signal_sweep_statement("trading.signals"),
              "DELETE FROM trading.signals WHERE portfolio_id = $1 AND strategy_id = $2 AND "
              "strategy_name = $3 AND \"timestamp\"::date = $4::date AND NOT (symbol = ANY($5) AND "
              "\"timestamp\" = $6::timestamptz)");
}

// ---------------------------------------------------------------------------------------------
// PostgresDatabase::store_signals on a real server.

namespace {

constexpr const char* kTestBook = "T3_TEST_PORTFOLIO";
constexpr const char* kOtherBook = "T3_TEST_PORTFOLIO_OTHER";
constexpr const char* kTestId = "T3_TEST_STRATEGY_ID";
constexpr const char* kTestSleeve = "T3_TEST_SLEEVE";
constexpr const char* kOtherSleeve = "T3_TEST_SLEEVE_FAST";

Timestamp at_utc(int year, int month, int day, int hour) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

}  // namespace

class DaySignalReplaceDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
        if (!dsn || !*dsn) {
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set; no database to exercise";
        }
        conn_ = dsn;
        db_ = std::make_shared<PostgresDatabase>(conn_);
        auto connected = db_->connect();
        ASSERT_TRUE(connected.is_ok() && db_->is_connected()) << "the test database is unreachable";
        purge();
    }

    void TearDown() override {
        if (db_ && db_->is_connected())
            purge();
    }

    void purge() {
        pqxx::connection c(conn_);
        pqxx::work txn(c);
        txn.exec("DELETE FROM trading.signals WHERE portfolio_id IN (" + txn.quote(kTestBook) +
                 ", " + txn.quote(kOtherBook) + ")");
        txn.commit();
    }

    // symbol -> signal of the rows of one portfolio, sleeve and UTC day.
    std::map<std::string, double> rows(const std::string& portfolio, const std::string& sleeve,
                                       const std::string& day) {
        pqxx::connection c(conn_);
        pqxx::work txn(c);
        auto r = txn.exec(
            "SELECT symbol, signal_value::float8 FROM trading.signals WHERE portfolio_id = $1 AND "
            "strategy_name = $2 AND (\"timestamp\" AT TIME ZONE 'UTC')::date = $3::date",
            pqxx::params{portfolio, sleeve, day});
        std::map<std::string, double> out;
        for (const auto& row : r) {
            EXPECT_EQ(out.count(row[0].as<std::string>()), 0u) << "two rows of one symbol-day";
            out[row[0].as<std::string>()] = row[1].as<double>();
        }
        return out;
    }

    // id and created_at of the one row of a symbol on a day, as text.
    std::string identity(const std::string& portfolio, const std::string& sleeve,
                         const std::string& day, const std::string& symbol) {
        pqxx::connection c(conn_);
        pqxx::work txn(c);
        auto r = txn.exec(
            "SELECT id::text || ' ' || created_at::text FROM trading.signals WHERE portfolio_id = "
            "$1 AND strategy_name = $2 AND (\"timestamp\" AT TIME ZONE 'UTC')::date = $3::date AND "
            "symbol = $4",
            pqxx::params{portfolio, sleeve, day, symbol});
        return r.size() == 1 ? r[0][0].as<std::string>() : "rows: " + std::to_string(r.size());
    }

    Result<void> store(const std::unordered_map<std::string, double>& signals,
                       const std::string& portfolio, const std::string& sleeve,
                       const Timestamp& when) {
        return db_->store_signals(signals, kTestId, sleeve, portfolio, when, "trading.signals");
    }

    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

//   | rows (portfolio, sleeve, day)             | first stores | re-store | after    |
//   | T3_TEST_PORTFOLIO, sleeve, 04-25          | A 1, B 2     | A 3      | A 3      |
//   | T3_TEST_PORTFOLIO, sleeve, 04-24          | A 1, B 2     |          | A 1, B 2 |
//   | T3_TEST_PORTFOLIO, other sleeve, 04-25    | A 1, B 2     |          | A 1, B 2 |
//   | T3_TEST_PORTFOLIO_OTHER, sleeve, 04-25    | A 1, B 2     |          | A 1, B 2 |
//
// An upsert fails it: the first row keeps B 2 (the upsert deletes nothing).
TEST_F(DaySignalReplaceDbTest, AReStoreOfADayRemovesTheSymbolItNoLongerProduces) {
    const std::unordered_map<std::string, double> first = {{"T3A", 1.0}, {"T3B", 2.0}};
    const std::map<std::string, double> kept = {{"T3A", 1.0}, {"T3B", 2.0}};
    const Timestamp day = at_utc(2026, 4, 25, 5);
    ASSERT_TRUE(store(first, kTestBook, kTestSleeve, day).is_ok());
    ASSERT_TRUE(store(first, kTestBook, kTestSleeve, at_utc(2026, 4, 24, 5)).is_ok());
    ASSERT_TRUE(store(first, kTestBook, kOtherSleeve, day).is_ok());
    ASSERT_TRUE(store(first, kOtherBook, kTestSleeve, day).is_ok());
    ASSERT_EQ(rows(kTestBook, kTestSleeve, "2026-04-25"), kept);
    const std::string first_identity = identity(kTestBook, kTestSleeve, "2026-04-25", "T3A");

    auto restored = store({{"T3A", 3.0}}, kTestBook, kTestSleeve, day);
    ASSERT_TRUE(restored.is_ok()) << restored.error()->what();
    EXPECT_EQ(identity(kTestBook, kTestSleeve, "2026-04-25", "T3A"), first_identity)
        << "the row written again keeps its id and its created_at (the first insert)";

    const std::map<std::string, double> want = {{"T3A", 3.0}};
    EXPECT_EQ(rows(kTestBook, kTestSleeve, "2026-04-25"), want)
        << "the symbol the re-store does not produce kept its earlier row";
    EXPECT_EQ(rows(kTestBook, kTestSleeve, "2026-04-24"), kept) << "another day";
    EXPECT_EQ(rows(kTestBook, kOtherSleeve, "2026-04-25"), kept) << "another sleeve of the book";
    EXPECT_EQ(rows(kOtherBook, kTestSleeve, "2026-04-25"), kept) << "another portfolio";
}

// An earlier run of the day stored at another time of day (17:00Z); the re-run stores at 05:00Z.
// An upsert fails it: three rows (A and B at 17:00Z, A at 05:00Z).
TEST_F(DaySignalReplaceDbTest, AReStoreAtAnotherTimeOfTheDayLeavesOneRowASymbol) {
    ASSERT_TRUE(store({{"T3A", 1.0}, {"T3B", 2.0}}, kTestBook, kTestSleeve, at_utc(2026, 4, 25, 17))
                    .is_ok());
    ASSERT_TRUE(store({{"T3A", 3.0}}, kTestBook, kTestSleeve, at_utc(2026, 4, 25, 5)).is_ok());
    const std::map<std::string, double> want = {{"T3A", 3.0}};
    EXPECT_EQ(rows(kTestBook, kTestSleeve, "2026-04-25"), want);
}

// The sweep and the store are one transaction: a re-store with a row the writer refuses (a
// signal that is not finite) returns the error and the day's earlier rows are all still there.
// A pin: an upsert, which deletes nothing, passes it too.
TEST_F(DaySignalReplaceDbTest, AReStoreThatFailsLeavesTheEarlierRows) {
    const std::map<std::string, double> kept = {{"T3A", 1.0}, {"T3B", 2.0}};
    const Timestamp day = at_utc(2026, 4, 25, 5);
    ASSERT_TRUE(store({{"T3A", 1.0}, {"T3B", 2.0}}, kTestBook, kTestSleeve, day).is_ok());

    auto refused =
        store({{"T3C", std::numeric_limits<double>::quiet_NaN()}}, kTestBook, kTestSleeve, day);
    ASSERT_TRUE(refused.is_error());
    EXPECT_EQ(rows(kTestBook, kTestSleeve, "2026-04-25"), kept);
}
