// The trade statistics on STRATEGY rows with the tracker carrying the entry price across ROLL legs
// (LOOP_SPEC v6.2 section 6.5): a roll inside a round trip is not a trade, does not reset the holding
// clock, moves the open entry price by the leg gap (opening leg price - closing leg price) so the
// later close scores the move of the contracts actually held, and its cost lands in the roll total,
// never in a trade's P&L.
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

TEST(TradeStatisticsRollLegs, ARollInsideARoundTripIsNotATradeAndCarriesTheEntry) {
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
    // The close scores the held contracts' whole move: 3.30 -> 3.376 in the old contract (0.076)
    // plus 3.965 -> 5.10 in the new one (1.135) = 1.211 in points, the entry carried to
    // 3.30 + (3.965 - 3.376) = 3.889; less the close's own cost of 1.0 (the tracker's trade P&L
    // charges the closing fill's cost; the opening fill's cost is charged on the symbol, below).
    // Re-anchoring on the opening leg (the rule before) scored 1.135 and dropped the 0.076.
    EXPECT_NEAR(s.max_win, 1.211 - 1.0, 1e-12);
    EXPECT_NEAR(s.total_profit, 1.211 - 1.0, 1e-12);
    EXPECT_NEAR(s.avg_holding_period, 8.0, 1e-9) << "the clock runs from day 0: the roll did not restart it";
    // The symbol P&L charges every cost, the legs' included, and scores the same move.
    const auto pnl = calc.calculate_symbol_pnl(execs);
    EXPECT_NEAR(pnl.at("NG.v.0"), 1.211 - 1.0 - 1.0 - 4.0, 1e-12);

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
// opening; the summed position never passes through 0 (3 -> 1 -> 3 -> 2 -> 3), and the roll's gap is
// carried into the entry ONCE (one tracker per symbol: the second sleeve's pair is the same roll),
// so the later close scores the held contracts' move, 3 x 1.211. Carrying it per pair would score
// 3 x (5.10 - 3.30 - 2 x 0.589); the untyped reading kept 3.30 and scored (5.10 - 3.30) x 3.
TEST(TradeStatisticsRollLegs, TwoSleevesHoldingTheSymbolCarryTheRollOnce) {
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
    EXPECT_NEAR(s.total_profit, 3 * 1.211, 1e-12)
        << "the close scores 3.30 -> 3.376 in the old contract plus 3.965 -> 5.10 in the new one";
    const auto pnl = calc.calculate_symbol_pnl(execs);
    EXPECT_NEAR(pnl.at("NG.v.0"), 3 * 1.211, 1e-12);
}

// TS (T-ROLLX-FIX commit 4; LOOP_SPEC v6.2 section 6.5): ONE trade held through TWO rolls scores
// exactly the held contracts' move. Long 2 XA (multiplier-free: points), costs 0:
//   entry 100.0 in contract A1;
//   roll 1: closing leg 104.0 (A1), opening leg 110.0 (A2): A1 moved 104 - 100 = +4;
//   roll 2: closing leg 113.0 (A2), opening leg 109.0 (A3): A2 moved 113 - 110 = +3;
//   exit 115.0 in A3:                                        A3 moved 115 - 109 = +6.
// The held contracts moved 4 + 3 + 6 = 13 points a contract, 26.0 for the 2 held; the entry is
// carried 100 + (110 - 104) + (109 - 113) = 102, and 115 - 102 = 13. Re-anchoring on the last
// opening leg scored 115 - 109 = 6 a contract (12.0) and dropped the two earlier contracts' moves.
TEST(TradeStatisticsRollLegs, OneTradeHeldThroughTwoRollsScoresTheHeldContractsMove) {
    BacktestMetricsCalculator calc;
    for (const bool is_long : {true, false}) {
        const Side open = is_long ? Side::BUY : Side::SELL;
        const Side close = is_long ? Side::SELL : Side::BUY;
        const std::vector<ExecutionReport> execs = {
            fill("XA.v.0", open, 2.0, 100.0, 0, 0.0),
            fill("XA.v.0", close, 2.0, 104.0, 5, 0.0, ExecutionType::ROLL, "A1"),
            fill("XA.v.0", open, 2.0, 110.0, 5, 0.0, ExecutionType::ROLL, "A2"),
            fill("XA.v.0", close, 2.0, 113.0, 11, 0.0, ExecutionType::ROLL, "A2"),
            fill("XA.v.0", open, 2.0, 109.0, 11, 0.0, ExecutionType::ROLL, "A3"),
            fill("XA.v.0", close, 2.0, 115.0, 20, 0.0),
        };
        const auto s = calc.calculate_trade_statistics(execs);
        EXPECT_EQ(s.total_trades, 1) << "one round trip; four legs are not trades";
        EXPECT_EQ(s.roll_fills, 4);
        EXPECT_NEAR(s.avg_holding_period, 20.0, 1e-9) << "the open time is kept across both rolls";
        const double scored = is_long ? s.total_profit : -s.total_loss;
        EXPECT_NEAR(scored, (is_long ? 1.0 : -1.0) * 2.0 * (4.0 + 3.0 + 6.0), 1e-12)
            << (is_long ? "long" : "short") << ": the held contracts' move, 2 x 13 points";
        EXPECT_EQ(s.winning_trades, is_long ? 1 : 0);
        const auto pnl = calc.calculate_symbol_pnl(execs);
        EXPECT_NEAR(pnl.at("XA.v.0"), (is_long ? 26.0 : -26.0), 1e-12);
    }
}

// A roll confirmed while the tracker is flat carries nothing into the next trade: its entry is its
// own fill price.
TEST(TradeStatisticsRollLegs, ARollWhileFlatLeavesTheNextEntryAlone) {
    BacktestMetricsCalculator calc;
    const std::vector<ExecutionReport> execs = {
        fill("XA.v.0", Side::BUY, 1.0, 100.0, 0, 0.0),
        fill("XA.v.0", Side::SELL, 1.0, 101.0, 2, 0.0),
        fill("XA.v.0", Side::BUY, 1.0, 108.0, 9, 0.0),
        fill("XA.v.0", Side::SELL, 1.0, 110.0, 12, 0.0),
    };
    auto with_roll = execs;
    with_roll.insert(with_roll.begin() + 2, fill("XA.v.0", Side::BUY, 1.0, 107.0, 5, 0.0,
                                                 ExecutionType::ROLL, "A2"));
    with_roll.insert(with_roll.begin() + 2, fill("XA.v.0", Side::SELL, 1.0, 102.0, 5, 0.0,
                                                 ExecutionType::ROLL, "A1"));
    const auto a = calc.calculate_trade_statistics(execs);
    const auto b = calc.calculate_trade_statistics(with_roll);
    EXPECT_EQ(b.total_trades, 2);
    EXPECT_NEAR(b.total_profit, a.total_profit, 1e-12);
    EXPECT_NEAR(b.total_profit, 1.0 + 2.0, 1e-12);
}
