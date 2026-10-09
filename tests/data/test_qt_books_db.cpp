// QT platform E1 (migration 021): three books -- system, qt_proposal, qt -- side by side in
// trading.positions, trading.executions, trading.live_results and trading.equity_curve, and no
// write of one book touching another.
//
// What is pinned, against a real server:
//   (a) writing (and re-writing) a qt or qt_proposal book for a date leaves the system rows
//       byte-identical, and the reverse;
//   (b) every read filters by book;
//   (c) a book outside the three is refused and writes nothing;
//   (d) every call that names no book reads and writes system, exactly as before 021.
//
// The database must have migration 021 applied (and so 001). A throwaway one is built with
//   psql -f tests/data/fixtures/qt_books_schema.sql && psql -f migrations/021_qt_books.sql
// Rows are written only under the scratch identities below and removed in SetUp and TearDown.
//
// Reachability gate as tests/data/test_delete_stale_executions_scope_db.cpp:
//   * TRADE_NGIN_REQUIRE_DB=1 -- an unreachable database FAILS rather than skips.
//   * unset -- skip (local dev and CI without a server).

#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <string>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/storage/live_results_manager.hpp"

using namespace trade_ngin;

namespace {

constexpr const char* kSid = "QTBOOKS_PROBE_ID";
constexpr const char* kSleeve = "QTBOOKS_PROBE_SLEEVE";
constexpr const char* kPid = "QTBOOKS_PROBE_PORTFOLIO";
constexpr const char* kExecs = "trading.executions";
constexpr const char* kPos = "trading.positions";
constexpr const char* kLive = "trading.live_results";
constexpr const char* kCurve = "trading.equity_curve";

std::string dsn() {
    const char* d = std::getenv("TRADE_NGIN_TEST_DSN");
    return (d && *d) ? std::string(d) : std::string();
}

Timestamp at(int y, int m, int d, int h = 21) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

Position pos(const std::string& symbol, double qty, double price, const Timestamp& ts) {
    Position p(symbol, Quantity(qty), Price(price), Decimal(0.0), Decimal(0.0), ts);
    return p;
}

ExecutionReport fill(const std::string& symbol, double qty, const Timestamp& ts,
                     ExecutionType type = ExecutionType::STRATEGY,
                     const std::string& suffix = "") {
    ExecutionReport e;
    e.symbol = symbol;
    e.order_id = "DAILY_" + symbol + "_20260424" + suffix;  // the system id, unchanged per book
    e.exec_id = "EXEC_" + symbol + "_20260424" + suffix;
    e.side = Side::BUY;
    e.filled_quantity = Quantity(qty);
    e.fill_price = Price(100.0);
    e.fill_time = ts;
    e.commissions_fees = Decimal(1.0);
    e.implicit_price_impact = Decimal(0.0);
    e.slippage_market_impact = Decimal(0.0);
    e.total_transaction_costs = Decimal(qty);
    e.is_partial = false;
    e.execution_type = type;
    if (type == ExecutionType::ROLL) e.instrument_id = "C" + suffix;
    return e;
}

}  // namespace

class QtBooksDb : public ::testing::Test {
protected:
    void SetUp() override {
        const char* r = std::getenv("TRADE_NGIN_REQUIRE_DB");
        const bool require = r && std::string(r) == "1";
        conn_ = dsn();
        if (conn_.empty()) {
            if (require) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set";
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set; no database to exercise";
        }
        db_ = std::make_shared<PostgresDatabase>(conn_);
        if (db_->connect().is_error() || !db_->is_connected()) {
            if (require) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable";
            GTEST_SKIP() << "database unreachable";
        }
        if (scalar("SELECT count(*) FROM information_schema.columns WHERE table_schema = "
                   "'trading' AND table_name = 'executions' AND column_name = 'portfolio_type'") !=
            "1") {
            FAIL() << "the test database lacks migration 021 (trading.executions.portfolio_type)";
        }
        purge();
    }

    void TearDown() override {
        if (db_ && db_->is_connected()) {
            purge();
            db_->disconnect();
        }
    }

    void purge() {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        for (const char* t : {kPos, kExecs, kLive}) {
            w.exec(std::string("DELETE FROM ") + t + " WHERE strategy_id = $1 OR portfolio_id = $2",
                   pqxx::params{kSid, kPid});
        }
        w.exec(std::string("DELETE FROM ") + kCurve + " WHERE strategy_id = $1 OR portfolio_id = $2",
               pqxx::params{kSid, kPid});
        w.commit();
    }

    std::string scalar(const std::string& sql) {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        auto r = w.exec(sql);
        w.commit();
        return (r.empty() || r[0][0].is_null()) ? std::string() : r[0][0].as<std::string>();
    }

    // A fingerprint of one book's scratch rows in one table: row count and the md5 of every
    // column of every row, so "byte-identical" means byte-identical.
    std::string fp(const char* table, const std::string& book) {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        auto r = w.exec(std::string("SELECT count(*) || ' ' || md5(coalesce(string_agg(x, ';' ORDER BY x), '')) "
                                    "FROM (SELECT to_jsonb(t)::text x FROM ") +
                            table +
                            " t WHERE (strategy_id = $1 OR portfolio_id = $2) AND portfolio_type = $3) s",
                        pqxx::params{kSid, kPid, book});
        w.commit();
        return r[0][0].as<std::string>();
    }

    int count(const char* table, const std::string& book) {
        const auto f = fp(table, book);
        return std::stoi(f.substr(0, f.find(' ')));
    }

    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

// (a) + (d), positions: the default call writes system; a qt and a qt_proposal write of the same
// sleeve, portfolio and date -- and a re-run of each -- leave it byte-identical; a system re-run
// leaves both desk books byte-identical.
TEST_F(QtBooksDb, PositionsBooksNeverTouchEachOther) {
    const auto d = at(2026, 4, 24);
    ASSERT_TRUE(db_->store_positions({pos("ES.v.0", 3, 5120.25, d), pos("NQ.v.0", -1, 18000, d)},
                                     kSid, kSleeve, kPid, kPos)
                    .is_ok());
    ASSERT_EQ(count(kPos, "system"), 2) << "the default call did not write system";
    const auto sys = fp(kPos, "system");

    for (const char* book : {"qt_proposal", "qt"}) {
        for (int rerun = 0; rerun < 2; ++rerun) {
            auto r = db_->store_positions({pos("ES.v.0", 5 + rerun, 5120.25, d)}, kSid, kSleeve,
                                          kPid, kPos, book);
            ASSERT_TRUE(r.is_ok()) << book << ": " << r.error()->what();
            EXPECT_EQ(fp(kPos, "system"), sys) << "a " << book << " write moved the system rows";
        }
        EXPECT_EQ(count(kPos, book), 1) << book;
    }
    const auto prop = fp(kPos, "qt_proposal");
    const auto qt = fp(kPos, "qt");
    ASSERT_TRUE(db_->store_positions({pos("ES.v.0", 4, 5121, d)}, kSid, kSleeve, kPid, kPos).is_ok());
    EXPECT_EQ(count(kPos, "system"), 1) << "the system re-run did not replace its own rows";
    EXPECT_EQ(fp(kPos, "qt_proposal"), prop) << "a system re-run moved qt_proposal";
    EXPECT_EQ(fp(kPos, "qt"), qt) << "a system re-run moved qt";

    // The unit-of-work overload is book-scoped the same way.
    auto uow = db_->begin_unit_of_work();
    ASSERT_TRUE(uow.is_ok());
    ASSERT_TRUE(db_->store_positions(*uow.value(), {pos("ES.v.0", 9, 1, d)}, kSid, kSleeve, kPid,
                                     kPos, "qt")
                    .is_ok());
    ASSERT_TRUE(uow.value()->commit().is_ok());
    EXPECT_EQ(scalar("SELECT quantity::float8::text FROM trading.positions WHERE strategy_id = "
                     "'QTBOOKS_PROBE_ID' AND portfolio_type = 'system'"),
              "4");
    EXPECT_EQ(fp(kPos, "qt_proposal"), prop);
}

// (b) + (d), positions: the default load reads system; each book reads its own rows.
TEST_F(QtBooksDb, LoadPositionsFiltersByBook) {
    const auto d = at(2026, 4, 24);
    ASSERT_TRUE(db_->store_positions({pos("ES.v.0", 3, 1, d)}, kSid, kSleeve, kPid, kPos).is_ok());
    ASSERT_TRUE(db_->store_positions({pos("ES.v.0", 7, 1, d), pos("ZN.v.0", 2, 1, d)}, kSid,
                                     kSleeve, kPid, kPos, "qt_proposal")
                    .is_ok());
    ASSERT_TRUE(db_->store_positions({pos("ES.v.0", 6, 1, d)}, kSid, kSleeve, kPid, kPos, "qt").is_ok());

    auto sys = db_->load_positions_by_date(kSid, kSleeve, kPid, d, kPos);
    ASSERT_TRUE(sys.is_ok()) << sys.error()->what();
    ASSERT_EQ(sys.value().size(), 1u);
    EXPECT_DOUBLE_EQ(static_cast<double>(sys.value().at("ES.v.0").quantity), 3.0);

    auto prop = db_->load_positions_by_date(kSid, kSleeve, kPid, d, kPos, "qt_proposal");
    ASSERT_TRUE(prop.is_ok());
    ASSERT_EQ(prop.value().size(), 2u);
    EXPECT_DOUBLE_EQ(static_cast<double>(prop.value().at("ES.v.0").quantity), 7.0);

    auto qt = db_->load_positions_by_date(kSid, "", kPid, d, kPos, "qt");  // the aggregate form
    ASSERT_TRUE(qt.is_ok());
    ASSERT_EQ(qt.value().size(), 1u);
    EXPECT_DOUBLE_EQ(static_cast<double>(qt.value().at("ES.v.0").quantity), 6.0);
}

// moved_by: written on qt and read back; refused on system and qt_proposal before anything is
// written; NULL on a row that names none.
TEST_F(QtBooksDb, MovedByIsQtOnly) {
    const auto d = at(2026, 4, 24);
    auto moved = pos("ES.v.0", 2, 1, d);
    moved.moved_by = "cap_bound";
    ASSERT_TRUE(db_->store_positions({moved, pos("NQ.v.0", 1, 1, d)}, kSid, kSleeve, kPid, kPos, "qt").is_ok());
    auto qt = db_->load_positions_by_date(kSid, kSleeve, kPid, d, kPos, "qt");
    ASSERT_TRUE(qt.is_ok());
    EXPECT_EQ(qt.value().at("ES.v.0").moved_by, "cap_bound");
    EXPECT_EQ(qt.value().at("NQ.v.0").moved_by, "");

    ASSERT_TRUE(db_->store_positions({pos("ES.v.0", 3, 1, d)}, kSid, kSleeve, kPid, kPos).is_ok());
    const auto sys = fp(kPos, "system");
    EXPECT_TRUE(db_->store_positions({moved}, kSid, kSleeve, kPid, kPos).is_error());
    EXPECT_TRUE(db_->store_positions({moved}, kSid, kSleeve, kPid, kPos, "qt_proposal").is_error());
    EXPECT_EQ(fp(kPos, "system"), sys) << "a refused write deleted rows";
    EXPECT_EQ(count(kPos, "qt_proposal"), 0);
}

// (a) + (d), executions: the same order and exec ids (the system ids, unchanged) coexist across
// books; a desk store -- which pre-deletes its order ids -- and a desk stale-delete leave system
// byte-identical; the default calls write and delete system only.
TEST_F(QtBooksDb, ExecutionsBooksNeverTouchEachOther) {
    const auto d = at(2026, 4, 24);
    ASSERT_TRUE(db_->store_executions({fill("ES.v.0", 1, d), fill("NQ.v.0", 2, d)}, kSid, kSleeve,
                                      kPid, kExecs)
                    .is_ok());
    ASSERT_EQ(count(kExecs, "system"), 2) << "the default call did not write system";
    const auto sys = fp(kExecs, "system");

    for (int rerun = 0; rerun < 2; ++rerun) {
        auto r = db_->store_executions({fill("ES.v.0", 4, d)}, kSid, kSleeve, kPid, kExecs, "qt");
        ASSERT_TRUE(r.is_ok()) << r.error()->what();
    }
    ASSERT_TRUE(db_->store_executions({fill("ES.v.0", 5, d)}, kSid, kSleeve, kPid, kExecs, "qt_proposal").is_ok());
    EXPECT_EQ(fp(kExecs, "system"), sys) << "a desk store moved the system executions";
    EXPECT_EQ(count(kExecs, "qt"), 1) << "the qt re-run did not replace its own row";
    EXPECT_EQ(scalar("SELECT order_id FROM trading.executions WHERE strategy_id = "
                     "'QTBOOKS_PROBE_ID' AND portfolio_type = 'qt'"),
              "DAILY_ES.v.0_20260424")
        << "the order id must stay the system id; the book in the key keeps them apart";

    const auto qt = fp(kExecs, "qt");
    ASSERT_TRUE(db_->delete_stale_executions({"DAILY_ES.v.0_20260424"}, d, kSleeve, kPid, kExecs,
                                             "qt_proposal")
                    .is_ok());
    EXPECT_EQ(count(kExecs, "qt_proposal"), 0);
    EXPECT_EQ(fp(kExecs, "system"), sys);
    EXPECT_EQ(fp(kExecs, "qt"), qt);

    ASSERT_TRUE(db_->delete_stale_executions({"DAILY_ES.v.0_20260424"}, d, kSleeve, kPid, kExecs).is_ok());
    EXPECT_EQ(count(kExecs, "system"), 1) << "the default stale-delete did not delete system";
    EXPECT_EQ(fp(kExecs, "qt"), qt) << "the default stale-delete reached into qt";
}

// The ROLL paths: the sweep and the one-transaction replace are book-scoped, and the reads of
// stored legs, roll costs and realised rows filter by book (default system).
TEST_F(QtBooksDb, RollPathsAreBookScoped) {
    const auto d = at(2026, 4, 24);
    const auto next = at(2026, 4, 27);
    ASSERT_TRUE(db_->store_executions({fill("ES.v.0", 1, d, ExecutionType::ROLL, "_RC")}, kSid,
                                      kSleeve, kPid, kExecs)
                    .is_ok());
    ASSERT_TRUE(db_->store_executions({fill("ES.v.0", 1, d, ExecutionType::ROLL, "_QRC")}, kSid,
                                      kSleeve, kPid, kExecs, "qt")
                    .is_ok());
    const auto sys = fp(kExecs, "system");

    ASSERT_TRUE(db_->delete_roll_executions(d, kSleeve, kPid, kExecs, "qt").is_ok());
    EXPECT_EQ(count(kExecs, "qt"), 0);
    EXPECT_EQ(fp(kExecs, "system"), sys) << "the qt ROLL sweep reached into system";

    std::vector<std::pair<std::string, std::vector<ExecutionReport>>> sleeves{
        {kSleeve, {fill("ES.v.0", 3, d, ExecutionType::ROLL, "_RC")}}};
    ASSERT_TRUE(db_->replace_roll_day_executions(sleeves, kSid, kPid, d, kExecs, "qt").is_ok());
    EXPECT_EQ(count(kExecs, "qt"), 1);
    EXPECT_EQ(fp(kExecs, "system"), sys) << "the qt replace reached into system";

    auto legs_sys = db_->get_stored_roll_contracts(kSid, kPid, d, kExecs);
    auto legs_qt = db_->get_stored_roll_contracts(kSid, kPid, d, kExecs, "qt");
    ASSERT_TRUE(legs_sys.is_ok() && legs_qt.is_ok());
    EXPECT_EQ(legs_sys.value().at("ES.v.0"), "C_RC");
    EXPECT_EQ(legs_qt.value().at("ES.v.0"), "C_RC");  // same id, its own row
    ASSERT_TRUE(db_->delete_roll_executions(d, kSleeve, kPid, kExecs).is_ok());
    EXPECT_EQ(count(kExecs, "system"), 0);
    EXPECT_EQ(count(kExecs, "qt"), 1) << "the default ROLL sweep reached into qt";

    auto costs_sys = db_->get_previous_total_roll_costs(kSid, kPid, next, kLive, kExecs);
    auto costs_qt = db_->get_previous_total_roll_costs(kSid, kPid, next, kLive, kExecs, "qt");
    ASSERT_TRUE(costs_sys.is_ok() && costs_qt.is_ok());
    EXPECT_DOUBLE_EQ(costs_sys.value(), 0.0);
    EXPECT_DOUBLE_EQ(costs_qt.value(), 3.0);

    ASSERT_TRUE(db_->store_positions({pos("ES.v.0", 1, 1, d)}, kSid, kSleeve, kPid, kPos).is_ok());
    ASSERT_TRUE(db_->store_positions({pos("ES.v.0", 2, 1, d), pos("NQ.v.0", 1, 1, d)}, kSid,
                                     kSleeve, kPid, kPos, "qt")
                    .is_ok());
    auto rows_sys = db_->get_stored_realised_rows(kSid, kPid, "2026-04-23", "2026-04-25", kPos);
    auto rows_qt = db_->get_stored_realised_rows(kSid, kPid, "2026-04-23", "2026-04-25", kPos, "qt");
    ASSERT_TRUE(rows_sys.is_ok() && rows_qt.is_ok());
    EXPECT_EQ(rows_sys.value().size(), 1u);
    EXPECT_EQ(rows_qt.value().size(), 2u);
}

// (a) + (b) + (d), live_results and equity_curve: one row per book per key; the desk book's
// delete, update and store leave the system row byte-identical; the reads filter by book.
TEST_F(QtBooksDb, LiveResultsAndEquityBooksNeverTouchEachOther) {
    const auto d = at(2026, 4, 24);
    const auto next = at(2026, 4, 27);
    const std::unordered_map<std::string, double> sys_m{{"current_portfolio_value", 500100.0},
                                                        {"total_pnl", 100.0}};
    const std::unordered_map<std::string, double> qt_m{{"current_portfolio_value", 499900.0},
                                                       {"total_pnl", -100.0}};
    ASSERT_TRUE(db_->store_live_results_complete(kSid, d, sys_m, {}, nlohmann::json(), kPid, kLive).is_ok());
    ASSERT_TRUE(db_->store_trading_equity_curve(kSid, d, 500100.0, kPid, kCurve).is_ok());
    ASSERT_EQ(count(kLive, "system"), 1) << "the default call did not write system";
    const auto sys_live = fp(kLive, "system");
    const auto sys_curve = fp(kCurve, "system");

    for (const char* book : {"qt_proposal", "qt"}) {
        auto r = db_->store_live_results_complete(kSid, d, qt_m, {}, nlohmann::json(), kPid, kLive,
                                                  nlohmann::json{{"binding_term", "R"}}, book);
        ASSERT_TRUE(r.is_ok()) << book << ": " << r.error()->what();
        ASSERT_TRUE(db_->store_trading_equity_curve(kSid, d, 499900.0, kPid, kCurve, book).is_ok());
        ASSERT_TRUE(db_->update_live_results(kSid, d, {{"total_pnl", -50.0}}, kPid, kLive, book).is_ok());
        ASSERT_TRUE(db_->update_live_equity_curve(kSid, d, 499950.0, kPid, kCurve, book).is_ok());
        EXPECT_EQ(fp(kLive, "system"), sys_live) << book;
        EXPECT_EQ(fp(kCurve, "system"), sys_curve) << book;
    }

    auto agg_sys = db_->get_previous_live_aggregates(kSid, kPid, next, kLive);
    auto agg_qt = db_->get_previous_live_aggregates(kSid, kPid, next, kLive, "qt");
    ASSERT_TRUE(agg_sys.is_ok() && agg_qt.is_ok());
    EXPECT_DOUBLE_EQ(std::get<0>(agg_sys.value()), 500100.0);
    EXPECT_DOUBLE_EQ(std::get<0>(agg_qt.value()), 499900.0);
    EXPECT_DOUBLE_EQ(std::get<1>(agg_qt.value()), -50.0);

    const auto qt_live = fp(kLive, "qt");
    const auto qt_curve = fp(kCurve, "qt");
    ASSERT_TRUE(db_->delete_live_results(kSid, d, kPid, kLive, "qt_proposal").is_ok());
    ASSERT_TRUE(db_->delete_live_equity_curve(kSid, d, kPid, kCurve, "qt_proposal").is_ok());
    EXPECT_EQ(count(kLive, "qt_proposal"), 0);
    EXPECT_EQ(count(kCurve, "qt_proposal"), 0);
    EXPECT_EQ(fp(kLive, "system"), sys_live);
    EXPECT_EQ(fp(kCurve, "system"), sys_curve);
    EXPECT_EQ(fp(kLive, "qt"), qt_live);

    // The default delete and update are system's only.
    ASSERT_TRUE(db_->update_live_results(kSid, d, {{"total_pnl", 1.0}}, kPid, kLive).is_ok());
    ASSERT_TRUE(db_->delete_live_results(kSid, d, kPid, kLive).is_ok());
    ASSERT_TRUE(db_->delete_live_equity_curve(kSid, d, kPid, kCurve).is_ok());
    EXPECT_EQ(count(kLive, "system"), 0);
    EXPECT_EQ(count(kCurve, "system"), 0);
    EXPECT_EQ(fp(kLive, "qt"), qt_live) << "the default live_results delete reached into qt";
    EXPECT_EQ(fp(kCurve, "qt"), qt_curve) << "the default equity delete reached into qt";
}

// (c): a book outside the three is refused by every entry point, and nothing is written or
// deleted.
TEST_F(QtBooksDb, InvalidBookIsRefusedEverywhere) {
    const auto d = at(2026, 4, 24);
    ASSERT_TRUE(db_->store_positions({pos("ES.v.0", 3, 1, d)}, kSid, kSleeve, kPid, kPos).is_ok());
    ASSERT_TRUE(db_->store_executions({fill("ES.v.0", 1, d)}, kSid, kSleeve, kPid, kExecs).is_ok());
    ASSERT_TRUE(db_->store_live_results_complete(kSid, d, {{"total_pnl", 1.0}}, {}, nlohmann::json(), kPid, kLive).is_ok());
    ASSERT_TRUE(db_->store_trading_equity_curve(kSid, d, 500000.0, kPid, kCurve).is_ok());
    const auto before = fp(kPos, "system") + fp(kExecs, "system") + fp(kLive, "system") + fp(kCurve, "system");

    for (const std::string bad : {"", "desk", "QT", "System", "qt ", "qt'; --"}) {
        SCOPED_TRACE("book='" + bad + "'");
        EXPECT_TRUE(db_->store_positions({pos("ES.v.0", 1, 1, d)}, kSid, kSleeve, kPid, kPos, bad).is_error());
        EXPECT_TRUE(db_->load_positions_by_date(kSid, kSleeve, kPid, d, kPos, bad).is_error());
        EXPECT_TRUE(db_->store_executions({fill("ES.v.0", 1, d)}, kSid, kSleeve, kPid, kExecs, bad).is_error());
        EXPECT_TRUE(db_->delete_stale_executions({"DAILY_ES.v.0_20260424"}, d, kSleeve, kPid, kExecs, bad).is_error());
        EXPECT_TRUE(db_->delete_roll_executions(d, kSleeve, kPid, kExecs, bad).is_error());
        EXPECT_TRUE(db_->replace_roll_day_executions({}, kSid, kPid, d, kExecs, bad).is_error());
        EXPECT_TRUE(db_->get_stored_roll_contracts(kSid, kPid, d, kExecs, bad).is_error());
        EXPECT_TRUE(db_->get_stored_realised_rows(kSid, kPid, "2026-04-23", "2026-04-25", kPos, bad).is_error());
        EXPECT_TRUE(db_->get_previous_total_roll_costs(kSid, kPid, d, kLive, kExecs, bad).is_error());
        EXPECT_TRUE(db_->store_live_results_complete(kSid, d, {{"total_pnl", 2.0}}, {}, nlohmann::json(), kPid, kLive, nlohmann::json(), bad).is_error());
        EXPECT_TRUE(db_->delete_live_results(kSid, d, kPid, kLive, bad).is_error());
        EXPECT_TRUE(db_->update_live_results(kSid, d, {{"total_pnl", 3.0}}, kPid, kLive, bad).is_error());
        EXPECT_TRUE(db_->get_previous_live_aggregates(kSid, kPid, d, kLive, bad).is_error());
        EXPECT_TRUE(db_->store_trading_equity_curve(kSid, d, 1.0e6, kPid, kCurve, bad).is_error());
        EXPECT_TRUE(db_->delete_live_equity_curve(kSid, d, kPid, kCurve, bad).is_error());
        EXPECT_TRUE(db_->update_live_equity_curve(kSid, d, 1.0e6, kPid, kCurve, bad).is_error());
    }
    EXPECT_EQ(fp(kPos, "system") + fp(kExecs, "system") + fp(kLive, "system") + fp(kCurve, "system"), before);
    EXPECT_EQ(scalar("SELECT count(*) FROM trading.positions WHERE strategy_id = 'QTBOOKS_PROBE_ID'"), "1");
}

// LiveResultsManager: with no book set it writes and clears system, as before; set to qt, a
// whole save_all_results for the same date (its stale-data delete included) writes qt rows in
// every table and leaves system byte-identical; an invalid book is refused.
TEST_F(QtBooksDb, LiveResultsManagerWritesItsBookOnly) {
    const auto d = at(2026, 4, 24);
    auto save = [&](const std::string& book, double qty, double equity) {
        LiveResultsManager mgr(db_, true, kSid, kPid, kSleeve);
        if (!book.empty()) {
            auto set = mgr.set_book(book);
            if (set.is_error()) return set;
        }
        mgr.set_positions({pos("ES.v.0", qty, 5120.25, d)});
        mgr.set_executions({fill("ES.v.0", qty, d)});
        mgr.set_metrics({{"current_portfolio_value", equity}, {"total_pnl", equity - 500000.0}});
        mgr.set_equity(equity);
        return mgr.save_all_results("run", d);
    };
    ASSERT_TRUE(save("", 3, 500100.0).is_ok());
    for (const char* t : {kPos, kExecs, kLive, kCurve}) {
        ASSERT_EQ(count(t, "system"), 1) << t << ": the default manager did not write system";
    }
    const auto sys = fp(kPos, "system") + fp(kExecs, "system") + fp(kLive, "system") + fp(kCurve, "system");

    ASSERT_TRUE(save("qt", 5, 499900.0).is_ok());
    ASSERT_TRUE(save("qt", 6, 499800.0).is_ok());  // a re-run: its own stale-data delete
    for (const char* t : {kPos, kExecs, kLive, kCurve}) {
        EXPECT_EQ(count(t, "qt"), 1) << t;
    }
    EXPECT_EQ(fp(kPos, "system") + fp(kExecs, "system") + fp(kLive, "system") + fp(kCurve, "system"), sys)
        << "a qt save moved system rows";

    const auto qt = fp(kPos, "qt") + fp(kExecs, "qt") + fp(kLive, "qt") + fp(kCurve, "qt");
    ASSERT_TRUE(save("", 4, 500200.0).is_ok());  // a system re-run
    EXPECT_EQ(fp(kPos, "qt") + fp(kExecs, "qt") + fp(kLive, "qt") + fp(kCurve, "qt"), qt)
        << "a system save moved qt rows";

    // The invalid-equity fallback reads the last good equity of its own book: qt's 04-23 row,
    // not system's later 04-24 row a book-blind read would take.
    ASSERT_TRUE(db_->store_trading_equity_curve(kSid, at(2026, 4, 23), 499700.0, kPid, kCurve, "qt").is_ok());
    ASSERT_TRUE(save("qt", 6, 0.0).is_ok());
    EXPECT_EQ(scalar("SELECT equity::text FROM trading.equity_curve WHERE strategy_id = "
                     "'QTBOOKS_PROBE_ID' AND portfolio_type = 'qt' AND timestamp = "
                     "'2026-04-24 21:00:00+00'"),
              "499700");

    LiveResultsManager bad(db_, true, kSid, kPid, kSleeve);
    EXPECT_TRUE(bad.set_book("desk").is_error());
    EXPECT_EQ(bad.get_book(), "system");
}
