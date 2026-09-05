// tests/data/test_current_holding_start_bound_db.cpp
//
// BA-19 -- `get_current_holding_start_dates` must not see rows the run cannot legitimately
// see.
//
// `trading.positions` is not append-only in practice. An interrupted windowed reset, or a
// replay abandoned partway through a chain, leaves rows dated AFTER the date being replayed.
// Unbounded, such a row moves the class-2 rename era in two different directions, and neither
// shows up in the run's output:
//
//   a stray NON-ZERO row  becomes the min(date) of "the current holding" when it is the only
//                         row after the last flat one -- so a symbol the book closed comes
//                         back with a holding that starts in the future, and the era test
//                         runs against it.
//   a stray FLAT row      becomes the break, pushing it past every real row, so the symbol
//                         has no non-zero row after it, drops out of the map entirely, and
//                         its rename is silently skipped.
//
// Direction of error matters here: the shipped rule already errs LATE on purpose (a skipped
// rename is retried next run; an early one re-keys a live holding permanently). A stray row
// defeats that choice in both directions at once, which is why the bound covers BOTH halves
// of the query.
//
// This test WRITES, unlike its read-only sibling test_current_holding_start_db.cpp: the
// failure needs a history that does not exist in the book. It is scoped to a synthetic
// portfolio_id and purged in SetUp AND TearDown, so a crashed run cannot leave residue --
// the same contract tests/live/corp_actions/test_broker_frame_db.cpp works under.

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
constexpr const char* kPortfolioId = "BA19_HOLDING_BOUND_TEST";  // synthetic, never a real book

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

class CurrentHoldingStartBoundDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const bool require_db = [] {
            const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
            return v && std::string(v) == "1";
        }();
        conn_string_ = discover_connection_string();
        if (conn_string_.empty()) {
            if (require_db) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set";
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        }
        db_ = std::make_shared<PostgresDatabase>(conn_string_);
        auto connected = db_->connect();
        if (connected.is_error() || !db_->is_connected()) {
            if (require_db) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable";
            GTEST_SKIP() << "database unreachable; this rule lives in SQL";
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
        pqxx::connection c(conn_string_);
        pqxx::work w(c);
        w.exec("DELETE FROM trading.positions WHERE portfolio_id = $1",
               pqxx::params{std::string(kPortfolioId)});
        w.commit();
    }

    // One position row. `quantity == 0` is a close (E2-F19 writes exactly one such row).
    void insert_row(const std::string& symbol, const std::string& date, double quantity) {
        pqxx::connection c(conn_string_);
        pqxx::work w(c);
        w.exec(
            "INSERT INTO trading.positions (symbol, quantity, average_price, "
            "daily_unrealized_pnl, daily_realized_pnl, last_update, updated_at, strategy_id, "
            "strategy_name, date, portfolio_id, portfolio_type) "
            "VALUES ($1, $2, 10.0, 0, 0, $3::date, $3::date, $4, $5, $3::date, $6, 'system')",
            pqxx::params{symbol, quantity, date, std::string(kStrategyId),
                         std::string(kStrategyName), std::string(kPortfolioId)});
        w.commit();
    }

    std::unordered_map<std::string, std::string> starts(const std::string& bound) {
        auto r = db_->get_current_holding_start_dates(kStrategyId, kStrategyName, kPortfolioId,
                                                      {"ZZTEST"}, bound);
        EXPECT_TRUE(r.is_ok()) << (r.is_ok() ? "" : r.error()->what());
        return r.is_ok() ? r.value() : std::unordered_map<std::string, std::string>{};
    }

    std::string conn_string_;
    std::shared_ptr<PostgresDatabase> db_;
};

TEST_F(CurrentHoldingStartBoundDbTest, AFutureDatedNonZeroRowDoesNotBecomeTheHoldingStart) {
    // Held 04-12..04-14, closed 04-15. Then a stray non-zero row on 05-01 -- the shape an
    // abandoned replay leaves behind.
    insert_row("ZZTEST", "2026-04-12", 100.0);
    insert_row("ZZTEST", "2026-04-14", 100.0);
    insert_row("ZZTEST", "2026-04-15", 0.0);
    insert_row("ZZTEST", "2026-05-01", 100.0);

    // Bounded at the run's T-1: the book is flat as at 04-20, so there is NO current holding
    // and the symbol is absent. Class 2 skips it, which is the safe direction.
    const auto bounded = starts("2026-04-20");
    EXPECT_EQ(bounded.count("ZZTEST"), 0u)
        << "a row dated after the run's previous_date was taken as the start of the holding "
           "the run is about to apply renames to; it reported "
        << (bounded.count("ZZTEST") ? bounded.at("ZZTEST") : std::string());

    // Unbounded is the defect, pinned so the fix cannot be quietly reverted: the 05-01 row is
    // the only non-zero row after the 04-15 break, so it becomes the holding start -- a date
    // in the run's future.
    const auto unbounded = starts("");
    ASSERT_EQ(unbounded.count("ZZTEST"), 1u);
    EXPECT_EQ(unbounded.at("ZZTEST"), "2026-05-01");
}

TEST_F(CurrentHoldingStartBoundDbTest, AFutureDatedFlatRowDoesNotHideALiveHolding) {
    // Held from 04-12 and still held. Then a stray FLAT row on 05-01.
    insert_row("ZZTEST", "2026-04-12", 100.0);
    insert_row("ZZTEST", "2026-04-14", 100.0);
    insert_row("ZZTEST", "2026-04-20", 100.0);
    insert_row("ZZTEST", "2026-05-01", 0.0);

    // Bounded: the holding began 04-12 and the rename era is tested from there.
    const auto bounded = starts("2026-04-20");
    ASSERT_EQ(bounded.count("ZZTEST"), 1u)
        << "a flat row dated after the run's previous_date became the break, so a live "
           "holding dropped out of the map and its rename would be skipped in silence";
    EXPECT_EQ(bounded.at("ZZTEST"), "2026-04-12");

    // Unbounded is the defect: the future flat row is the newest, nothing follows it, and the
    // symbol vanishes.
    EXPECT_EQ(starts("").count("ZZTEST"), 0u);
}

TEST_F(CurrentHoldingStartBoundDbTest, AnEmptyBoundIsThePreviousBehaviourExactly) {
    // No stray rows: bounded and unbounded must agree, and both must give the earliest
    // non-zero row after the last flat one.
    insert_row("ZZTEST", "2026-04-06", 50.0);
    insert_row("ZZTEST", "2026-04-08", 0.0);   // previous holding closed
    insert_row("ZZTEST", "2026-04-12", 100.0); // current holding starts here
    insert_row("ZZTEST", "2026-04-14", 100.0);

    const auto unbounded = starts("");
    const auto bounded = starts("2026-04-20");
    ASSERT_EQ(unbounded.count("ZZTEST"), 1u);
    EXPECT_EQ(unbounded.at("ZZTEST"), "2026-04-12");
    EXPECT_EQ(bounded, unbounded);

    // And the bound is inclusive on its own date.
    const auto on_the_day = starts("2026-04-12");
    ASSERT_EQ(on_the_day.count("ZZTEST"), 1u);
    EXPECT_EQ(on_the_day.at("ZZTEST"), "2026-04-12");
}

TEST_F(CurrentHoldingStartBoundDbTest, TheBoundNeverInventsAHoldingThatEndedBeforeIt) {
    // Closed on 04-08 and never re-opened: absent under every bound. The bound must not turn
    // a previous holding into a current one by hiding the flat row that closed it.
    insert_row("ZZTEST", "2026-04-06", 50.0);
    insert_row("ZZTEST", "2026-04-08", 0.0);

    for (const auto* bound : {"", "2026-04-20", "2026-04-08"}) {
        EXPECT_EQ(starts(bound).count("ZZTEST"), 0u) << "bound=" << bound;
    }
    // Bounding BEFORE the close does report the old holding -- correct, and the reason the
    // runner passes its own previous_date rather than an arbitrary date: as at 04-07 the
    // book really did hold it.
    const auto before_close = starts("2026-04-07");
    ASSERT_EQ(before_close.count("ZZTEST"), 1u);
    EXPECT_EQ(before_close.at("ZZTEST"), "2026-04-06");
}
