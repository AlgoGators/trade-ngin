// tests/live/test_realized_row_retention.cpp
//
// E2-F19 route 3 and gap G1: which trading.positions rows survive a write.
//
// A position closed to zero used to write NO row, so the realized P&L of the exit --
// the largest single figure of an equity position's life -- had nowhere to live at
// row level. The rule was copied from futures, where an exit realizes exactly zero
// (average_price is reset to close(T-1) daily and fills strike at close(T-1)), so
// dropping the row there discards a row that is genuinely empty. Equities carry a
// true cost basis and the exit realizes the whole accumulated gain.
//
// Measured 2026-04-15: TMUS sold out for -402.654413; live_results carried it, the
// positions table did not, residual 436.64 for the day. Eleven such close events in
// the 2026-04-01..08-04 series.
//
// These tests pin the pure rules the runner applies at both write sites, plus the
// load-time partition that keeps closed rows out of every other consumer.

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/live_daily_cycle.hpp"

using namespace trade_ngin;

namespace {

Position row(const std::string& symbol, double qty, double realized, double basis = 100.0) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(basis);
    p.unrealized_pnl = Decimal(0.0);
    p.realized_pnl = Decimal(realized);
    p.last_update = std::chrono::system_clock::now();
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// is_dead_row: drop a row only when it carries neither quantity nor realized.
// ---------------------------------------------------------------------------

// The futures exit and the persistent-flat case: identical to today.
TEST(RealizedRowRetention, FlatRowWithNoRealizedIsDead) {
    EXPECT_TRUE(LiveDailyCycle::is_dead_row(row("MNQ.v.0", 0.0, 0.0)));
}

// The AAPL/TMUS close-day case: the row that used to be dropped.
TEST(RealizedRowRetention, ClosedRowCarryingRealizedIsKept) {
    EXPECT_FALSE(LiveDailyCycle::is_dead_row(row("TMUS", 0.0, -402.654413)))
        << "an equity exit realizes its whole accumulated gain; the row that carries "
           "it must survive the write (E2-F19 route 3)";
}

// A held position that did not trade today: kept, as today.
TEST(RealizedRowRetention, HeldRowWithNoRealizedIsKept) {
    EXPECT_FALSE(LiveDailyCycle::is_dead_row(row("TMUS", 20.844513, 0.0)));
}

// Tolerance is per field, not on the sum. Decimal is fixed-point at 1e-8, so a
// value below that rounds to exactly zero before the predicate ever sees it; the
// smallest representable non-zero figure must keep the row on its own.
TEST(RealizedRowRetention, ToleranceIsPerField) {
    EXPECT_TRUE(LiveDailyCycle::is_dead_row(row("X", 1e-11, 1e-11)))
        << "below Decimal resolution both fields are exactly zero";
    EXPECT_FALSE(LiveDailyCycle::is_dead_row(row("X", 0.0, 1e-7)))
        << "a realized figure above tolerance keeps the row even at zero quantity";
    EXPECT_FALSE(LiveDailyCycle::is_dead_row(row("X", 1e-7, 0.0)));
}

// ---------------------------------------------------------------------------
// split_open_and_closed: the load-time partition.
// ---------------------------------------------------------------------------

TEST(RealizedRowRetention, PartitionSeparatesClosedRowsFromTheHeldBook) {
    std::unordered_map<std::string, Position> loaded{
        {"DD", row("DD", 7.5738, -15.4473)},
        {"AAPL", row("AAPL", 0.0, -327.342)},
        {"META", row("META", 4.148866, 0.0)},
    };
    std::unordered_map<std::string, Position> open, closed;
    LiveDailyCycle::split_open_and_closed(loaded, open, closed);

    ASSERT_EQ(open.size(), 2u);
    ASSERT_EQ(closed.size(), 1u);
    EXPECT_TRUE(open.count("DD"));
    EXPECT_TRUE(open.count("META"));
    ASSERT_TRUE(closed.count("AAPL"));
    EXPECT_DOUBLE_EQ(closed.at("AAPL").realized_pnl.as_double(), -327.342)
        << "a closed row is carried verbatim";
}

// A book with no closed rows -- today's data, every day -- is returned unchanged.
TEST(RealizedRowRetention, PartitionIsIdentityOnAnOpenOnlyBook) {
    std::unordered_map<std::string, Position> loaded{
        {"DD", row("DD", 7.5738, -15.4473)},
        {"META", row("META", 4.148866, 73.351951)},
    };
    std::unordered_map<std::string, Position> open, closed;
    LiveDailyCycle::split_open_and_closed(loaded, open, closed);
    EXPECT_EQ(open.size(), 2u);
    EXPECT_TRUE(closed.empty());
}

// ---------------------------------------------------------------------------
// add_rowless_exits (G1): a symbol the strategy closed out that has no entry in
// the day-T target map. Reachable when a held name leaves the configured universe
// (contra-merge, rename, de-configuration): generate_daily_executions emits the
// close-out, on_execution books the realized, and nothing iterates it into a row.
// ---------------------------------------------------------------------------

TEST(RealizedRowRetention, StrategyExitWithoutATargetEntryGetsAClosedRow) {
    const Timestamp now = std::chrono::system_clock::now();
    std::unordered_map<std::string, Position> positions{{"DD", row("DD", 7.5738, 0.0)}};
    std::unordered_map<std::string, Position> strategy{
        {"DD", row("DD", 7.5738, 0.0)},
        {"MSFT", row("MSFT", 0.0, 100.0)},  // closed out today, absent from targets
    };

    auto added = LiveDailyCycle::add_rowless_exits(positions, strategy, now);

    ASSERT_EQ(added.size(), 1u) << "exactly one rowless exit must be reported";
    EXPECT_EQ(added.front(), "MSFT");
    ASSERT_TRUE(positions.count("MSFT")) << "the exit must now have a row to live on";
    const auto& synthesized = positions.at("MSFT");
    EXPECT_DOUBLE_EQ(synthesized.quantity.as_double(), 0.0);
    EXPECT_DOUBLE_EQ(synthesized.average_price.as_double(), 0.0)
        << "a closed row records no basis: average_price is a cost basis for a "
           "position that no longer exists (AVERAGE_PRICE_LIFECYCLE.md)";
    EXPECT_DOUBLE_EQ(synthesized.unrealized_pnl.as_double(), 0.0);
    EXPECT_DOUBLE_EQ(synthesized.realized_pnl.as_double(), 100.0);
    EXPECT_EQ(synthesized.last_update, now);
    EXPECT_FALSE(LiveDailyCycle::is_dead_row(synthesized));
}

// A symbol already in the target map is the runner's business, not this helper's.
TEST(RealizedRowRetention, TargetEntryIsNeverOverwritten) {
    const Timestamp now = std::chrono::system_clock::now();
    std::unordered_map<std::string, Position> positions{{"DD", row("DD", 0.0, -15.0)}};
    std::unordered_map<std::string, Position> strategy{{"DD", row("DD", 0.0, -15.0)}};

    auto added = LiveDailyCycle::add_rowless_exits(positions, strategy, now);

    EXPECT_TRUE(added.empty());
    EXPECT_DOUBLE_EQ(positions.at("DD").realized_pnl.as_double(), -15.0);
}

// A strategy symbol with nothing realized and no target entry adds nothing:
// the configured universe carries flat symbols the strategy also knows about.
TEST(RealizedRowRetention, FlatStrategySymbolAddsNoRow) {
    const Timestamp now = std::chrono::system_clock::now();
    std::unordered_map<std::string, Position> positions;
    std::unordered_map<std::string, Position> strategy{{"MSFT", row("MSFT", 0.0, 0.0)}};

    auto added = LiveDailyCycle::add_rowless_exits(positions, strategy, now);

    EXPECT_TRUE(added.empty());
    EXPECT_TRUE(positions.empty());
}
