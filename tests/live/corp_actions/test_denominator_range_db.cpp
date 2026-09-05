// tests/live/corp_actions/test_denominator_range_db.cpp
//
// FIX-2 against a real server: the collapse was in the ARGUMENTS, so only running both
// argument shapes against real data shows the difference. The pure tests in
// test_denominator_frame.cpp pin the range the runner computes; these pin what that
// range actually fetches.
//
// Read-only. It queries closes for a long-history symbol and writes nothing.
//
// Reachability gate matches tests/data/test_db_transaction_atomicity.cpp:
//   * TRADE_NGIN_REQUIRE_DB=1 -- unreachable database FAILS rather than skips.
//   * unset -- skip (local dev without a server).

#include <gtest/gtest.h>
#include <pqxx/pqxx>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/live/corp_action_window.hpp"

using namespace trade_ngin;

namespace {

// Held since 2000-01-03 in equities_data.ohlcv_1d, so it is unambiguously "deep"
// against any bulk load we run.
constexpr const char* kDeepSymbol = "KO";

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

class DenominatorRangeDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const bool require_db = [] {
            const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
            return v && std::string(v) == "1";
        }();

        const std::string conn = discover_connection_string();
        if (conn.empty()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set, "
                          "so the top-up range collapse goes unverified";
            }
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set; no database to exercise";
        }
        db_ = std::make_shared<PostgresDatabase>(conn);
        auto connected = db_->connect();
        if (connected.is_error() || !db_->is_connected()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable, so the "
                          "top-up range collapse goes unverified";
            }
            GTEST_SKIP() << "database unreachable; the range collapse needs real bars";
        }
    }

    void TearDown() override {
        if (db_ && db_->is_connected()) db_->disconnect();
    }

    std::shared_ptr<PostgresDatabase> db_;
};

// FIX-3, measured rather than hypothesised: the frame gap is present in the data we
// will actually run E2 against.
TEST_F(DenominatorRangeDbTest, AdjustedAndRawClosesDivergeOnRealStackedEvents) {
    // Find a dividend that has a split after it -- the stacked shape the old code got
    // wrong. Located by query rather than hardcoded so a data refresh cannot rot it.
    pqxx::connection c(discover_connection_string());
    pqxx::work w(c);
    auto row = w.exec(
        "SELECT a.symbol, a.time::date::text, a.close, a.adjusted_close, a.div_cash "
        "FROM equities_data.ohlcv_1d a "
        "WHERE a.div_cash > 0 AND a.close > 0 AND a.adjusted_close > 0 "
        // The premise is a row whose adjusted close already DIFFERS from its raw close
        // (a later split or dividend has restated it). Without this predicate the
        // time-descending locator drifts with the calendar and, as of 2026-09-04, picked
        // APH 2026-06-23 where adjusted_close == close, failing the test on a row that
        // never carried the stacked shape (D-4-FIX F-2).
        "  AND a.adjusted_close <> a.close "
        // Bounded so the scan stays quick; stacked pairs are common in recent history.
        "  AND a.time > now() - interval '2 years' "
        "  AND EXISTS (SELECT 1 FROM equities_data.ohlcv_1d b "
        "              WHERE b.symbol = a.symbol AND b.time > a.time AND b.split_factor <> 1) "
        "ORDER BY a.time DESC LIMIT 1");
    w.commit();

    if (row.empty()) {
        GTEST_SKIP() << "no dividend-then-split pair in the loaded history";
    }
    const std::string symbol = row[0][0].c_str();
    const std::string ex_date = row[0][1].c_str();
    const double raw_close = row[0][2].as<double>();
    const double adjusted_close = row[0][3].as<double>();
    const double div = row[0][4].as<double>();

    // get_historical_closes -- what the denominator now reads -- must return the RAW
    // close. If it ever starts returning the adjusted one, the fix is silently undone.
    auto fetched = db_->get_historical_closes({symbol}, ex_date, ex_date);
    ASSERT_TRUE(fetched.is_ok()) << fetched.error()->what();
    ASSERT_EQ(fetched.value().count(symbol), 1u);
    ASSERT_EQ(fetched.value().at(symbol).count(ex_date), 1u);
    EXPECT_NEAR(fetched.value().at(symbol).at(ex_date), raw_close, 1e-6)
        << "the denominator source stopped returning raw closes";

    // And the two frames really are far apart here: a later split deflates the adjusted
    // close, so a basis rescale computed from it overstates the dividend's effect by
    // exactly the split factor.
    ASSERT_GT(raw_close, 0.0);
    ASSERT_GT(adjusted_close, 0.0);
    const double raw_ratio = 1.0 + div / raw_close;
    const double adjusted_ratio = 1.0 + div / adjusted_close;
    EXPECT_GT(std::abs(adjusted_ratio - raw_ratio), 1e-4)
        << symbol << " on " << ex_date << ": raw " << raw_close << " vs adjusted "
        << adjusted_close << " -- if these agree the case is not actually stacked";
}

TEST_F(DenominatorRangeDbTest, UnifiedRangeFetchesTheWholeHistoryTheOldTopUpMissed) {
    const std::time_t today = parse_ymd_utc("2026-08-28");  // last bar date in the table
    std::unordered_map<std::string, std::string> inception = {{kDeepSymbol, "2005-04-01"}};

    auto w = derive_corp_action_window(today, /*min_days=*/14, /*bulk_days=*/730, inception);
    ASSERT_FALSE(w.deep_symbols.empty());
    ASSERT_EQ(w.start, w.deep_start) << "this equality is what made the old range degenerate";

    const std::vector<std::string> symbols = {kDeepSymbol};

    // What the code used to ask for: [deep_start, window.start]. Both ends are the same
    // date, and get_historical_closes is inclusive at both ends, so this is one day.
    auto collapsed = db_->get_historical_closes(symbols, format_ymd_utc(w.deep_start),
                                                format_ymd_utc(w.start));
    ASSERT_TRUE(collapsed.is_ok()) << collapsed.error()->what();
    size_t collapsed_dates = 0;
    for (const auto& [sym, by_date] : collapsed.value()) collapsed_dates += by_date.size();
    EXPECT_LE(collapsed_dates, 1u)
        << "the old top-up could never return more than the single boundary day";

    // What it asks for now.
    auto range = denominator_fetch_range(w, today);
    auto full = db_->get_historical_closes(symbols, range.start, range.end);
    ASSERT_TRUE(full.is_ok()) << full.error()->what();
    size_t full_dates = 0;
    for (const auto& [sym, by_date] : full.value()) full_dates += by_date.size();

    // ~21 years of trading days. Anything near the collapsed count means the seam is back.
    EXPECT_GT(full_dates, 4000u) << "the denominator read no longer spans the position's history";
    EXPECT_GT(full_dates, collapsed_dates * 100);
}

TEST_F(DenominatorRangeDbTest, EqualEndpointsReturnAtMostOneDay) {
    // The mechanism itself, stated directly against the server: get_historical_closes
    // with start == end is a single day, which is why an end argument of window.start
    // was fatal rather than merely narrow.
    auto one_day = db_->get_historical_closes({kDeepSymbol}, "2015-06-10", "2015-06-10");
    ASSERT_TRUE(one_day.is_ok()) << one_day.error()->what();
    for (const auto& [sym, by_date] : one_day.value()) {
        EXPECT_LE(by_date.size(), 1u);
    }
}

// ──────────────────────────────────────────────────────────────────────────
// BA-8 / C-1 D12 -- get_delisting_dates must not hand a 2026 run a 2008 row.
//
// delisting_date is keyed on the TICKER and the reader takes max() over the
// symbol's entire history, so a reused ticker inherits the dead company's
// delisting. The runner's bars-contradict guard needs a bar to contradict with:
// delisting_is_stale() is false when last_bar_date is empty, which is exactly
// the symbol that stopped printing. The floor closes it at the source.
//
// Read-only against real rows. HPC and MER are real delistings in this database
// (2008-11-24 and 2008-12-31), which is what makes the bound worth asserting.
// ──────────────────────────────────────────────────────────────────────────
class DelistingFloorDbTest : public DenominatorRangeDbTest {};

TEST_F(DelistingFloorDbTest, DateFloorExcludesADecadeOldDelisting) {
    const std::vector<std::string> tickers{"HPC", "MER"};

    // No floor: the historical rows come back, which is the pre-BA-8 behaviour
    // and the input the runner used to act on.
    auto unbounded = db_->get_delisting_dates(tickers);
    ASSERT_TRUE(unbounded.is_ok()) << unbounded.error()->what();
    ASSERT_EQ(unbounded.value().size(), 2u)
        << "both tickers must actually carry a delisting row, or this test proves nothing";
    EXPECT_EQ(unbounded.value().at("HPC"), "2008-11-24");
    EXPECT_EQ(unbounded.value().at("MER"), "2008-12-31");

    // With a floor a 2026 run cannot see them at all.
    auto bounded = db_->get_delisting_dates(tickers, "2026-06-01");
    ASSERT_TRUE(bounded.is_ok()) << bounded.error()->what();
    EXPECT_TRUE(bounded.value().empty())
        << "a delisting 18 years before the run date must not reach a live book";

    // The floor is INCLUSIVE, so a delisting exactly on the bound survives.
    auto on_bound = db_->get_delisting_dates({"HPC"}, "2008-11-24");
    ASSERT_TRUE(on_bound.is_ok()) << on_bound.error()->what();
    ASSERT_EQ(on_bound.value().size(), 1u) << "the bound is inclusive";
    EXPECT_EQ(on_bound.value().at("HPC"), "2008-11-24");

    // One day later excludes it, pinning the boundary rather than the direction only.
    auto past_bound = db_->get_delisting_dates({"HPC"}, "2008-11-25");
    ASSERT_TRUE(past_bound.is_ok()) << past_bound.error()->what();
    EXPECT_TRUE(past_bound.value().empty());
}

TEST_F(DelistingFloorDbTest, EmptyFloorPreservesTheUnboundedRead) {
    // The default argument must be behaviour-preserving: any caller that passes
    // nothing gets exactly what it got before.
    auto with_empty = db_->get_delisting_dates({"MER"}, "");
    auto defaulted = db_->get_delisting_dates({"MER"});
    ASSERT_TRUE(with_empty.is_ok() && defaulted.is_ok());
    EXPECT_EQ(with_empty.value(), defaulted.value());
    ASSERT_EQ(defaulted.value().size(), 1u);
    EXPECT_EQ(defaulted.value().at("MER"), "2008-12-31");
}
