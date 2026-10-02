// The trade statistics on STRATEGY rows with the tracker re-anchoring on ROLL legs (LOOP_SPEC v6.1
// section 6.5; T-ROLLX commit 3): a roll inside a round trip is not a trade, does not reset the
// holding clock, re-anchors the average at the opening leg's price so the later close realises the
// strategy's own move, and its cost lands in the roll total, never in a trade's P&L.
#include <gtest/gtest.h>
#include <chrono>
#include <vector>
#include "../core/test_base.hpp"
#include "trade_ngin/backtest/backtest_metrics_calculator.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {
Timestamp day(int d) {
    return std::chrono::system_clock::from_time_t(1761523200) + std::chrono::hours(24 * d);  // 2025-10-27 + d
}
ExecutionReport fill(const std::string& symbol, Side side, double qty, double price, int d, double cost,
                     ExecutionType type = ExecutionType::STRATEGY, const std::string& id = "") {
    ExecutionReport e;
    e.order_id = "O";
    e.exec_id = "E";
    e.symbol = symbol;
    e.side = side;
    e.filled_quantity = Quantity(qty);
    e.fill_price = Price(price);
    e.fill_time = day(d);
    e.commissions_fees = Decimal(cost);
    e.total_transaction_costs = Decimal(cost);
    e.execution_type = type;
    e.instrument_id = id;
    return e;
}
}  // namespace

TEST(TradeStatisticsRollLegs, ARollInsideARoundTripIsNotATradeAndReanchorsTheAverage) {
    BacktestMetricsCalculator calc;
    // Long 1 NG at 3.30 on day 0; the vendor switches on day 3 (3.376 -> 3.965); confirmed day 4:
    // closing leg at 3.376 (864), opening leg at 3.965 (863); the strategy sells on day 8 at 5.10.
    const std::vector<ExecutionReport> execs = {
        fill("NG.v.0", Side::BUY, 1.0, 3.30, 0, 1.0),
        fill("NG.v.0", Side::SELL, 1.0, 3.376, 4, 2.0, ExecutionType::ROLL, "864"),
        fill("NG.v.0", Side::BUY, 1.0, 3.965, 4, 2.0, ExecutionType::ROLL, "863"),
        fill("NG.v.0", Side::SELL, 1.0, 5.10, 8, 1.0),
    };
    const auto s = calc.calculate_trade_statistics(execs);
    EXPECT_EQ(s.total_trades, 1) << "one strategy round trip; the legs are not trades";
    EXPECT_EQ(s.roll_fills, 2);
    EXPECT_DOUBLE_EQ(s.roll_costs, 4.0);
    EXPECT_EQ(s.winning_trades, 1);
    // The close realises the move from the OPENING leg's price (the new contract), not from 3.30:
    // 5.10 - 3.965 = 1.135 in points, less the close's own cost of 1.0 (the tracker's trade P&L
    // charges the closing fill's cost; the opening fill's cost is charged on the symbol, below).
    EXPECT_NEAR(s.max_win, 1.135 - 1.0, 1e-12);
    EXPECT_NEAR(s.total_profit, 1.135 - 1.0, 1e-12);
    EXPECT_NEAR(s.avg_holding_period, 8.0, 1e-9) << "the clock runs from day 0: the roll did not restart it";
    // The symbol P&L charges every cost, the legs' included, and realises the strategy's move.
    const auto pnl = calc.calculate_symbol_pnl(execs);
    EXPECT_NEAR(pnl.at("NG.v.0"), 1.135 - 1.0 - 1.0 - 4.0, 1e-12);

    // The control: the same rows all STRATEGY (the parent's reading) score two trades and
    // realise the splice gap 3.376 - 3.30 as a trade (a loss of 1.924 after the two costs).
    auto untyped = execs;
    for (auto& e : untyped) e.execution_type = ExecutionType::STRATEGY;
    const auto c = calc.calculate_trade_statistics(untyped);
    EXPECT_EQ(c.total_trades, 2);
    EXPECT_EQ(c.roll_fills, 0);
}

TEST(TradeStatisticsRollLegs, ABorrowRowIsNeitherATradeNorALeg) {
    BacktestMetricsCalculator calc;
    const std::vector<ExecutionReport> execs = {
        fill("XYZ", Side::SELL, 10.0, 20.0, 0, 1.0),
        fill("XYZ", Side::SELL, 0.0, 20.0, 1, 0.3, ExecutionType::BORROW),
        fill("XYZ", Side::BUY, 10.0, 19.0, 2, 1.0),
    };
    const auto s = calc.calculate_trade_statistics(execs);
    EXPECT_EQ(s.total_trades, 1);
    EXPECT_EQ(s.roll_fills, 0);
}

// Code review D1 (T-ROLLX-FIX): two sleeves hold the symbol (BASE: TREND long 2, FAST long 1, one
// tracker per symbol over every sleeve's rows, 3 in all). Each sleeve's pair is stored closing,
// opening; the summed position never passes through 0 (3 -> 1 -> 3 -> 2 -> 3), and the opening legs
// still re-anchor the average, so the later close realises the strategy's move from the new
// contract's price, not the accumulated contract gap. The parent's rule (re-anchor only from flat)
// kept 3.30 and scored (5.10 - 3.30) x 3.
TEST(TradeStatisticsRollLegs, TwoSleevesHoldingTheSymbolStillReanchorOnTheOpeningLegs) {
    BacktestMetricsCalculator calc;
    const std::vector<ExecutionReport> execs = {
        fill("NG.v.0", Side::BUY, 2.0, 3.30, 0, 0.0),
        fill("NG.v.0", Side::BUY, 1.0, 3.30, 0, 0.0),
        fill("NG.v.0", Side::SELL, 2.0, 3.376, 4, 0.0, ExecutionType::ROLL, "864"),
        fill("NG.v.0", Side::BUY, 2.0, 3.965, 4, 0.0, ExecutionType::ROLL, "863"),
        fill("NG.v.0", Side::SELL, 1.0, 3.376, 4, 0.0, ExecutionType::ROLL, "864"),
        fill("NG.v.0", Side::BUY, 1.0, 3.965, 4, 0.0, ExecutionType::ROLL, "863"),
        fill("NG.v.0", Side::SELL, 3.0, 5.10, 8, 0.0),
    };
    const auto s = calc.calculate_trade_statistics(execs);
    EXPECT_EQ(s.total_trades, 1);
    EXPECT_EQ(s.roll_fills, 4);
    EXPECT_NEAR(s.total_profit, 3 * (5.10 - 3.965), 1e-12)
        << "the close realises the move from the opening legs' price, not from 3.30";
    const auto pnl = calc.calculate_symbol_pnl(execs);
    EXPECT_NEAR(pnl.at("NG.v.0"), 3 * (5.10 - 3.965), 1e-12);
}
