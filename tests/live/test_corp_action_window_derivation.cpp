#include <gtest/gtest.h>

#include <ctime>
#include <string>
#include <unordered_map>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/corp_action_window.hpp"

using namespace trade_ngin;

// The corp-action lookback must reach back to when a position was ESTABLISHED.
// It used to be derived from Position::last_update, which cannot work:
// load_positions_by_date() selects WHERE DATE(last_update) = DATE($n), so every
// row it returns carries the requested date by construction, and the live table
// has zero rows where last_update differs from date. That derivation always
// collapsed to "yesterday", silently leaving the effective window at its 14-day
// floor -- the very window that dropped 8 of the 9 dividends the configured
// universe saw after live last wrote on 2026-05-03.
namespace {

constexpr long kDay = 24 * 60 * 60;
constexpr long kMinDays = 14;
constexpr long kBulkDays = 730;

// A fixed "today" so the arithmetic is readable: 2026-09-01T00:00:00Z.
constexpr std::time_t kToday = 1788220800;

std::string ymd_days_ago(long days) {
    return format_ymd_utc(kToday - days * kDay);
}

}  // namespace

// A holding established well inside the bulk window still pulls the window back
// to its inception -- not to the 14-day floor.
TEST(CorpActionWindowDerivation, WindowReachesBackToInception) {
    std::unordered_map<std::string, std::string> inception{
        {"AAPL", ymd_days_ago(400)},
    };

    const auto w = derive_corp_action_window(kToday, kMinDays, kBulkDays, inception);

    EXPECT_EQ(w.start, parse_ymd_utc(ymd_days_ago(400)))
        << "window must cover the whole holding period, not the floor";
    EXPECT_TRUE(w.deep_symbols.empty())
        << "400 days is inside the 730-day bulk load; no top-up needed";
}

// THE REGRESSION THIS FIX EXISTS FOR: a holding older than the bulk price load
// must widen the window to its inception AND be flagged for a per-symbol close
// top-up. The old code clamped here and applied a partial adjustment, which
// leaves that position's cost basis permanently wrong.
TEST(CorpActionWindowDerivation, HoldingOlderThanBulkLoadIsCoveredNotTruncated) {
    std::unordered_map<std::string, std::string> inception{
        {"IBM", ymd_days_ago(1500)},  // ~4 years, well beyond the 730-day load
    };

    const auto w = derive_corp_action_window(kToday, kMinDays, kBulkDays, inception);

    EXPECT_EQ(w.start, parse_ymd_utc(ymd_days_ago(1500)))
        << "window must NOT be clamped to the bulk window edge";
    EXPECT_LT(w.start, kToday - kBulkDays * kDay)
        << "window start must predate the bulk load, proving no truncation";

    ASSERT_EQ(w.deep_symbols.size(), 1u);
    EXPECT_EQ(w.deep_symbols[0], "IBM")
        << "the old holding must be flagged so its closes get topped up";
    EXPECT_EQ(w.deep_start, parse_ymd_utc(ymd_days_ago(1500)));
}

// The top-up must stay targeted: symbols inside the bulk window are never
// added, so the extension cannot degenerate into a full-universe deep load.
TEST(CorpActionWindowDerivation, OnlySymbolsNeedingItAreFlaggedForTopUp) {
    std::unordered_map<std::string, std::string> inception{
        {"OLD_A", ymd_days_ago(2000)},
        {"OLD_B", ymd_days_ago(900)},
        {"RECENT_A", ymd_days_ago(300)},
        {"RECENT_B", ymd_days_ago(10)},
    };

    const auto w = derive_corp_action_window(kToday, kMinDays, kBulkDays, inception);

    ASSERT_EQ(w.deep_symbols.size(), 2u)
        << "only holdings predating the bulk load need a top-up";
    std::vector<std::string> got = w.deep_symbols;
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got[0], "OLD_A");
    EXPECT_EQ(got[1], "OLD_B");

    EXPECT_EQ(w.deep_start, parse_ymd_utc(ymd_days_ago(2000)))
        << "top-up must start at the earliest deep inception, covering both";
}

// With no positions at all there is nothing to reach back for, so the floor
// applies -- weekend/holiday stacks stay covered cheaply.
TEST(CorpActionWindowDerivation, EmptyBookFallsBackToTheFloor) {
    const auto w = derive_corp_action_window(kToday, kMinDays, kBulkDays, {});

    EXPECT_EQ(w.start, kToday - kMinDays * kDay);
    EXPECT_TRUE(w.deep_symbols.empty());
}

// Malformed inception dates must not silently narrow the window: a bad row is
// skipped, and the remaining holdings still set the bound.
TEST(CorpActionWindowDerivation, MalformedInceptionDoesNotNarrowTheWindow) {
    std::unordered_map<std::string, std::string> inception{
        {"GOOD", ymd_days_ago(500)},
        {"BAD", "not-a-date"},
    };

    const auto w = derive_corp_action_window(kToday, kMinDays, kBulkDays, inception);

    EXPECT_EQ(w.start, parse_ymd_utc(ymd_days_ago(500)))
        << "a malformed row must not pull the window in";
}

// Dates are parsed and formatted as UTC. On the deployed image
// (TZ=America/New_York) a localtime round-trip pushes every key a day early,
// which is what put the dividend denominator on the wrong close (F-2/F-3).
TEST(CorpActionWindowDerivation, DateRoundTripIsUtcNotLocaltime) {
    const std::string ymd = "2026-08-10";
    EXPECT_EQ(format_ymd_utc(parse_ymd_utc(ymd)), ymd);

    // Midnight UTC is the previous evening in New York; the round-trip must not
    // drift regardless of the host zone.
    const std::time_t midnight_utc = parse_ymd_utc(ymd);
    std::tm tm{};
    gmtime_r(&midnight_utc, &tm);
    EXPECT_EQ(tm.tm_mday, 10);
    EXPECT_EQ(tm.tm_hour, 0);
}

// Pins WHY the derivation source changed, so a revert to last_update fails here.
// load_positions_by_date() returns rows whose last_update IS the requested date
// (it selects on exactly that), so feeding those dates in is indistinguishable
// from having no history at all: the window collapses to the floor and every
// event older than 14 days is silently dropped. Real inception dates for the
// same book reach back 1500 days.
TEST(CorpActionWindowDerivation, LastUpdateDatesCollapseToTheFloorButInceptionDoesNot) {
    // What last_update yields for a book held for years: yesterday, for every row.
    std::unordered_map<std::string, std::string> from_last_update{
        {"IBM", ymd_days_ago(1)},
        {"AAPL", ymd_days_ago(1)},
    };
    const auto collapsed =
        derive_corp_action_window(kToday, kMinDays, kBulkDays, from_last_update);

    EXPECT_EQ(collapsed.start, kToday - kMinDays * kDay)
        << "last_update-derived dates cannot widen the window past the floor";
    EXPECT_TRUE(collapsed.deep_symbols.empty())
        << "and they can never reveal a holding that predates the bulk load";

    // The same book, sourced from position history instead.
    std::unordered_map<std::string, std::string> from_inception{
        {"IBM", ymd_days_ago(1500)},
        {"AAPL", ymd_days_ago(1)},
    };
    const auto derived =
        derive_corp_action_window(kToday, kMinDays, kBulkDays, from_inception);

    EXPECT_LT(derived.start, collapsed.start)
        << "inception must reach further back than the floor for an old book";
    ASSERT_EQ(derived.deep_symbols.size(), 1u);
    EXPECT_EQ(derived.deep_symbols[0], "IBM");
}

// The window's lower bound must report WHICH rule set it. A window sitting at
// the floor while positions are held is the exact signature of the last_update
// regression this derivation replaced -- and with only a date logged, that is
// indistinguishable from a legitimate floor.
TEST(CorpActionWindowDerivation, FloorReportsItselfAsTheSource) {
    const std::time_t today = parse_ymd_utc("2026-08-31");
    const auto w = derive_corp_action_window(today, 14, 730, {});

    EXPECT_EQ(w.source, CorpActionWindowSource::Floor);
    EXPECT_TRUE(w.source_symbol.empty());
    EXPECT_STREQ(to_string(w.source), "floor");
}

TEST(CorpActionWindowDerivation, InceptionReportsTheSymbolThatWidenedTheWindow) {
    const std::time_t today = parse_ymd_utc("2026-08-31");
    // AAPL is recent enough to leave the floor alone; ZT reaches back further.
    const std::unordered_map<std::string, std::string> inception{
        {"AAPL", "2026-08-25"},
        {"ZT.v.0", "2025-11-11"},
    };
    const auto w = derive_corp_action_window(today, 14, 730, inception);

    EXPECT_EQ(w.source, CorpActionWindowSource::Inception);
    EXPECT_EQ(w.source_symbol, "ZT.v.0")
        << "the symbol reported must be the one that actually set the bound";
    EXPECT_EQ(w.start, parse_ymd_utc("2025-11-11"));
    EXPECT_STREQ(to_string(w.source), "inception");
}

// A holding inside the floor must not claim to have widened anything.
TEST(CorpActionWindowDerivation, HoldingNewerThanTheFloorLeavesTheSourceAtFloor) {
    const std::time_t today = parse_ymd_utc("2026-08-31");
    const std::unordered_map<std::string, std::string> inception{{"AAPL", "2026-08-29"}};
    const auto w = derive_corp_action_window(today, 14, 730, inception);

    EXPECT_EQ(w.source, CorpActionWindowSource::Floor);
    EXPECT_TRUE(w.source_symbol.empty());
}

// A deep holding both widens the window and is flagged for close top-up; the
// reported source must still be the symbol that set the bound.
TEST(CorpActionWindowDerivation, DeepHoldingIsReportedAsTheSource) {
    const std::time_t today = parse_ymd_utc("2026-08-31");
    const std::unordered_map<std::string, std::string> inception{{"OLD", "2022-01-03"}};
    const auto w = derive_corp_action_window(today, 14, 730, inception);

    EXPECT_EQ(w.source, CorpActionWindowSource::Inception);
    EXPECT_EQ(w.source_symbol, "OLD");
    ASSERT_EQ(w.deep_symbols.size(), 1u);
    EXPECT_EQ(w.deep_symbols[0], "OLD");
}
