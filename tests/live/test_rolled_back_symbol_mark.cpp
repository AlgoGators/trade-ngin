// B-iv regression pins -- a holding that could not be priced must be MARKED at the level it
// was carried at, so the row and the aggregate agree and the day still writes.
//
// THE DEFECT: execute_day_t rule 3 rolls an unpriceable holding back to its T-1 row
// (live_daily_cycle.hpp:389-397) with `positions[symbol] = prev_it->second`, overwriting
// only last_update. That copy carries the T-1 row's stored `unrealized_pnl` -- a LEVEL.
//
// The day-T aggregate is summed over `positions`, so it includes that carried level. But
// the day-T ROW is re-marked from `day_t_mark_prices`, and a symbol with no close within
// the staleness bound has no entry in either `t1_closes` or `execution_prices`, so its row
// is written with unrealized 0.
//
// Row says 0, aggregate says -402.65, and 3dc65a62's in-run unrealized identity is FATAL at
// 1e-4 on a trading day -- so the run exits 1 and the day is not written at all. The guard
// that exists to catch a marking disagreement is triggered by the one case where the book is
// behaving correctly: a symbol that genuinely did not trade.
//
// THE FIX: on rollback, carry the T-1 row's implied mark forward alongside the row, so the
// row and the aggregate are computed from the same level. This is the same policy R-2
// settled for the finalizer ("carry the last mark for an unprinted symbol"), applied to the
// day-T side. The mark is recovered from the row itself rather than from a price series --
// there is no price series, which is why the symbol was rolled back.
//
//     unrealized = qty * (mark - basis)   =>   mark = basis + unrealized / qty
//
// Run R4 is the end-to-end evidence: a real delisted symbol, seeded held, run past the end
// of its bars.

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>

#include "trade_ngin/live/live_daily_cycle.hpp"

using namespace trade_ngin;

namespace {
Position carried_row(double qty, double basis, double unrealized) {
    Position p;
    p.symbol = "TMUS";
    p.quantity = Decimal(qty);
    p.average_price = Decimal(basis);
    p.unrealized_pnl = Decimal(unrealized);
    return p;
}
}  // namespace

TEST(RolledBackSymbolMark, TheImpliedMarkIsRecoveredFromTheRow) {
    // 17.6 sh, basis 150.00, row carries -402.654413 of unrealized.
    // mark = 150 + (-402.654413 / 17.6) = 127.121908...
    const auto p = carried_row(17.6, 150.0, -402.654413);
    const double mark = LiveDailyCycle::carried_mark_from_row(p);
    EXPECT_NEAR(mark, 150.0 + (-402.654413 / 17.6), 1e-9);

    // And it round-trips: marking the row at that level reproduces the carried unrealized,
    // which is exactly what makes the row and the aggregate agree.
    EXPECT_NEAR(17.6 * (mark - 150.0), -402.654413, 1e-9);
}

TEST(RolledBackSymbolMark, AFlatOrBasislessRowYieldsNoMark) {
    // Nothing to imply a mark from. 0 means "absent", and day_t_mark_prices already
    // ignores non-positive substitutes -- absent is better than a zero mark, which would
    // book the whole notional as a gain.
    EXPECT_DOUBLE_EQ(LiveDailyCycle::carried_mark_from_row(carried_row(0.0, 150.0, 0.0)), 0.0);
    EXPECT_DOUBLE_EQ(LiveDailyCycle::carried_mark_from_row(carried_row(17.6, 0.0, -10.0)), 0.0);
}

TEST(RolledBackSymbolMark, ACarriedMarkOverlaysTheDayTMarkMap) {
    // t1_closes has the nine symbols that printed; TMUS is absent because it has no close
    // within the staleness bound. The carried mark must reach the mark map, or the row is
    // written at 0 while the aggregate carries the level.
    const std::unordered_map<std::string, double> t1 = {{"AAPL", 190.0}};
    const std::unordered_map<std::string, double> exec_prices = {{"AAPL", 190.0}};
    const std::unordered_map<std::string, double> carried = {{"TMUS", 127.121908}};

    const auto marks = LiveDailyCycle::day_t_mark_prices(t1, exec_prices, carried);

    ASSERT_EQ(marks.count("TMUS"), 1u) << "the rolled-back symbol must have a mark";
    EXPECT_DOUBLE_EQ(marks.at("TMUS"), 127.121908);
    EXPECT_DOUBLE_EQ(marks.at("AAPL"), 190.0) << "priced symbols are untouched";
}

TEST(RolledBackSymbolMark, ARealExecutionPriceStillBeatsACarriedMark) {
    // A carried mark is a fallback for a symbol that did NOT trade. If a price exists for
    // the symbol, that price wins -- marking and fills must come from the same number.
    const std::unordered_map<std::string, double> t1 = {{"AAPL", 190.0}};
    const std::unordered_map<std::string, double> exec_prices = {{"AAPL", 191.5}};
    const std::unordered_map<std::string, double> carried = {{"AAPL", 100.0}};

    const auto marks = LiveDailyCycle::day_t_mark_prices(t1, exec_prices, carried);
    EXPECT_DOUBLE_EQ(marks.at("AAPL"), 191.5);
}

TEST(RolledBackSymbolMark, TheClosedDayPathIsUntouched) {
    // Empty execution_prices is the carry-forward day, where execute_day_t never runs and
    // there are no rolled-back symbols. t1_closes must come back unchanged.
    const std::unordered_map<std::string, double> t1 = {{"AAPL", 190.0}, {"MSFT", 410.0}};
    const auto marks = LiveDailyCycle::day_t_mark_prices(t1, {}, {});
    EXPECT_EQ(marks.size(), 2u);
    EXPECT_DOUBLE_EQ(marks.at("AAPL"), 190.0);
    EXPECT_DOUBLE_EQ(marks.at("MSFT"), 410.0);
}
