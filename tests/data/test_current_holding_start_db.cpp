// tests/data/test_current_holding_start_db.cpp
//
// BA-2 / C-3 D1 -- PostgresDatabase::get_current_holding_start_dates.
//
// The class-2 rename era must be tested against the start of the CURRENT holding.
// `get_position_inception_dates` answers min(date) over all history and fails WIDE on
// purpose (class 1's price window would rather over-fetch than under-fetch); asked the
// class-2 question it returns a date from a PREVIOUS holding of a reused ticker, which
// satisfies the era test for an alias belonging to the previous issuer and re-keys a
// live position onto a symbol with no bars.
//
// The rule is expressed in SQL, so it needs the real table to be pinned. Read-only:
// nothing here writes. The assertions are data-independent -- they compare the shipped
// query against an INDEPENDENT formulation of the same rule and against the lifetime
// query, so they hold whatever the book happens to contain on the day they run.
//
// Reachability gate matches tests/live/corp_actions/test_corp_action_query_bounds_db.cpp:
//   * TRADE_NGIN_REQUIRE_DB=1 -- unreachable database FAILS rather than skips.
//   * unset -- skip (local dev without a server).

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {

constexpr const char* kStrategyId = "LIVE_EQUITY_MEAN_REVERSION";
constexpr const char* kStrategyName = "EQUITY_MEAN_REVERSION";

std::string discover_connection_string() {
    // Database-backed tests connect ONLY through TRADE_NGIN_TEST_DSN. The previous
    // behaviour walked up from the working directory looking for config/defaults.json,
    // which pointed a test run inside any worktree of this checkout at the live
    // database. Tests must never discover production credentials by accident.
    const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
    if (dsn && *dsn) {
        return std::string(dsn);
    }
    return {};
}

}  // namespace

class CurrentHoldingStartDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const bool require_db = [] {
            const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
            return v && std::string(v) == "1";
        }();

        conn_string_ = discover_connection_string();
        if (conn_string_.empty()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set, "
                          "so the class-2 era query goes unverified";
            }
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set; no database to exercise";
        }
        db_ = std::make_shared<PostgresDatabase>(conn_string_);
        auto connected = db_->connect();
        if (connected.is_error() || !db_->is_connected()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable, so the "
                          "class-2 era query goes unverified";
            }
            GTEST_SKIP() << "database unreachable; this rule lives in SQL";
        }

        pqxx::connection c(conn_string_);
        pqxx::work w(c);
        auto rows = w.exec(
            "SELECT DISTINCT portfolio_id, symbol FROM trading.positions "
            "WHERE strategy_id = $1 AND strategy_name = $2",
            pqxx::params{kStrategyId, kStrategyName});
        for (const auto& row : rows) {
            portfolio_id_ = row["portfolio_id"].c_str();
            symbols_.push_back(row["symbol"].c_str());
        }
        w.commit();
        if (symbols_.empty()) {
            GTEST_SKIP() << "no equity position history to derive holding starts from";
        }
    }

    void TearDown() override {
        if (db_ && db_->is_connected()) db_->disconnect();
    }

    std::string conn_string_;
    std::string portfolio_id_;
    std::vector<std::string> symbols_;
    std::shared_ptr<PostgresDatabase> db_;
};

// The shipped query against an INDEPENDENT formulation of the same rule: the earliest
// non-zero row AFTER the last flat row, found with a partition-wide window function
// instead of a correlated max(). Two different SQL shapes for one definition; if they
// disagree the shipped one is wrong, whatever the data looks like.
//
// Note what the rule excludes: a symbol whose newest row is flat has no CURRENT holding
// and is absent from both answers. That is correct -- apply_renames only ever asks about
// positions it holds -- and it is the case that separates this from a running max()
// (which would answer with a previous holding start for a symbol that is now closed).
TEST_F(CurrentHoldingStartDbTest, HoldingStartMatchesAnIndependentFormulationOfTheRule) {
    auto shipped = db_->get_current_holding_start_dates(kStrategyId, kStrategyName,
                                                        portfolio_id_, symbols_);
    ASSERT_TRUE(shipped.is_ok()) << shipped.error()->what();

    pqxx::connection c(conn_string_);
    pqxx::work w(c);
    auto rows = w.exec(
        "WITH marked AS ("
        "  SELECT symbol, date, quantity,"
        "         max(CASE WHEN quantity = 0 THEN date END) OVER ("
        "             PARTITION BY symbol) AS last_flat"
        "    FROM trading.positions"
        "   WHERE strategy_id = $1 AND strategy_name = $2 AND portfolio_id = $3"
        "     AND symbol = ANY($4))"
        " SELECT symbol, min(date)::text AS holding_start FROM marked"
        "  WHERE quantity <> 0 AND (last_flat IS NULL OR date > last_flat)"
        "  GROUP BY symbol",
        pqxx::params{kStrategyId, kStrategyName, portfolio_id_, symbols_});
    std::unordered_map<std::string, std::string> expected;
    for (const auto& row : rows) expected.emplace(row["symbol"].c_str(),
                                                  row["holding_start"].c_str());
    w.commit();

    EXPECT_EQ(shipped.value().size(), expected.size());
    for (const auto& [symbol, start] : expected) {
        auto it = shipped.value().find(symbol);
        ASSERT_NE(it, shipped.value().end()) << "shipped query lost " << symbol;
        EXPECT_EQ(it->second, start) << "shipped query disagrees on " << symbol;
    }
}

// The direction of error that matters. A holding start is never EARLIER than the
// lifetime inception, so class 2 can only ever be handed a date at or after the one
// class 1 uses -- and "later" is the safe direction: a skipped rename is retried next
// run, a wrongly applied one is silent, permanent corruption.
TEST_F(CurrentHoldingStartDbTest, HoldingStartIsNeverEarlierThanTheLifetimeInception) {
    auto holding = db_->get_current_holding_start_dates(kStrategyId, kStrategyName,
                                                        portfolio_id_, symbols_);
    ASSERT_TRUE(holding.is_ok()) << holding.error()->what();
    auto lifetime = db_->get_position_inception_dates(kStrategyId, kStrategyName,
                                                      portfolio_id_, symbols_);
    ASSERT_TRUE(lifetime.is_ok()) << lifetime.error()->what();

    for (const auto& [symbol, start] : holding.value()) {
        auto it = lifetime.value().find(symbol);
        ASSERT_NE(it, lifetime.value().end())
            << symbol << " has a current holding but no lifetime inception, which cannot be";
        EXPECT_GE(start, it->second)
            << symbol << ": holding start " << start << " precedes lifetime inception "
            << it->second;
    }
}

// The two questions are not the same question. Every symbol whose history contains a
// flat row after its first non-zero row was closed and re-opened, and for exactly those
// the two answers MUST differ -- that difference is the whole point of BA-2. Where the
// book contains no such symbol the test says so rather than passing silently.
TEST_F(CurrentHoldingStartDbTest, ReopenedTickersGetADifferentDateFromTheLifetimeQuery) {
    pqxx::connection c(conn_string_);
    pqxx::work w(c);
    auto rows = w.exec(
        "SELECT symbol FROM trading.positions"
        " WHERE strategy_id = $1 AND strategy_name = $2 AND portfolio_id = $3"
        "   AND quantity = 0"
        "   AND date > (SELECT min(i.date) FROM trading.positions i"
        "                WHERE i.strategy_id = trading.positions.strategy_id"
        "                  AND i.strategy_name = trading.positions.strategy_name"
        "                  AND i.portfolio_id = trading.positions.portfolio_id"
        "                  AND i.symbol = trading.positions.symbol AND i.quantity <> 0)"
        " GROUP BY symbol",
        pqxx::params{kStrategyId, kStrategyName, portfolio_id_});
    std::vector<std::string> reopened;
    for (const auto& row : rows) reopened.push_back(row["symbol"].c_str());
    w.commit();

    auto holding = db_->get_current_holding_start_dates(kStrategyId, kStrategyName,
                                                        portfolio_id_, symbols_);
    ASSERT_TRUE(holding.is_ok()) << holding.error()->what();
    auto lifetime = db_->get_position_inception_dates(kStrategyId, kStrategyName,
                                                      portfolio_id_, symbols_);
    ASSERT_TRUE(lifetime.is_ok()) << lifetime.error()->what();

    int compared = 0;
    for (const auto& symbol : reopened) {
        auto h = holding.value().find(symbol);
        if (h == holding.value().end()) continue;  // still flat: no current holding at all
        auto l = lifetime.value().find(symbol);
        ASSERT_NE(l, lifetime.value().end());
        EXPECT_GT(h->second, l->second)
            << symbol << " was closed and re-opened, so the class-2 era date must be the "
                         "re-open, not the original purchase";
        ++compared;
    }
    if (compared == 0) {
        GTEST_SKIP() << "no currently-held symbol in this book has been closed and "
                        "re-opened; the two queries cannot be shown to differ on this "
                        "data (the pure tests carry the behaviour)";
    }
}
