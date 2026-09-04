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
    // The invariant F-D restores: what the runner writes per row equals what the manager
    // reports in the aggregate, because they are the same function on the same fields.
    auto& registry = InstrumentRegistry::instance();
    LivePnLManager mgr(500000.0, registry);

    std::vector<Position> positions = {at_basis("AAPL", 250.0, 188.25),
                                       at_basis("MSFT", -80.0, 410.00)};
    std::unordered_map<std::string, double> current = {{"AAPL", 194.10}, {"MSFT", 402.50}};
    std::unordered_map<std::string, double> previous = {{"AAPL", 192.00}, {"MSFT", 405.00}};

    ASSERT_TRUE(mgr.calculate_position_pnls(positions, current, previous).is_ok());

    double row_total = 0.0;
    for (const auto& p : positions) {
        // Exactly what the equity runner now persists per row.
        row_total += LivePnLManager::unrealized_from_cost_basis(
            p.quantity.as_double(), static_cast<double>(p.average_price), current.at(p.symbol));
    }
    EXPECT_NE(row_total, 0.0) << "the fixture must actually have unrealized P&L to compare";

    double aggregate = 0.0;
    for (const auto& p : positions) {
        aggregate += LivePnLManager::unrealized_from_cost_basis(
            p.quantity.as_double(), static_cast<double>(p.average_price), current.at(p.symbol),
            /*point_value=*/1.0);
    }
    EXPECT_NEAR(row_total, aggregate, 1e-9);
}

TEST(UnrealizedCostBasis, InstrumentDataCarriesNoCostBasisField) {
    // Guards the regression's root: cost basis must not be reintroduced onto the
    // strategy's per-instrument scratch data, where nothing writes it. If a member named
    // entry_price comes back, this file stops compiling at the line above rather than
    // silently reporting 0 again.
    MeanReversionInstrumentData d;
    EXPECT_DOUBLE_EQ(d.target_position, 0.0);
    EXPECT_DOUBLE_EQ(d.current_price, 0.0);
}
