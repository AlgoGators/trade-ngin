// E2-F6: trading.get_trading_days() must exist in BOTH forms, and the scoped form must scope.
//
// Two distinct things are pinned here, and the first is the one that bites hardest.
//
// 1. THE FUNCTION MUST EXIST. Both overloads were created by hand against the live server and
//    lived nowhere in the repository until migrations/004. That matters because the failure
//    mode when it is absent is SILENT: the runners initialise `trading_days_count = 1` and
//    only overwrite it if the query succeeds, so a missing function leaves the count at 1,
//    nothing throws, the run exits 0, and five stored columns are corrupted --
//    total_annualized_return, sharpe_ratio, sortino_ratio, total_days, and win_rate (which
//    becomes winning_days * 100, roughly 9000%). A DB rebuilt without 004 fails exactly this
//    way, and this test is what catches it.
//
// 2. THE SCOPED FORM MUST SCOPE. The 2-arg form keys on strategy_id alone with
//    `ORDER BY live_start_date LIMIT 1` and no portfolio predicate, so it takes the earliest
//    row across ALL portfolios. LIVE_EQUITY_MEAN_REVERSION unscoped anchors to a stale
//    2025-08-15 BASE_PORTFOLIO row and reports 356 days against a 10-day series -- a 36x
//    understatement of annualized return (E2/B4).
//
// Read-only: calls the function, touches no table.
//
// Reachability gate matches tests/data/test_db_transaction_atomicity.cpp:
//   * TRADE_NGIN_REQUIRE_DB=1 -- unreachable database FAILS rather than skips.
//   * unset -- skip (local dev without a server).

#include <gtest/gtest.h>
#include <pqxx/pqxx>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace {

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

class GetTradingDaysScopeTest : public ::testing::Test {
protected:
    void SetUp() override {
        const bool require_db = [] {
            const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
            return v && std::string(v) == "1";
        }();
        conn_ = discover_connection_string();
        if (conn_.empty()) {
            if (require_db) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is "
                                       "not set; E2-F6 scoping goes unverified";
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        }
        try {
            c_ = std::make_unique<pqxx::connection>(conn_);
        } catch (const std::exception& e) {
            if (require_db) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable: "
                                   << e.what();
            GTEST_SKIP() << "database unreachable";
        }
    }

    int overload_count() {
        pqxx::work txn(*c_);
        return txn
            .exec("SELECT count(*) FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace "
                  "WHERE n.nspname='trading' AND p.proname='get_trading_days'")[0][0]
            .as<int>();
    }

    std::string conn_;
    std::unique_ptr<pqxx::connection> c_;
};

// The migrations/004 guarantee. If this fails on a fresh environment, 004 was not applied and
// every futures annualization figure is about to be silently wrong.
TEST_F(GetTradingDaysScopeTest, BothOverloadsExist) {
    EXPECT_EQ(overload_count(), 2)
        << "trading.get_trading_days does not have both a 2-arg and a 3-arg overload. The "
           "runners will leave trading_days_count at 1 and exit 0 with corrupted "
           "total_annualized_return / sharpe_ratio / sortino_ratio / total_days / win_rate. "
           "Apply migrations/004_get_trading_days_portfolio_scope.sql.";
}

// The scoped form must actually consult portfolio_id, not merely accept it.
TEST_F(GetTradingDaysScopeTest, ScopedFormDistinguishesPortfolios) {
    pqxx::work txn(*c_);
    // LIVE_EQUITY_MEAN_REVERSION has a real EQUITY_MR_PORTFOLIO row and a stale legacy
    // BASE_PORTFOLIO row, so the two scopes must disagree. If they agree, the predicate is
    // being ignored.
    const int eq = txn.exec("SELECT trading.get_trading_days('LIVE_EQUITY_MEAN_REVERSION', "
                            "DATE '2026-05-03', 'EQUITY_MR_PORTFOLIO')")[0][0]
                       .as<int>();
    const int base = txn.exec("SELECT trading.get_trading_days('LIVE_EQUITY_MEAN_REVERSION', "
                              "DATE '2026-05-03', 'BASE_PORTFOLIO')")[0][0]
                         .as<int>();

    EXPECT_NE(eq, base)
        << "The 3-arg form returned the same answer for two different portfolios, so it is "
           "not scoping. Unscoped, this strategy anchors to a stale 2025-08-15 BASE_PORTFOLIO "
           "row and reports 356 days against a 10-day series (E2/B4).";
}

// The repoint's falsifiable prediction: futures numbers must NOT move. Both
// LIVE_TREND_FOLLOWING metadata rows carry live_start_date = 2025-10-05, so scoped and
// unscoped agree -- today. If this ever fails, that coincidence has ended and the futures
// annualization has silently changed.
TEST_F(GetTradingDaysScopeTest, FuturesScopedAndUnscopedStillAgree) {
    pqxx::work txn(*c_);
    for (const char* date : {"2026-05-03", "2026-09-01", "2027-01-01"}) {
        const std::string d = std::string("DATE '") + date + "'";

        const int cons_old =
            txn.exec("SELECT trading.get_trading_days('LIVE_TREND_FOLLOWING', " + d + ")")[0][0]
                .as<int>();
        const int cons_new = txn.exec("SELECT trading.get_trading_days('LIVE_TREND_FOLLOWING', " +
                                      d + ", 'CONSERVATIVE_PORTFOLIO')")[0][0]
                                 .as<int>();
        EXPECT_EQ(cons_old, cons_new)
            << "Conservative futures annualization moved at " << date
            << " when the runners were repointed to the scoped form. The two "
               "LIVE_TREND_FOLLOWING metadata rows no longer share live_start_date, so the "
               "no-movement assumption behind E2-F6 has ended -- investigate before shipping.";

        const int base_old = txn.exec("SELECT trading.get_trading_days('LIVE_TREND_FOLLOWING_"
                                      "TREND_FOLLOWING_FAST', " +
                                      d + ")")[0][0]
                                 .as<int>();
        const int base_new = txn.exec("SELECT trading.get_trading_days('LIVE_TREND_FOLLOWING_"
                                      "TREND_FOLLOWING_FAST', " +
                                      d + ", 'BASE_PORTFOLIO')")[0][0]
                                 .as<int>();
        EXPECT_EQ(base_old, base_new) << "Base futures annualization moved at " << date << ".";
    }
}
