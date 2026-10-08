// tests/live/test_unrealized_cost_basis.cpp
//
// F-D: per-row trading.positions.unrealized_pnl and the live_results aggregate must be
// the same measurement.
//
// They were not. The aggregate came from Position::average_price, which on_execution
// maintains and the corp-action applier adjusts. The persisted row came from
// MeanReversionInstrumentData::entry_price -- a field with NO writer anywhere in the
// tree, so it was always 0.0, the guard `entry_price > 0.0` never passed, and every row
// was written with unrealized_pnl = 0 while the aggregate reported something else. A
// guaranteed log-versus-DB mismatch on any day with open positions.
//
// Both sides now go through unrealized_from_cost_basis(), and the dead field is gone.

#include <gtest/gtest.h>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

using namespace trade_ngin;

namespace {

Position at_basis(const std::string& symbol, double qty, double avg_price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(avg_price);
    p.unrealized_pnl = Decimal(0.0);
    p.realized_pnl = Decimal(0.0);
    p.last_update = std::chrono::system_clock::now();
    return p;
}

}  // namespace

TEST(UnrealizedCostBasis, LongPositionMarkedAboveBasisShowsAGain) {
    // 100 sh bought at 40, marked at 45.
    EXPECT_NEAR(LivePnLManager::unrealized_from_cost_basis(100.0, 40.0, 45.0), 500.0, 1e-9);
}

TEST(UnrealizedCostBasis, ShortPositionMarkedAboveBasisShowsALoss) {
    EXPECT_NEAR(LivePnLManager::unrealized_from_cost_basis(-100.0, 40.0, 45.0), -500.0, 1e-9);
}

TEST(UnrealizedCostBasis, PointValueScalesTheResultForFutures) {
    EXPECT_NEAR(LivePnLManager::unrealized_from_cost_basis(2.0, 4000.0, 4010.0, 50.0), 1000.0,
                1e-9);
}

TEST(UnrealizedCostBasis, NoBasisYetMeasuresNothingRatherThanTheWholeNotional) {
    // average_price is 0 until on_execution processes the fill. Measuring against 0
    // would report the entire notional as a gain.
    EXPECT_NEAR(LivePnLManager::unrealized_from_cost_basis(100.0, 0.0, 45.0), 0.0, 1e-9);
    EXPECT_NEAR(LivePnLManager::unrealized_from_cost_basis(0.0, 40.0, 45.0), 0.0, 1e-9);
}

TEST(UnrealizedCostBasis, MarkPriceIsTheCallersToScreen) {
    // The helper does not screen mark_price: this manager skips positions with no price,
    // while the equity runner defaults a missing close to 0.0 and checks it itself.
    // Folding the guard in here would have changed what the futures path reports for a
    // present-but-zero mark, so it is pinned out.
    EXPECT_NEAR(LivePnLManager::unrealized_from_cost_basis(100.0, 40.0, 0.0), -4000.0, 1e-9);
}

TEST(UnrealizedCostBasis, AHeldPositionDoesNotMeasureAsZero) {
    // The defect in one line: with a real basis and a real mark, the persisted row must
    // carry a nonzero figure. Pre-F-D it was always exactly 0.
    const double v = LivePnLManager::unrealized_from_cost_basis(250.0, 188.25, 194.10);
    EXPECT_NE(v, 0.0);
    EXPECT_NEAR(v, 250.0 * (194.10 - 188.25), 1e-9);
}

TEST(UnrealizedCostBasis, RowAndAggregateAgreeOnTheSameInputs) {
    // The invariant F-D restores: what the runner writes per row equals what the MANAGER
    // reports in its aggregate.
    //
    // BA-3 / C-3 D2(b): this test used to accumulate BOTH sides from
    // unrealized_from_cost_basis itself, differing only by point_value defaulted versus
    // passed as 1.0. That is a tautology -- it compared the helper to itself and never
    // read a single manager output, so it would have passed even if the aggregate had
    // stopped using the helper entirely, which is the exact drift the commit claims to
    // prevent. It now reads get_current_snapshot(), and pins both sides against an
    // independently computed figure so they cannot drift TOGETHER either.
    auto& registry = InstrumentRegistry::instance();
    LivePnLManager mgr(500000.0, registry);
    // An equity book: one share is one unit. Stated explicitly because the manager's
    // default is FUTURE, where an unregistered AAPL would pick up a contract multiplier.
    mgr.set_asset_type(AssetType::EQUITY);

    std::vector<Position> positions = {at_basis("AAPL", 250.0, 188.25),
                                       at_basis("MSFT", -80.0, 410.00)};
    std::unordered_map<std::string, double> current = {{"AAPL", 194.10}, {"MSFT", 402.50}};
    std::unordered_map<std::string, double> previous = {{"AAPL", 192.00}, {"MSFT", 405.00}};

    ASSERT_TRUE(mgr.calculate_position_pnls(positions, current, previous).is_ok());

    // Computed here from the position fields alone, with no reference to the code under
    // test. If the row rule and the aggregate rule both changed in the same direction,
    // this is what still catches it.
    const double expected = 250.0 * (194.10 - 188.25) + (-80.0) * (402.50 - 410.00);
    ASSERT_NE(expected, 0.0) << "the fixture must have real unrealized P&L to compare";

    // Side 1: the manager's OWN aggregate, the number live_results carries.
    auto snapshot = mgr.get_current_snapshot();
    ASSERT_TRUE(snapshot.is_ok());
    const double aggregate = snapshot.value().unrealized_pnl;

    // Side 2: what the equity runner persists per row, at the point value the manager
    // itself resolved for each symbol -- not a hard-coded 1.0 standing in for it.
    double row_total = 0.0;
    for (const auto& p : positions) {
        row_total += LivePnLManager::unrealized_from_cost_basis(
            p.quantity.as_double(), static_cast<double>(p.average_price),
            current.at(p.symbol), mgr.get_point_value(p.symbol));
    }

    EXPECT_NEAR(aggregate, expected, 1e-9)
        << "the manager's aggregate must be the cost-basis measurement, not something else";
    EXPECT_NEAR(row_total, expected, 1e-9) << "so must the per-row figure";
    EXPECT_NEAR(aggregate, row_total, 1e-9)
        << "row and aggregate are the same measurement on the same inputs (F-D)";
}

// BA-3 / C-3 D2(a): an ACTUAL compile-time pin on the struct's shape.
//
// This test used to claim "if a member named entry_price comes back, this file stops
// compiling at the line above" while its body only asserted that two unrelated members
// were 0.0. Nothing referenced entry_price, so nothing could stop compiling: restoring
// the field AND reverting the runner to the always-zero persistence left the file
// building and the suite green. The claim is now enforced by a detector rather than
// asserted in a comment.
namespace {

template <typename T, typename = void>
struct has_entry_price : std::false_type {};

template <typename T>
struct has_entry_price<T, std::void_t<decltype(std::declval<T&>().entry_price)>>
    : std::true_type {};

// The pin. Reintroducing MeanReversionInstrumentData::entry_price fails the BUILD here,
// which is the only place that can catch it before it silently reports 0 again: the
// field had no writer anywhere in the tree, so every persisted row read 0 while the
// aggregate reported otherwise.
static_assert(!has_entry_price<MeanReversionInstrumentData>::value,
              "MeanReversionInstrumentData::entry_price is back. It is a cost-basis field "
              "on per-instrument scratch data with no writer, which is what made every "
              "persisted trading.positions.unrealized_pnl 0 while live_results disagreed "
              "(F-D). Cost basis lives on Position::average_price, whose sole writer is "
              "BaseStrategy::on_execution -- see docs/AVERAGE_PRICE_LIFECYCLE.md.");

}  // namespace

TEST(UnrealizedCostBasis, InstrumentDataCarriesNoCostBasisField) {
    // The static_assert above is the real guard; this makes the intent visible in the
    // test log and fails loudly rather than only at build time.
    EXPECT_FALSE(has_entry_price<MeanReversionInstrumentData>::value)
        << "cost basis must not be reintroduced onto the strategy's scratch data";

    // A cost basis has exactly one home, and it is not here.
    EXPECT_TRUE(has_entry_price<Position>::value == false)
        << "Position carries average_price, never entry_price";

    MeanReversionInstrumentData d;
    EXPECT_DOUBLE_EQ(d.target_position, 0.0);
    EXPECT_DOUBLE_EQ(d.current_price, 0.0);
}
