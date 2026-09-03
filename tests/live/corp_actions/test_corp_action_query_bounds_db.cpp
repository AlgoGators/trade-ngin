// tests/live/corp_actions/test_corp_action_query_bounds_db.cpp
//
// E3 G6-4 / G6-5 / V4-1 -- the three things that can go wrong at the BOUNDARY of
// equities_data.corporate_action, all of which need the real table to show anything:
//
//   G6-4  `date` is TEXT. The reader used `date::date BETWEEN $3::date AND $4::date`,
//         which casts every row in the scan and dies on the first value that is not a
//         parseable date. ISO-8601 strings sort lexicographically, so a plain text
//         comparison selects exactly the same rows with no cast at all -- and it is
//         index-native on (ticker, date).
//   G6-5  Future-dated rows exist (SBDS 2027-07-18). The upper bound is what keeps them
//         out of a 2026 run; nothing pinned that it was actually applied.
//   V4-1  A class-1 row can exist in this table with NO corresponding move on the bar.
//         Class-1 effects are sourced PER BAR, so the applier is a silent no-op on such
//         a row: LEN's 2025-02-07 Millrose spinoff is in corporate_action with ratio 0.5
//         while the LEN bar that day carries split_factor 1 and div_cash 0.
//
// Read-only. Nothing here writes.
//
// Reachability gate matches tests/live/corp_actions/test_denominator_range_db.cpp:
//   * TRADE_NGIN_REQUIRE_DB=1 -- unreachable database FAILS rather than skips.
//   * unset -- skip (local dev without a server).

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/live/corporate_actions_classification.hpp"

using namespace trade_ngin;

namespace {

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

}  // namespace

class CorpActionQueryBoundsDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const bool require_db = [] {
            const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
            return v && std::string(v) == "1";
        }();

        conn_string_ = discover_connection_string();
        if (conn_string_.empty()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but config/defaults.json is not reachable, "
                          "so the corporate_action query bounds go unverified";
            }
            GTEST_SKIP() << "config/defaults.json not reachable; no database to exercise";
        }
        db_ = std::make_shared<PostgresDatabase>(conn_string_);
        auto connected = db_->connect();
        if (connected.is_error() || !db_->is_connected()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable, so the "
                          "corporate_action query bounds go unverified";
            }
            GTEST_SKIP() << "database unreachable; these bounds need the real table";
        }
    }

    void TearDown() override {
        if (db_ && db_->is_connected()) db_->disconnect();
    }

    std::string conn_string_;
    std::shared_ptr<PostgresDatabase> db_;
};

// ──────────────────────────────────────────────────────────────────────────
// G6-4
// ──────────────────────────────────────────────────────────────────────────

// The precondition the text comparison rests on. Every value in the column is a
// 10-character ISO date, so `>=` / `<=` on text orders identically to a date
// comparison. If a single row ever arrives in another shape this fails, and the
// text comparison silently starts selecting the wrong set -- which is exactly
// the day someone needs to know.
TEST_F(CorpActionQueryBoundsDbTest, EveryDateValueIsISO8601) {
    pqxx::connection c(conn_string_);
    pqxx::work w(c);
    const auto total =
        w.exec("SELECT count(*) FROM equities_data.corporate_action")[0][0].as<long>();
    const auto non_iso =
        w.exec("SELECT count(*) FROM equities_data.corporate_action "
               "WHERE date !~ '^[0-9]{4}-[0-9]{2}-[0-9]{2}$'")[0][0]
            .as<long>();
    w.commit();

    ASSERT_GT(total, 100000) << "the table is not loaded; this proves nothing about it";
    EXPECT_EQ(non_iso, 0)
        << "a non-ISO value in a TEXT date column breaks BOTH readings: the old "
           "`date::date` cast errors on it, and a lexicographic comparison misplaces it";
}

// The row that motivated the change: it must still come back, verbatim, through
// the production accessor. A range whose ends do not straddle the row proves the
// bound is a bound and not an accident.
TEST_F(CorpActionQueryBoundsDbTest, TextRangeReturnsTheLenSpinoffRow) {
    auto in_range = db_->get_corporate_actions({"LEN"}, "2025-02-01", "2025-02-28", {"spinoff"});
    ASSERT_TRUE(in_range.is_ok()) << in_range.error()->what();
    ASSERT_EQ(in_range.value().size(), 1u)
        << "LEN's 2025-02-07 Millrose spinoff is the row this range exists to select";
    EXPECT_EQ(in_range.value()[0].date_str, "2025-02-07");
    EXPECT_EQ(in_range.value()[0].action, "spinoff");
    EXPECT_EQ(in_range.value()[0].ticker, "LEN");
    EXPECT_EQ(in_range.value()[0].contra_ticker, "MRP");
    EXPECT_DOUBLE_EQ(in_range.value()[0].value, 0.5);

    // Both ends are INCLUSIVE, and one day inside either end excludes the row.
    auto on_both_bounds =
        db_->get_corporate_actions({"LEN"}, "2025-02-07", "2025-02-07", {"spinoff"});
    ASSERT_TRUE(on_both_bounds.is_ok()) << on_both_bounds.error()->what();
    EXPECT_EQ(on_both_bounds.value().size(), 1u) << "the bounds are inclusive at both ends";

    auto after_start = db_->get_corporate_actions({"LEN"}, "2025-02-08", "2025-02-28", {"spinoff"});
    ASSERT_TRUE(after_start.is_ok()) << after_start.error()->what();
    EXPECT_TRUE(after_start.value().empty()) << "start bound is not applied";

    auto before_end = db_->get_corporate_actions({"LEN"}, "2025-02-01", "2025-02-06", {"spinoff"});
    ASSERT_TRUE(before_end.is_ok()) << before_end.error()->what();
    EXPECT_TRUE(before_end.value().empty()) << "end bound is not applied";
}

// Lexicographic and calendar ordering must agree across the shapes that break
// naive string comparison elsewhere: a year boundary, a month boundary, and the
// zero-padded single-digit month/day that make ISO-8601 sortable in the first
// place.
TEST_F(CorpActionQueryBoundsDbTest, TextComparisonSelectsTheSameSetACastWould) {
    pqxx::connection c(conn_string_);
    pqxx::work w(c);
    // The two readings, side by side, over a window that spans a year end.
    auto cast_rows = w.exec(
        "SELECT ticker, date, action FROM equities_data.corporate_action "
        "WHERE ticker = ANY(ARRAY['AAPL','MSFT','LEN','SBDS','DD']) "
        "  AND date::date BETWEEN DATE '2024-12-15' AND DATE '2025-03-15' "
        "ORDER BY date, ticker, action");
    auto text_rows = w.exec(
        "SELECT ticker, date, action FROM equities_data.corporate_action "
        "WHERE ticker = ANY(ARRAY['AAPL','MSFT','LEN','SBDS','DD']) "
        "  AND date >= '2024-12-15' AND date <= '2025-03-15' "
        "ORDER BY date, ticker, action");
    w.commit();

    ASSERT_GT(cast_rows.size(), 0u) << "an empty window compares two empty sets";
    ASSERT_EQ(cast_rows.size(), text_rows.size())
        << "the text comparison changed WHICH rows the reader sees";
    for (size_t i = 0; i < cast_rows.size(); ++i) {
        EXPECT_EQ(std::string(cast_rows[i][0].c_str()), std::string(text_rows[i][0].c_str()));
        EXPECT_EQ(std::string(cast_rows[i][1].c_str()), std::string(text_rows[i][1].c_str()));
        EXPECT_EQ(std::string(cast_rows[i][2].c_str()), std::string(text_rows[i][2].c_str()));
    }
}

// ──────────────────────────────────────────────────────────────────────────
// G6-5 -- the UPPER bound is what keeps future-dated rows out of a run.
//
// equities_data.corporate_action carries rows dated in the future: SBDS has a
// tickerchangeto/from pair on 2027-07-18 (contra DTCB), placeholders the vendor
// emits ahead of an announced change. Every reader passes the run's as-of date
// as the end bound, so they are invisible today. Nothing pinned that. If the
// end bound is ever widened -- a "load everything, filter later" refactor, a
// bulk backfill -- a 2026 run would act on a 2027 rename.
// ──────────────────────────────────────────────────────────────────────────

TEST_F(CorpActionQueryBoundsDbTest, FutureDatedRowsAreExcludedByTheEndBound) {
    const auto tickerchange = vendor_labels_for_class(CorpActionClass::SERIES_CONTINUITY);
    ASSERT_FALSE(tickerchange.empty());

    // A run bounded at its own as-of date sees nothing.
    auto bounded = db_->get_corporate_actions({"SBDS"}, "2020-01-01", "2026-09-03", tickerchange);
    ASSERT_TRUE(bounded.is_ok()) << bounded.error()->what();
    for (const auto& r : bounded.value()) {
        EXPECT_LE(r.date_str, "2026-09-03")
            << "a row dated after the run leaked past the end bound: " << r.ticker << " "
            << r.date_str << " " << r.action;
    }

    // The rows are really there -- widening the bound returns them, which is what
    // makes the assertion above a bound and not an empty table.
    auto widened = db_->get_corporate_actions({"SBDS"}, "2020-01-01", "2027-12-31", tickerchange);
    ASSERT_TRUE(widened.is_ok()) << widened.error()->what();
    size_t future_rows = 0;
    for (const auto& r : widened.value()) {
        if (r.date_str > "2026-09-03") ++future_rows;
    }
    EXPECT_EQ(future_rows, 2u)
        << "SBDS's 2027-07-18 tickerchangeto/from pair is the fixture this pin rests on; "
           "if the data changed, re-pick a future-dated symbol rather than deleting the test";
    EXPECT_GT(widened.value().size(), bounded.value().size())
        << "the two bounds must not return the same set, or the bound is doing nothing";
}

// The same statement one layer down, independent of the accessor: the bound is a
// property of the query, and a lexicographic upper bound on ISO text orders the
// same way a date bound does across the year boundary the future rows sit past.
TEST_F(CorpActionQueryBoundsDbTest, TheFutureRowsExistAndAreDatedBeyondAnyRun) {
    pqxx::connection c(conn_string_);
    pqxx::work w(c);
    auto rows = w.exec(
        "SELECT date, action FROM equities_data.corporate_action "
        "WHERE ticker = 'SBDS' AND date > '2026-12-31' ORDER BY date, action");
    const auto max_real = w.exec(
        "SELECT max(date) FROM equities_data.corporate_action "
        "WHERE action NOT IN ('tickerchangeto','tickerchangefrom')")[0][0].c_str();
    w.commit();

    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(std::string(rows[0][0].c_str()), "2027-07-18");
    EXPECT_EQ(std::string(rows[1][0].c_str()), "2027-07-18");
    EXPECT_LT(std::string(max_real), "2027-01-01")
        << "only the tickerchange placeholders are future-dated; if a real event ever is, "
           "every reader's as-of bound becomes load-bearing for more than renames";
}
