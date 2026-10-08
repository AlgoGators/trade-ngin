// tests/live/corp_actions/test_rename_eras.cpp
//
// FIX-1: renames are bound to the era the position was established in.
//
// ticker_aliases is a historical map, but tickers get REUSED. Our own backfill maps
// META -> METV (effective_until 2022-01-31); META has been Meta Platforms ever since,
// with 3,589 bars through 2026-08-28, while METV has none. Applying that alias to a
// position opened in 2026 re-keys a live holding onto a symbol with no prices -- and
// 131 historical tickers in the live table are still actively trading, 33 of them with
// two or more successors, so a map keyed on historical_ticker alone also picks an
// arbitrary winner. The fix resolves an alias against the date the POSITION was
// established, the same way the dedup mirror resolves one against an event's ex_date.
//
// The class-3 delisting guard is tested here too: it is the same hazard (a stale row
// inherited from a prior issuer) one class over.

#include <gtest/gtest.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/corporate_actions_lifecycle.hpp"

using namespace trade_ngin;

namespace {

Position held(const std::string& symbol, double qty, double avg_price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(avg_price);
    p.unrealized_pnl = Decimal(0.0);
    p.realized_pnl = Decimal(0.0);
    p.last_update = std::chrono::system_clock::now();
    return p;
}

constexpr const char* kToday = "2026-08-31";

}  // namespace

// --------------------------------------------------------------------------
// The regression this fix exists for.
// --------------------------------------------------------------------------

TEST(RenameEras, MetaOpenedAfterTheRenameIsNotRekeyedOntoMetv) {
    std::unordered_map<std::string, Position> positions;
    positions["META"] = held("META", 30.0, 512.40);

    // The row our backfill actually carries.
    std::vector<TickerAlias> aliases = {{"META", "METV", "2022-01-31", "backfill"}};
    // Meta Platforms, bought last month -- four years AFTER the alias's era closed.
    std::unordered_map<std::string, std::string> inception = {{"META", "2026-08-03"}};

    auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, kToday, inception);

    EXPECT_TRUE(log.empty());
    ASSERT_EQ(positions.count("META"), 1u) << "a live holding was re-keyed onto a dead symbol";
    EXPECT_EQ(positions.count("METV"), 0u);
    EXPECT_DOUBLE_EQ(positions["META"].quantity.as_double(), 30.0);
    EXPECT_DOUBLE_EQ(positions["META"].average_price.as_double(), 512.40);
}

TEST(RenameEras, MetaHeldFromInsideTheEraIsRekeyed) {
    // The other direction: era-bounding must not become "never rename". A position
    // genuinely established while META meant Meta Materials still stitches to METV.
    std::unordered_map<std::string, Position> positions;
    positions["META"] = held("META", 30.0, 4.10);

    std::vector<TickerAlias> aliases = {{"META", "METV", "2022-01-31", "backfill"}};
    std::unordered_map<std::string, std::string> inception = {{"META", "2021-06-15"}};

    auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, kToday, inception);

    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::RENAMED);
    EXPECT_EQ(log[0].contra_ticker, "METV");
    EXPECT_EQ(positions.count("META"), 0u);
    ASSERT_EQ(positions.count("METV"), 1u);
    EXPECT_DOUBLE_EQ(positions["METV"].quantity.as_double(), 30.0);
    // A rename is not an economic event: basis carries across untouched.
    EXPECT_DOUBLE_EQ(positions["METV"].average_price.as_double(), 4.10);
    EXPECT_EQ(positions["METV"].symbol, "METV");
}

// --------------------------------------------------------------------------
// Multi-successor tickers: 33 of them in the live table.
// --------------------------------------------------------------------------

TEST(RenameEras, MultiSuccessorTickerResolvesToTheEraTheHoldingBelongsTo) {
    // BBT -> BBT1 (until 1998-12-10), then BBT -> TFC (until 2019-12-10). Keyed on the
    // ticker alone one of these wins arbitrarily; keyed on inception each holding
    // resolves to its own era.
    std::vector<TickerAlias> aliases = {{"BBT", "TFC", "2019-12-10", ""},
                                        {"BBT", "BBT1", "1998-12-10", ""}};

    {
        std::unordered_map<std::string, Position> positions;
        positions["BBT"] = held("BBT", 10.0, 20.0);
        auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, kToday,
                                                            {{"BBT", "1997-05-01"}});
        ASSERT_EQ(log.size(), 1u);
        EXPECT_EQ(positions.count("BBT1"), 1u) << "the 1997 holding belongs to the first era";
        EXPECT_EQ(positions.count("TFC"), 0u);
    }
    {
        std::unordered_map<std::string, Position> positions;
        positions["BBT"] = held("BBT", 10.0, 40.0);
        auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, kToday,
                                                            {{"BBT", "2005-03-09"}});
        ASSERT_EQ(log.size(), 1u);
        EXPECT_EQ(positions.count("TFC"), 1u) << "the 2005 holding belongs to the second era";
        EXPECT_EQ(positions.count("BBT1"), 0u);
    }
    {
        std::unordered_map<std::string, Position> positions;
        positions["BBT"] = held("BBT", 10.0, 55.0);
        auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, kToday,
                                                            {{"BBT", "2020-07-01"}});
        EXPECT_TRUE(log.empty()) << "later than every rename: the ticker belongs to who holds it now";
        EXPECT_EQ(positions.count("BBT"), 1u);
    }
}

TEST(RenameEras, ChainFollowsToTheEndInOnePass) {
    // A -> B (2010) -> C (2015). A holding from 2009 predates both, so it must land on C
    // in a single call, not stop halfway at B.
    std::unordered_map<std::string, Position> positions;
    positions["A"] = held("A", 7.0, 33.0);

    std::vector<TickerAlias> aliases = {{"A", "B", "2010-04-01", ""}, {"B", "C", "2015-09-30", ""}};

    auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, kToday,
                                                        {{"A", "2009-02-11"}});

    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0].contra_ticker, "B");
    EXPECT_EQ(log[1].contra_ticker, "C");
    EXPECT_EQ(positions.count("A"), 0u);
    EXPECT_EQ(positions.count("B"), 0u);
    ASSERT_EQ(positions.count("C"), 1u);
    EXPECT_DOUBLE_EQ(positions["C"].quantity.as_double(), 7.0);
    EXPECT_DOUBLE_EQ(positions["C"].average_price.as_double(), 33.0);
    EXPECT_EQ(positions["C"].symbol, "C");
}

// --------------------------------------------------------------------------
// Fail-narrow: when the era cannot be established, do nothing.
// --------------------------------------------------------------------------

TEST(RenameEras, MissingInceptionSkipsTheRenameRatherThanGuessing) {
    std::unordered_map<std::string, Position> positions;
    positions["META"] = held("META", 30.0, 512.40);

    std::vector<TickerAlias> aliases = {{"META", "METV", "2022-01-31", ""}};

    // Empty map: this is the shape the runner passes nothing at all in, and it stands in
    // for an unreadable inception read. Skipping is retried next run; a wrong re-key is
    // silent corruption.
    auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, kToday, {});

    EXPECT_TRUE(log.empty());
    EXPECT_EQ(positions.count("META"), 1u);
    EXPECT_EQ(positions.count("METV"), 0u);
}

TEST(RenameEras, EmptyInceptionStringIsAlsoTreatedAsUnknown) {
    std::unordered_map<std::string, Position> positions;
    positions["META"] = held("META", 30.0, 512.40);
    std::vector<TickerAlias> aliases = {{"META", "METV", "2022-01-31", ""}};

    auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, kToday,
                                                        {{"META", ""}});

    EXPECT_TRUE(log.empty());
    EXPECT_EQ(positions.count("META"), 1u);
}

TEST(RenameEras, AliasWithNoEffectiveUntilCannotBeEraBoundedAndIsSkipped) {
    // An open-ended alias has no era to test against. Applying it unconditionally is
    // precisely the shape that re-keys a currently-trading ticker, so it is dropped --
    // the same rule the dedup mirror applies.
    std::unordered_map<std::string, Position> positions;
    positions["OLD"] = held("OLD", 12.0, 9.0);
    std::vector<TickerAlias> aliases = {{"OLD", "NEW", "", "unbounded"}};

    auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, kToday,
                                                        {{"OLD", "2015-01-05"}});

    EXPECT_TRUE(log.empty());
    EXPECT_EQ(positions.count("OLD"), 1u);
    EXPECT_EQ(positions.count("NEW"), 0u);
}

TEST(RenameEras, RenameNotYetEffectiveIsStillLeftAloneEvenInsideItsEra) {
    // The as_of guard survives the rewrite: the position is in the right era, but the
    // rename has not happened yet as of this run.
    std::unordered_map<std::string, Position> positions;
    positions["BK"] = held("BK", 10.0, 55.0);
    std::vector<TickerAlias> aliases = {{"BK", "BNY", "2026-01-01", ""}};

    auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, "2025-12-15",
                                                        {{"BK", "2025-06-01"}});

    EXPECT_TRUE(log.empty());
    EXPECT_EQ(positions.count("BK"), 1u);
}

TEST(RenameEras, BoundaryDayErrsTowardNotApplying) {
    // effective_until conventions differ by a day between curated (day-after) and
    // backfilled (source-date) rows, so the compare is inclusive on purpose: on the
    // boundary the rename is deferred to the next run rather than possibly applied a
    // day early. Pinned so the direction is not silently flipped.
    std::unordered_map<std::string, Position> positions;
    positions["OLD"] = held("OLD", 12.0, 9.0);
    std::vector<TickerAlias> aliases = {{"OLD", "NEW", "2026-08-31", ""}};

    auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, "2026-08-31",
                                                        {{"OLD", "2015-01-05"}});

    EXPECT_TRUE(log.empty());
    EXPECT_EQ(positions.count("OLD"), 1u);
}

// --------------------------------------------------------------------------
// Class 3: the same stale-row hazard, one class over.
// --------------------------------------------------------------------------

TEST(DelistingStaleGuard, BarsAfterTheDelistingDateContradictIt) {
    // A reused ticker inherits the dead company's delisting row. Acting on it exits a
    // live position at a stale price.
    EXPECT_TRUE(CorporateActionsLifecycle::delisting_is_stale("2019-04-12", "2026-08-28"));
}

TEST(DelistingStaleGuard, ARealDelistingIsNotContradicted) {
    // No bars after the delisting -- which is what a genuine delisting looks like.
    EXPECT_FALSE(CorporateActionsLifecycle::delisting_is_stale("2026-04-09", "2026-04-09"));
    EXPECT_FALSE(CorporateActionsLifecycle::delisting_is_stale("2026-04-09", "2026-04-08"));
    // No bars loaded at all is not evidence of trading, so the termination stands.
    EXPECT_FALSE(CorporateActionsLifecycle::delisting_is_stale("2026-04-09", ""));
    EXPECT_FALSE(CorporateActionsLifecycle::delisting_is_stale("", "2026-08-28"));
}
