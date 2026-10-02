// The two ROLL fills of a held position on the confirming bar (LOOP_SPEC v6.1 section 6.5; T-ROLLX
// commit 3), built by roll_series::make_roll_legs: closing leg at the last pre-change close on the
// side opposite to the position, opening leg at the change bar's close with the position, both
// |q| contracts, each costed by the model, type ROLL, netting 0, the contract ids, the ids given;
// nothing for a flat position; a leg without a usable close is refused (STRICT).
#include <gtest/gtest.h>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>
#include "trade_ngin/data/roll_series.hpp"

using namespace trade_ngin;

namespace {
roll_series::RollLegCost cost_fn(const std::string&, double q, double px) {
    // fee 1.50 a contract, an implicit term of 0.001 x price a contract in dollars
    roll_series::RollLegCost c;
    c.commissions_fees = 1.5 * std::abs(q);
    c.implicit_price_impact = 0.001 * px;
    c.slippage_market_impact = 0.001 * px * std::abs(q);
    c.total_transaction_costs = c.commissions_fees + c.slippage_market_impact;
    return c;
}
const Timestamp kT = std::chrono::system_clock::from_time_t(1761609600);  // 2025-10-28 00:00 UTC
}  // namespace

TEST(RollLegs, ALongIsClosedAtTheLastPreChangeCloseAndReopenedAtTheChangeBarsClose) {
    const auto legs = roll_series::make_roll_legs("NG.v.0", 2.0, 3.376, 3.965, "864", "863", kT,
                                                  "EXEC_NG.v.0_20251028_RC", "ROLL_NG.v.0_20251028_RC",
                                                  "EXEC_NG.v.0_20251028_RO", "ROLL_NG.v.0_20251028_RO",
                                                  cost_fn);
    ASSERT_EQ(legs.size(), 2u);
    const auto& rc = legs[0];
    const auto& ro = legs[1];
    EXPECT_EQ(rc.execution_type, ExecutionType::ROLL);
    EXPECT_EQ(ro.execution_type, ExecutionType::ROLL);
    EXPECT_EQ(rc.side, Side::SELL) << "the closing leg sells the long";
    EXPECT_EQ(ro.side, Side::BUY) << "the opening leg re-buys it";
    EXPECT_DOUBLE_EQ(static_cast<double>(rc.filled_quantity), 2.0);
    EXPECT_DOUBLE_EQ(static_cast<double>(ro.filled_quantity), 2.0);
    EXPECT_DOUBLE_EQ(static_cast<double>(rc.fill_price), 3.376);
    EXPECT_DOUBLE_EQ(static_cast<double>(ro.fill_price), 3.965);
    EXPECT_EQ(rc.instrument_id, "864");
    EXPECT_EQ(ro.instrument_id, "863");
    EXPECT_EQ(rc.exec_id, "EXEC_NG.v.0_20251028_RC");
    EXPECT_EQ(ro.order_id, "ROLL_NG.v.0_20251028_RO");
    EXPECT_EQ(rc.fill_time, kT);
    EXPECT_DOUBLE_EQ(static_cast<double>(rc.commissions_fees), 3.0);
    EXPECT_DOUBLE_EQ(static_cast<double>(rc.total_transaction_costs), 3.0 + 0.001 * 3.376 * 2.0);
    EXPECT_DOUBLE_EQ(static_cast<double>(ro.total_transaction_costs), 3.0 + 0.001 * 3.965 * 2.0);
    EXPECT_DOUBLE_EQ(static_cast<double>(rc.netting_adjustment), 0.0);
    EXPECT_FALSE(rc.is_partial);
}

TEST(RollLegs, AShortIsBoughtBackAndResold) {
    const auto legs = roll_series::make_roll_legs("ES.v.0", -3.0, 5000.0, 5020.0, "1", "2", kT,
                                                  "c", "c", "o", "o", cost_fn);
    ASSERT_EQ(legs.size(), 2u);
    EXPECT_EQ(legs[0].side, Side::BUY);
    EXPECT_EQ(legs[1].side, Side::SELL);
    EXPECT_DOUBLE_EQ(static_cast<double>(legs[0].filled_quantity), 3.0);
}

TEST(RollLegs, NothingForAFlatPositionAndAMissingCloseIsRefused) {
    EXPECT_TRUE(roll_series::make_roll_legs("NG.v.0", 0.0, 3.0, 3.1, "a", "b", kT, "c", "c", "o", "o", cost_fn).empty());
    EXPECT_THROW(roll_series::make_roll_legs("NG.v.0", 1.0, 0.0, 3.1, "a", "b", kT, "c", "c", "o", "o", cost_fn),
                 std::invalid_argument);
    EXPECT_THROW(roll_series::make_roll_legs("NG.v.0", 1.0, 3.0, 0.0, "a", "b", kT, "c", "c", "o", "o", cost_fn),
                 std::invalid_argument);
}
