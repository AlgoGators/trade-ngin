// B-ii regression pins -- the staleness guard must be driven by the universe it was ASKED
// about, not by the rows that happened to come back.
//
// THE DEFECT: assess_feed_freshness() took only `last_bar_date`, the map built by walking
// the bars that loaded (live_equity_mean_reversion.cpp:770-775). A symbol with zero rows in
// ohlcv_1d never enters that map, so it cannot be the minimum and cannot set
// `stalest_date`. `f.symbols` was `last_bar_date.size()` -- the count of symbols WITH data.
//
// At ten configured names with one symbol absent the guard logs
//     "stalest of 9 symbols (AAPL) at <yesterday>, 1 days behind"
// for a ten-name book, and passes. The absent symbol is the one that matters: it reaches
// day T unpriced, execute_day_t rule 3 rolls its target back to the carried quantity, and
// the book carries an unpriceable holding -- exactly the zombie-carry the guard exists to
// pre-empt (E2-F34).
//
// This is the shape E2_RUN_PROTOCOL.md §1.5 warns about: at 852 names the failure is not
// "the feed is a day late", it is "eleven names silently stopped printing".
//
// An absent symbol has no last-bar date, so it cannot be expressed as a number of days
// behind. It is reported as its own quantity (`absent`, `absent_symbol`) and the caller
// treats any absence as stale, rather than inventing a days_behind for it.

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/live/data_freshness.hpp"

using namespace trade_ngin;

namespace {
std::unordered_map<std::string, std::string> nine_of_ten_loaded() {
    return {
        {"AAPL", "2026-04-15"}, {"MSFT", "2026-04-16"}, {"GOOGL", "2026-04-16"},
        {"AMZN", "2026-04-16"}, {"META", "2026-04-16"}, {"TMUS", "2026-04-16"},
        {"BKNG", "2026-04-16"}, {"DD", "2026-04-16"},   {"LEN", "2026-04-16"},
    };
}
const std::vector<std::string> kRequested = {"AAPL", "MSFT",  "GOOGL", "AMZN", "META",
                                             "TMUS", "BKNG",  "DD",    "LEN",  "ABEV"};
}  // namespace

TEST(FeedFreshnessUniverse, AnAbsentSymbolIsCountedAndNamed) {
    const auto f = assess_feed_freshness(nine_of_ten_loaded(), "2026-04-16", kRequested);

    EXPECT_EQ(f.symbols, 10u) << "the guard must report the universe it was asked about, "
                                 "not the nine that answered";
    EXPECT_EQ(f.absent, 1u);
    EXPECT_EQ(f.absent_symbol, "ABEV") << "the absent symbol must be nameable in the WARN";
}

TEST(FeedFreshnessUniverse, AnAbsentSymbolDoesNotDisturbTheStalestLoadedSymbol) {
    // The existing threshold policy is unchanged: days_behind still describes the stalest
    // symbol that actually printed. Absence is reported separately because it has no date.
    const auto f = assess_feed_freshness(nine_of_ten_loaded(), "2026-04-16", kRequested);

    EXPECT_TRUE(f.any_data);
    EXPECT_EQ(f.stalest_symbol, "AAPL");
    EXPECT_EQ(f.stalest_date, "2026-04-15");
    EXPECT_EQ(f.days_behind, 1);
}

TEST(FeedFreshnessUniverse, EveryRequestedSymbolPresentReportsNoAbsence) {
    auto loaded = nine_of_ten_loaded();
    loaded["ABEV"] = "2026-04-16";
    const auto f = assess_feed_freshness(loaded, "2026-04-16", kRequested);

    EXPECT_EQ(f.symbols, 10u);
    EXPECT_EQ(f.absent, 0u);
    EXPECT_TRUE(f.absent_symbol.empty());
}

TEST(FeedFreshnessUniverse, TheWholeUniverseAbsentIsMaximallyStaleNotNeutral) {
    const auto f = assess_feed_freshness({}, "2026-04-16", kRequested);

    EXPECT_FALSE(f.any_data);
    EXPECT_EQ(f.symbols, 10u);
    EXPECT_EQ(f.absent, 10u);
    EXPECT_FALSE(f.absent_symbol.empty());
}

TEST(FeedFreshnessUniverse, AbsenceIsDeterministicallyNamed) {
    auto loaded = nine_of_ten_loaded();
    loaded.erase("TMUS");
    // Two absent: ABEV and TMUS. The first in sort order is reported so two runs on the
    // same state produce the same log line.
    const auto f = assess_feed_freshness(loaded, "2026-04-16", kRequested);
    EXPECT_EQ(f.absent, 2u);
    EXPECT_EQ(f.absent_symbol, "ABEV");
}

TEST(FeedFreshnessUniverse, WithNoUniverseSuppliedTheOldBehaviourIsUnchanged) {
    // The three-argument form is what the pre-fix callers used; it must still report the
    // loaded count and no absence, so nothing that has not been taught about the universe
    // changes meaning.
    const auto f = assess_feed_freshness(nine_of_ten_loaded(), "2026-04-16");
    EXPECT_EQ(f.symbols, 9u);
    EXPECT_EQ(f.absent, 0u);
    EXPECT_EQ(f.stalest_symbol, "AAPL");
}
