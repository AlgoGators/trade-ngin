#include <gtest/gtest.h>

#include "trade_ngin/live/pnl_manager_base.hpp"

using namespace trade_ngin;

// Review T2.11 / §F8: pin the invariant that an equity position's daily PnL equals
// the change in unrealized PnL plus realized PnL. The production daily-PnL formula
// (qty * (current_close - previous_close) * point_value) is only *coincidentally*
// correct for equities because point_value == 1.0. These tests fail if a future
// refactor drops/mishandles point_value, or computes the daily move from cost basis
// instead of the prior close.

namespace {

// A held position (no trades that day) -> realized == 0, so daily PnL must equal the
// day's change in unrealized PnL.
TEST(DailyPnlIdentity, EquityHeldPositionEqualsChangeInUnrealized) {
    PnLManagerBase pnl;
    const double qty = 100.0;
    const double avg_price = 95.0;    // cost basis
    const double prev_close = 100.0;  // T-2 close
    const double cur_close = 103.0;   // T-1 close
    const double point_value = 1.0;   // equities

    const double daily = pnl.calculate_daily_pnl(qty, prev_close, cur_close, point_value);

    const double unrealized_prev =
        pnl.calculate_position_pnl(qty, avg_price, prev_close, point_value);
    const double unrealized_cur =
        pnl.calculate_position_pnl(qty, avg_price, cur_close, point_value);
    const double realized = 0.0;  // position held all day

    EXPECT_DOUBLE_EQ(daily, (unrealized_cur - unrealized_prev) + realized);
    // The plain equity expectation: shares * price move.
    EXPECT_DOUBLE_EQ(daily, qty * (cur_close - prev_close));
}

// Short position: daily PnL is negative when price rises.
TEST(DailyPnlIdentity, EquityShortPositionSignIsCorrect) {
    PnLManagerBase pnl;
    const double qty = -50.0;
    const double prev_close = 200.0;
    const double cur_close = 210.0;
    const double daily = pnl.calculate_daily_pnl(qty, prev_close, cur_close, 1.0);
    EXPECT_DOUBLE_EQ(daily, -500.0);  // -50 * (210 - 200) * 1
}

// The identity must hold for point_value != 1.0 too -- this pins the *formula*, not
// the equity coincidence. A refactor that hardcodes point_value = 1 would fail here.
TEST(DailyPnlIdentity, HoldsForNonUnitPointValue) {
    PnLManagerBase pnl;
    const double qty = 3.0;
    const double avg_price = 4000.0;
    const double prev_close = 4100.0;
    const double cur_close = 4080.0;
    const double point_value = 50.0;  // non-equity multiplier

    const double daily = pnl.calculate_daily_pnl(qty, prev_close, cur_close, point_value);
    const double unrealized_prev =
        pnl.calculate_position_pnl(qty, avg_price, prev_close, point_value);
    const double unrealized_cur =
        pnl.calculate_position_pnl(qty, avg_price, cur_close, point_value);

    EXPECT_DOUBLE_EQ(daily, (unrealized_cur - unrealized_prev));
    EXPECT_DOUBLE_EQ(daily, qty * (cur_close - prev_close) * point_value);
}

}  // namespace
