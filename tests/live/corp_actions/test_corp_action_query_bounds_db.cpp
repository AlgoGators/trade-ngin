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
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set, "
                          "so the corporate_action query bounds go unverified";
            }
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set; no database to exercise";
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
    // B-v: `const std::string`, NOT `const auto`. `auto` deduced `const char*` pointing
    // into the pqxx::result temporary, which was destroyed (and its buffer PQclear'd) at
    // the end of this statement -- so the comparison three statements below read freed
    // memory and the assertion was not actually being made. Copy the bytes out.
    const std::string max_real = w.exec(
        "SELECT max(date) FROM equities_data.corporate_action "
        "WHERE action NOT IN ('tickerchangeto','tickerchangefrom')")[0][0].c_str();
    w.commit();

    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(std::string(rows[0][0].c_str()), "2027-07-18");
    EXPECT_EQ(std::string(rows[1][0].c_str()), "2027-07-18");
    // The comparison is only meaningful if the value survived the statement that produced
    // it: a dangling read yields empty or garbage, and "" < "2027-01-01" passes vacuously.
    ASSERT_EQ(max_real.size(), 10u)
        << "max(date) must come back as a ten-character ISO date, not a dangling read";
    EXPECT_LT(max_real, "2027-01-01")
        << "only the tickerchange placeholders are future-dated; if a real event ever is, "
           "every reader's as-of bound becomes load-bearing for more than renames";
}


// ──────────────────────────────────────────────────────────────────────────
// V4-1 -- the LEN/Millrose hole.
//
// Class-1 (price-restating) effects are sourced PER BAR: the applier reads
// ohlcv_1d.split_factor and ohlcv_1d.div_cash via get_per_bar_corporate_actions,
// never corporate_action.value. equities_data.corporate_action is consulted only
// for class-2 renames and class-3 deal terms. So a class-1 row whose bar carries
// no move is applied by NOTHING -- no error, no WARN, no dedup row, no deferral.
// The position carries an unrestated cost basis across a real event, forever.
//
// LEN 2025-02-07 is that shape: corporate_action holds `spinoff` MRP 0.5 and
// `spinoffdividend` 11.495, while the LEN bar for that day carries split_factor 1
// and div_cash 0 -- and the close fell 127.25 -> 121.94.
//
// Two distinct shapes come out of the same join and must not be conflated:
//
//   (a) a bar EXISTS on the ex-date and is flat  -- V4-1. Nothing downstream can
//       ever notice, because the applier only sees bars and this bar says
//       "nothing happened". This is the failing assertion below.
//   (b) NO bar exists on the ex-date -- the applier has no input and the runner
//       already says so out loud ("No price history exists for held symbol(s)")
//       and leaves the event unapplied for a later run to pick up. Not silent,
//       and not this test's subject; what IS pinned is that this shape only ever
//       happens to dividends. A missing bar on a split or spinoff date would mean
//       a share count that never adjusts.
// ──────────────────────────────────────────────────────────────────────────

namespace {

// Bars carry split_factor/div_cash reliably over the era a live book can reach
// (this book's positions start 2026-04-01). Before 2020 the rows are dominated
// by ticker-reuse artefacts -- DD's 2018-19 dividends belong to DowDuPont, whose
// history now lives under DD1 -- which say nothing about the applier.
constexpr const char* kBarEraFloor = "2020-01-01";

// (ticker, date, action) triples KNOWN to be silent and accepted. The allowlist
// is the point of the test: a corporate action nothing applies is tolerable only
// once somebody has looked at it and written down why.
//
// B-5b widened the scan from the configured book to EVERY symbol with bars since
// 2020-01-01 (see tripwire_universe()), which is what E2-F41 asked for: the
// ten-symbol book hid the whole class. Measured 2026-09-04, universe-wide, with a
// bar present on the ex-date: 87 dividends, 5 `spinoff` + 5 `spinoffdividend`,
// 1 adrratiosplit, 1 split.
//
// The QUANTITY-CHANGING and DISTRIBUTION-BEARING shapes are enumerated here, one
// row each, because those are the ones a silent no-op cannot be recovered from --
// a share count that never adjusts, or a child never received. Dividends are the
// numerous, less dangerous shape and are bounded by count instead (see
// kMaxSilentDividends); listing 87 of them would make this a data dump rather
// than a decision.
const std::set<std::vector<std::string>>& allowlisted_silent_class1() {
    static const std::set<std::vector<std::string>> a = {
        // The Millrose (MRP) spin-off. Tiingo encoded it in neither split_factor
        // nor div_cash on the LEN bar, so the class-1 applier cannot see it at
        // all. E2-F31 receives spinoff children now, but it is driven by the
        // PER-BAR row: a bar with no move produces no row, so the routing never
        // fires and this stays silent. LEN is not in the traded universe.
        // Both vendor labels for the one event are listed -- corporate_action
        // carries a `spinoff` row (ratio 0.5) and a `spinoffdividend` row
        // (11.495) for the same date.
        {"LEN", "2025-02-07", "spinoff"},
        {"LEN", "2025-02-07", "spinoffdividend"},

        // The four other spinoffs of the same shape, none in any equity config.
        // Each is a real distribution the bar does not carry, so E2-F31 cannot
        // see them either; each is now announced at run time by the E2-F41
        // cross-check if the symbol is ever held.
        {"TT", "2020-03-02", "spinoff"},           // Trane -> Ingersoll Rand, 0.8824
        {"TT", "2020-03-02", "spinoffdividend"},   // ... 29.8162 the same day
        {"MTCH", "2020-07-01", "spinoff"},         // Match -> IAC, 2.0
        {"MTCH", "2020-07-01", "spinoffdividend"}, // ... 222.315
        {"OXY", "2020-08-03", "spinoff"},          // Occidental -> OXY.WS warrants, 0.125
        {"OXY", "2020-08-03", "spinoffdividend"},  // ... 0.6
        {"AMC", "2022-08-22", "spinoff"},          // AMC -> APE units, 1.0
        {"AMC", "2022-08-22", "spinoffdividend"},  // ... 69.5

        // BIDU's 2021 ADR ratio change: value 80 with a flat bar. An ADR ratio
        // change IS a share-count change, so a silent one is the dangerous kind;
        // it is allowlisted because BIDU is in no equity config, not because it
        // is harmless.
        {"BIDU", "2021-03-01", "adrratiosplit"},

        // MBC 2022-12-15 `split` with value 1280 and a flat bar. A 1280:1 split
        // is not a corporate action, it is a units artefact in the deal-terms
        // feed (MBC listed in December 2022); the bar is right and the row is
        // wrong. In no equity config.
        {"MBC", "2022-12-15", "split"},
    };
    return a;
}

// Dividends whose ex-date bar carries div_cash 0 are the numerous shape: 87 of
// them universe-wide since 2020. They are still unapplied basis restatements, so
// the count is pinned -- it may not grow without somebody looking -- but they are
// not enumerated. A dividend cannot change a share count, and the equity book's
// own dividends are all visible to the per-bar applier (the E2-F41 run-time
// cross-check reports zero for the configured universe on both gate windows).
constexpr size_t kMaxSilentDividends = 95;

// EVERY symbol with bars in the era a live book can reach -- not the configured
// ten (E2-F41, widened in B-5b).
//
// Scanning the configured universe was the flaw the finding names: the book holds
// ten symbols and the whole silent class lives outside them, so the tripwire was
// green while ~99 rows sat unapplied one config edit away. `equities_data.ohlcv_1d`
// since 2020-01-01 is roughly 850 symbols, which is the set any equity config on
// this branch can name, and the join is indexed on (symbol, time).
std::vector<std::string> tripwire_universe(const std::string& conn) {
    pqxx::connection c(conn);
    pqxx::work w(c);
    auto rows = w.exec(
        "SELECT DISTINCT symbol FROM equities_data.ohlcv_1d WHERE time >= $1::date",
        pqxx::params{std::string(kBarEraFloor)});
    w.commit();

    std::set<std::string> symbols = {"LEN"};  // the proven instance, kept even if it ever loses bars
    for (const auto& r : rows) symbols.insert(r[0].c_str());
    return std::vector<std::string>(symbols.begin(), symbols.end());
}

struct SilentClass1Row {
    std::string ticker, date, action;
    bool has_bar = false;
    double split_factor = 1.0;
    double div_cash = 0.0;
};

std::vector<SilentClass1Row> scan_silent_class1(const std::string& conn,
                                                const std::vector<std::string>& universe) {
    pqxx::connection c(conn);
    pqxx::work w(c);
    auto rows = w.exec(
        "SELECT ca.ticker, ca.date, ca.action, "
        "       (b.symbol IS NOT NULL) AS has_bar, "
        "       COALESCE(b.split_factor, 1) AS split_factor, "
        "       COALESCE(b.div_cash, 0) AS div_cash "
        "FROM equities_data.corporate_action ca "
        "LEFT JOIN equities_data.ohlcv_1d b "
        "       ON b.symbol = ca.ticker AND b.time::date = ca.date::date "
        "WHERE ca.ticker = ANY($1) AND ca.action = ANY($2) AND ca.date >= $3 "
        "ORDER BY ca.date, ca.ticker, ca.action",
        pqxx::params{universe, vendor_labels_for_class(CorpActionClass::PRICE_RESTATING),
                     std::string(kBarEraFloor)});
    w.commit();

    std::vector<SilentClass1Row> out;
    for (const auto& r : rows) {
        SilentClass1Row s;
        s.ticker = r["ticker"].c_str();
        s.date = r["date"].c_str();
        s.action = r["action"].c_str();
        s.has_bar = r["has_bar"].as<bool>();
        s.split_factor = r["split_factor"].as<double>();
        s.div_cash = r["div_cash"].as<double>();
        if (s.has_bar && (s.split_factor != 1.0 || s.div_cash != 0.0)) continue;  // applied
        out.push_back(std::move(s));
    }
    return out;
}

}  // namespace

// (a) The silent no-op. A bar exists, the applier reads it, and it says nothing
// happened.
TEST_F(CorpActionQueryBoundsDbTest, NoSilentlyUnappliedClass1RowInTheLiveUniverse) {
    const auto universe = tripwire_universe(conn_string_);
    ASSERT_GE(universe.size(), 500u)
        << "the scan set is " << universe.size()
        << " symbols; E2-F41 is precisely that the ten configured names hide the class, so a "
           "small set here proves nothing";

    const auto silent = scan_silent_class1(conn_string_, universe);

    std::vector<std::string> unexplained;
    size_t silent_dividends = 0;
    for (const auto& s : silent) {
        if (!s.has_bar) continue;  // shape (b), pinned by the next test
        if (allowlisted_silent_class1().count({s.ticker, s.date, s.action})) continue;
        // Dividends are bounded by count rather than enumerated -- see the comment on
        // kMaxSilentDividends. They cannot change a share count, and 87 named rows would
        // turn this allowlist into a data dump nobody reads.
        if (s.action == "dividend") {
            ++silent_dividends;
            continue;
        }
        unexplained.push_back(s.ticker + " " + s.date + " " + s.action +
                              " (bar present, split_factor=" + std::to_string(s.split_factor) +
                              " div_cash=" + std::to_string(s.div_cash) + ")");
    }

    std::string detail;
    for (const auto& u : unexplained) detail += "\n    " + u;
    EXPECT_TRUE(unexplained.empty())
        << unexplained.size()
        << " class-1 corporate_action row(s) that CHANGE A SHARE COUNT or DELIVER A "
           "SECURITY have a bar on the ex-date that carries no move, so the applier is a "
           "silent no-op and neither the quantity nor the cost basis is ever restated:"
        << detail
        << "\n  Either the bar data is wrong (fix the feed) or the row is genuinely "
           "inapplicable (add it to allowlisted_silent_class1 with the reason).";

    EXPECT_LE(silent_dividends, kMaxSilentDividends)
        << silent_dividends << " dividends across the whole universe have a flat ex-date bar "
           "(bound " << kMaxSilentDividends
        << "). Each is an unapplied basis restatement. The bound exists so the class cannot "
           "grow unnoticed -- if the feed has degraded, raise it deliberately and say why; if "
           "a configured symbol is now among them, the run-time E2-F41 cross-check will name "
           "it every session.";
    EXPECT_GT(silent_dividends, 0u)
        << "no silent dividends at all means the scan is not reaching the table; E2-F41 "
           "measured 87 on 2026-09-04";
}

// The allowlist must never become a rubber stamp: every entry has to name a row
// that is really there and really silent. An entry that stops matching means the
// data moved and the exemption is now hiding something else.
TEST_F(CorpActionQueryBoundsDbTest, EveryAllowlistedSilentRowStillExistsAndIsStillSilent) {
    ASSERT_FALSE(allowlisted_silent_class1().empty())
        << "an empty allowlist makes the tripwire above prove nothing about LEN";

    const auto silent = scan_silent_class1(conn_string_, tripwire_universe(conn_string_));
    std::set<std::vector<std::string>> observed;
    for (const auto& s : silent) {
        if (s.has_bar) observed.insert({s.ticker, s.date, s.action});
    }
    for (const auto& entry : allowlisted_silent_class1()) {
        EXPECT_EQ(observed.count(entry), 1u)
            << "allowlisted " << entry[0] << " " << entry[1] << " " << entry[2]
            << " is no longer a silent class-1 row -- remove the exemption rather than "
               "leaving it to cover a future one";
    }
}

// (b) The other shape: no bar at all on the ex-date. The runner is loud about
// this one and retries it, so it is not V4-1 -- but it must stay confined to
// dividends. A split or spinoff whose ex-date has no bar means a share count
// that never adjusts, which no later run can repair.
TEST_F(CorpActionQueryBoundsDbTest, AMissingBarOnAClass1DateIsOnlyEverADividend) {
    const auto silent = scan_silent_class1(conn_string_, tripwire_universe(conn_string_));

    std::vector<std::string> quantity_changing;
    size_t missing_bar_dividends = 0;
    for (const auto& s : silent) {
        if (s.has_bar) continue;
        if (s.action == "dividend") {
            ++missing_bar_dividends;
            continue;
        }
        quantity_changing.push_back(s.ticker + " " + s.date + " " + s.action);
    }

    std::string detail;
    for (const auto& q : quantity_changing) detail += "\n    " + q;
    EXPECT_TRUE(quantity_changing.empty())
        << "a quantity-changing class-1 event has no bar on its ex-date, so the share "
           "count is never adjusted and no later run can repair it:"
        << detail;

    // Recorded, not asserted to be zero: these are real and are why the runner's
    // "No price history exists for held symbol(s)" ERROR is load-bearing.
    RecordProperty("missing_bar_dividends", static_cast<int>(missing_bar_dividends));
}

// ──────────────────────────────────────────────────────────────────────────
// E4 item 3 -- against the real table.
//
// The pure tests in test_effect_classes.cpp pin what the status says for a given
// measurement. This one pins that a measurement HAPPENS: the reported date has
// to equal what the server holds, which a literal cannot do once the feed moves.
// ──────────────────────────────────────────────────────────────────────────

#include "trade_ngin/live/corp_action_feed_status.hpp"

TEST_F(CorpActionQueryBoundsDbTest, FrozenAfterDateIsMeasuredNotHardcoded) {
    const std::string as_of = "2026-09-03";

    auto measured = db_->get_corp_action_feed_last_date(as_of);
    ASSERT_TRUE(measured.is_ok()) << measured.error()->what();

    // Independently derived, not read back from the same accessor.
    pqxx::connection c(conn_string_);
    pqxx::work w(c);
    const std::string expected =
        w.exec_params("SELECT COALESCE(max(date), '') FROM equities_data.corporate_action "
                      "WHERE date <= $1",
                      as_of)[0][0]
            .c_str();
    w.commit();

    ASSERT_FALSE(expected.empty()) << "the table is empty; this proves nothing";
    EXPECT_EQ(measured.value(), expected)
        << "the reported last-row date is not what the table holds -- a literal cannot "
           "track a revived feed";

    // The audit's assertion: it is at least as new as the compiled-in date, and
    // the accessor is the thing that would move if the feed were backfilled.
    EXPECT_GE(measured.value(), std::string(kCorpActionTableFrozenAfter))
        << "the feed appears to have LOST rows since " << kCorpActionTableFrozenAfter
        << "; that is a data incident, not a code defect";

    const auto status = assess_corp_action_feed(measured.value(), as_of);
    EXPECT_TRUE(status.measured);
    EXPECT_EQ(status.frozen, expected < as_of);

    // The upper bound matters: the table carries tickerchange placeholders dated
    // 2027-07-18, and an unbounded max() would report a feed running ahead of the
    // run -- which would make `frozen` false and the WARN text nonsense.
    auto unbounded = db_->get_corp_action_feed_last_date();
    ASSERT_TRUE(unbounded.is_ok()) << unbounded.error()->what();
    EXPECT_GT(unbounded.value(), measured.value())
        << "the future-dated rows are what the as-of bound exists to exclude";
    EXPECT_FALSE(assess_corp_action_feed(unbounded.value(), as_of).frozen)
        << "and this is the wrong answer the bound prevents";
}
