// tests/live/test_effective_universe.cpp
//
// E2-F34 / E3 F-4 -- the tradeable universe must be finalized AFTER the book is known,
// and BA-2 / C-3 D1 -- the era test that decides which renames count must be asked
// about the CURRENT holding, not the lifetime of the ticker.
//
// The failure shape these pin, end to end: the runner fixed `symbols` from config,
// registered instruments, loaded bars and built trading_params from it, and only then
// -- about 1,500 lines later -- ran apply_renames. A held position whose successor was
// not in config was re-keyed onto a symbol the run had no bars, no instrument, no cost
// config and no target for. The day-T pass logged "Missing T-1 price for symbol with a
// non-zero position", execute_day_t rule 3 rolled the target back to the carried
// quantity, and the next session did the same thing again: an unpriceable zombie,
// persisted under the new key, that re-running does not clear.
//
// LiveDailyCycle::effective_universe is the pure part of the reordering: given the book
// and the alias table, which extra tickers must this run be able to price?

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/live_daily_cycle.hpp"

using namespace trade_ngin;

namespace {

constexpr const char* kToday = "2026-09-03";

Position held(const std::string& symbol, double qty) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(100.0);
    p.last_update = std::chrono::system_clock::now();
    return p;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

// ---------------------------------------------------------------------------
// E2-F34 / F-4
// ---------------------------------------------------------------------------

// The regression. FB is held, FB -> META closed on 2022-06-09, the holding was
// established inside that era, and META is NOT configured. apply_renames WILL move
// the position onto META later in the run, so the run has to be able to price META.
TEST(EffectiveUniverse, SuccessorOfAHeldRenamedTickerJoinsTheUniverse) {
    const std::vector<std::string> config{"AAPL", "MSFT"};
    std::unordered_map<std::string, Position> book{{"FB", held("FB", 100.0)}};
    const std::vector<TickerAlias> aliases{{"FB", "META", "2022-06-09", "rename"}};
    const std::unordered_map<std::string, std::string> holding_start{{"FB", "2021-03-01"}};

    const auto universe =
        LiveDailyCycle::effective_universe(config, book, aliases, kToday, holding_start);

    EXPECT_TRUE(contains(universe, "META"))
        << "the successor apply_renames will re-key onto has no bars, no instrument and "
           "no target unless the universe carries it";
    EXPECT_TRUE(contains(universe, "AAPL"));
    EXPECT_TRUE(contains(universe, "MSFT"));
    EXPECT_EQ(universe.size(), 3u);
}

// Config order is preserved and additions are appended, so the universe is
// deterministic run to run (the symbol list drives query text and log ordering).
TEST(EffectiveUniverse, ConfigOrderIsPreservedAndAdditionsAppendedSorted) {
    const std::vector<std::string> config{"ZTS", "AAPL"};
    std::unordered_map<std::string, Position> book{{"FB", held("FB", 10.0)},
                                                   {"TWTR", held("TWTR", 5.0)}};
    const std::vector<TickerAlias> aliases{{"FB", "META", "2022-06-09", ""},
                                           {"TWTR", "X", "2023-07-24", ""}};
    const std::unordered_map<std::string, std::string> holding_start{{"FB", "2021-03-01"},
                                                                    {"TWTR", "2022-01-04"}};

    const auto universe =
        LiveDailyCycle::effective_universe(config, book, aliases, kToday, holding_start);

    ASSERT_EQ(universe.size(), 4u);
    EXPECT_EQ(universe[0], "ZTS");
    EXPECT_EQ(universe[1], "AAPL");
    EXPECT_EQ(universe[2], "META");
    EXPECT_EQ(universe[3], "X");
}

// An alias that maps a ticker we do not hold contributes nothing: the universe grows
// only for positions the run actually has to carry.
TEST(EffectiveUniverse, UnrelatedAliasAddsNothing) {
    const std::vector<std::string> config{"AAPL"};
    std::unordered_map<std::string, Position> book{{"AAPL", held("AAPL", 10.0)}};
    const std::vector<TickerAlias> aliases{{"FB", "META", "2022-06-09", ""}};
    const std::unordered_map<std::string, std::string> holding_start{{"AAPL", "2026-01-05"}};

    const auto universe =
        LiveDailyCycle::effective_universe(config, book, aliases, kToday, holding_start);

    EXPECT_EQ(universe, config);
}

// A flat row is not a holding. Closed rows exist to carry realized P&L on the day
// they closed (E2-F19) and nothing re-keys them, so they must not widen the universe.
TEST(EffectiveUniverse, AFlatRowDoesNotWidenTheUniverse) {
    const std::vector<std::string> config{"AAPL"};
    std::unordered_map<std::string, Position> book{{"FB", held("FB", 0.0)}};
    const std::vector<TickerAlias> aliases{{"FB", "META", "2022-06-09", ""}};
    const std::unordered_map<std::string, std::string> holding_start{{"FB", "2021-03-01"}};

    const auto universe =
        LiveDailyCycle::effective_universe(config, book, aliases, kToday, holding_start);

    EXPECT_EQ(universe, config);
}

// A successor already in config is not duplicated -- the symbol list is passed straight
// to get_market_data and to the per-symbol config loops.
TEST(EffectiveUniverse, AlreadyConfiguredSuccessorIsNotDuplicated) {
    const std::vector<std::string> config{"AAPL", "META"};
    std::unordered_map<std::string, Position> book{{"FB", held("FB", 100.0)}};
    const std::vector<TickerAlias> aliases{{"FB", "META", "2022-06-09", ""}};
    const std::unordered_map<std::string, std::string> holding_start{{"FB", "2021-03-01"}};

    const auto universe =
        LiveDailyCycle::effective_universe(config, book, aliases, kToday, holding_start);

    EXPECT_EQ(universe, config);
}

// The universe must not admit a rename apply_renames will refuse. Its as-of guard is
// `as_of_date > effective_until`; on or before the boundary the rename has not happened
// yet and the position keeps its own ticker.
TEST(EffectiveUniverse, ARenameThatHasNotHappenedYetIsNotInTheUniverse) {
    const std::vector<std::string> config{"AAPL"};
    std::unordered_map<std::string, Position> book{{"FB", held("FB", 100.0)}};
    const std::vector<TickerAlias> aliases{{"FB", "META", "2026-12-31", ""}};
    const std::unordered_map<std::string, std::string> holding_start{{"FB", "2021-03-01"}};

    const auto universe =
        LiveDailyCycle::effective_universe(config, book, aliases, kToday, holding_start);

    EXPECT_EQ(universe, config);

    // ... and the re-keying agrees, which is the property that matters: one rename map,
    // one era test, shared by both.
    auto positions = book;
    auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, kToday,
                                                        holding_start);
    EXPECT_TRUE(log.empty());
    EXPECT_EQ(positions.count("FB"), 1u);
}

// A chain A -> B -> C is followed at the same era, so every hop is priceable.
TEST(EffectiveUniverse, EveryHopOfARenameChainJoinsTheUniverse) {
    const std::vector<std::string> config{"AAPL"};
    std::unordered_map<std::string, Position> book{{"A", held("A", 10.0)}};
    const std::vector<TickerAlias> aliases{{"A", "B", "2023-01-31", ""},
                                           {"B", "C", "2024-01-31", ""}};
    const std::unordered_map<std::string, std::string> holding_start{{"A", "2022-06-01"}};

    const auto universe =
        LiveDailyCycle::effective_universe(config, book, aliases, kToday, holding_start);

    EXPECT_TRUE(contains(universe, "B"));
    EXPECT_TRUE(contains(universe, "C"));
    EXPECT_EQ(universe.size(), 3u);
}

// A symbol with no era date is skipped, not guessed at: class 2 fails narrow, and so
// must the universe derived from it, or the run would load bars for a rename that is
// never applied.
TEST(EffectiveUniverse, NoHoldingStartMeansNoAddition) {
    const std::vector<std::string> config{"AAPL"};
    std::unordered_map<std::string, Position> book{{"FB", held("FB", 100.0)}};
    const std::vector<TickerAlias> aliases{{"FB", "META", "2022-06-09", ""}};

    const auto universe = LiveDailyCycle::effective_universe(config, book, aliases, kToday, {});

    EXPECT_EQ(universe, config);
}

// ---------------------------------------------------------------------------
// BA-2 / C-3 D1 -- which date the era test is asked about
// ---------------------------------------------------------------------------

// The two inputs, side by side, on one book. `get_position_inception_dates` answers
// min(date) over ALL history and is deliberately fail-wide for the class-1 price
// window; feeding that answer to class 2 re-keys a live holding onto a dead symbol.
// `get_current_holding_start_dates` answers "when did the holding we hold now begin",
// and the same alias then correctly does nothing.
//
// The live shape this is drawn from, measured 2026-09-03 on EQUITY_MR_PORTFOLIO:
// META's lifetime inception is 2026-06-11, its current holding began 2026-07-31 (the
// position was closed and re-opened in between).
TEST(RenameEraInput, LifetimeInceptionRekeysAReopenedTickerAndTheHoldingStartDoesNot) {
    const std::vector<TickerAlias> aliases{{"META", "METV", "2022-01-31", "backfill"}};

    // Held 2021-11-01 .. 2021-12-15 (Facebook), closed, re-bought 2026-08-20 (Meta
    // Platforms) and still held.
    const std::unordered_map<std::string, std::string> lifetime{{"META", "2021-11-01"}};
    const std::unordered_map<std::string, std::string> current_holding{{"META", "2026-08-20"}};

    {
        std::unordered_map<std::string, Position> positions{{"META", held("META", 100.0)}};
        auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, kToday, lifetime);
        ASSERT_EQ(log.size(), 1u) << "characterizing the defect: the lifetime date is inside "
                                     "the alias era, so the rename fires";
        EXPECT_EQ(positions.count("META"), 0u);
        EXPECT_EQ(positions.count("METV"), 1u)
            << "a live 100-share holding was moved onto a symbol with no bars";
    }
    {
        std::unordered_map<std::string, Position> positions{{"META", held("META", 100.0)}};
        auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, kToday,
                                                            current_holding);
        EXPECT_TRUE(log.empty());
        ASSERT_EQ(positions.count("META"), 1u)
            << "the holding we actually hold began four years after the alias era closed";
        EXPECT_EQ(positions.count("METV"), 0u);
        EXPECT_DOUBLE_EQ(positions.at("META").quantity.as_double(), 100.0);
    }
}

// The same distinction, seen from the universe: class 1's answer makes the run load
// bars for a dead ticker; class 2's answer leaves the universe alone.
TEST(RenameEraInput, LifetimeInceptionWidensTheUniverseOntoADeadTicker) {
    const std::vector<std::string> config{"META"};
    std::unordered_map<std::string, Position> book{{"META", held("META", 100.0)}};
    const std::vector<TickerAlias> aliases{{"META", "METV", "2022-01-31", "backfill"}};

    const auto with_lifetime = LiveDailyCycle::effective_universe(
        config, book, aliases, kToday, {{"META", "2021-11-01"}});
    EXPECT_TRUE(contains(with_lifetime, "METV"))
        << "characterizing the defect: class 1's fail-wide date drags a dead symbol in";

    const auto with_holding_start = LiveDailyCycle::effective_universe(
        config, book, aliases, kToday, {{"META", "2026-08-20"}});
    EXPECT_EQ(with_holding_start, config);
}
