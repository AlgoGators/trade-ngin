// The trade statistics on STRATEGY rows with the tracker carrying the entry price across ROLL legs
// (LOOP_SPEC v6.2 section 6.5): a roll inside a round trip is not a trade, does not reset the holding
// clock, moves the open entry price by the leg gap (opening leg price - closing leg price) so the
// later close scores the move of the contracts actually held, and its cost lands in the roll total,
// never in a trade's P&L.
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>
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

// =============================================================================================
// T-ROLLX-FIX commit 5 (finding 11): a roll's closing and opening leg are paired by the legs' own
// identity (the exec id's role and pair; without one, the side against the tracked position), never
// by the order they arrive in, and the bar's rolls are carried on the bar's first row of any type.
// The five fixtures of the lead's adversary (c4_lead_audit/adversary/A5): three scored the hand
// value before, the two orderings the engine's sort does not produce scored 20.0 and 30.0.
// =============================================================================================

// A SHORT held through a roll by TWO sleeves, with a partial close before the roll.
//   day 0  sleeve A SELL 3 @ 100, sleeve B SELL 2 @ 102   -> short 5, average 100.8
//   day 2  sleeve A BUY 1 @ 98 (partial close)            -> scores 1 x (100.8 - 98) = 2.8; short 4
//   day 4  roll: closing legs BUY @ 97 (old), opening legs SELL @ 91 (new); A 2 lots, B 2 lots
//   day 6  A BUY 2 @ 90, B BUY 2 @ 90
// Hand: each remaining short lot earns (100.8 - 97) in the old contract + (91 - 90) in the new = 4.8;
// 4 lots = 19.2 (9.6 per closing fill). All costs 0.
TEST(TradeStatisticsRollLegs, AShortInTwoSleevesWithAPartialCloseBeforeTheRoll) {
    BacktestMetricsCalculator calc;
    const std::vector<ExecutionReport> execs = {
        fill("ZZ", Side::SELL, 3.0, 100.0, 0, 0.0),
        fill("ZZ", Side::SELL, 2.0, 102.0, 0, 0.0),
        fill("ZZ", Side::BUY, 1.0, 98.0, 2, 0.0),
        fill("ZZ", Side::BUY, 2.0, 97.0, 4, 0.0, ExecutionType::ROLL, "OLD"),
        fill("ZZ", Side::SELL, 2.0, 91.0, 4, 0.0, ExecutionType::ROLL, "NEW"),
        fill("ZZ", Side::BUY, 2.0, 97.0, 4, 0.0, ExecutionType::ROLL, "OLD"),
        fill("ZZ", Side::SELL, 2.0, 91.0, 4, 0.0, ExecutionType::ROLL, "NEW"),
        fill("ZZ", Side::BUY, 2.0, 90.0, 6, 0.0),
        fill("ZZ", Side::BUY, 2.0, 90.0, 6, 0.0),
    };
    const auto s = calc.calculate_trade_statistics(execs);
    EXPECT_EQ(s.total_trades, 3);
    EXPECT_EQ(s.roll_fills, 4);
    EXPECT_NEAR(s.total_profit, 2.8 + 19.2, 1e-9);
    EXPECT_NEAR(s.max_win, 9.6, 1e-9);
    EXPECT_NEAR(s.total_loss, 0.0, 1e-12);
    EXPECT_NEAR(calc.calculate_symbol_pnl(execs).at("ZZ"), 22.0, 1e-9);
}

// The two sleeves holding DIFFERENT sizes (A 2, B 1): each sleeve's pair has the same two prices,
// so the gap is carried once. Hand: short 3 @ 100; roll 97 -> 91; out @ 90:
// 3 x ((100 - 97) + (91 - 90)) = 12.
TEST(TradeStatisticsRollLegs, TwoSleevesOfDifferentSizeCarryOnce) {
    BacktestMetricsCalculator calc;
    const std::vector<ExecutionReport> execs = {
        fill("ZZ", Side::SELL, 2.0, 100.0, 0, 0.0),
        fill("ZZ", Side::SELL, 1.0, 100.0, 0, 0.0),
        fill("ZZ", Side::BUY, 2.0, 97.0, 4, 0.0, ExecutionType::ROLL, "OLD"),
        fill("ZZ", Side::SELL, 2.0, 91.0, 4, 0.0, ExecutionType::ROLL, "NEW"),
        fill("ZZ", Side::BUY, 1.0, 97.0, 4, 0.0, ExecutionType::ROLL, "OLD"),
        fill("ZZ", Side::SELL, 1.0, 91.0, 4, 0.0, ExecutionType::ROLL, "NEW"),
        fill("ZZ", Side::BUY, 3.0, 90.0, 6, 0.0),
    };
    EXPECT_NEAR(calc.calculate_trade_statistics(execs).total_profit, 12.0, 1e-9);
    EXPECT_NEAR(calc.calculate_symbol_pnl(execs).at("ZZ"), 12.0, 1e-9);
}

// A roll pair arriving for a symbol with NO open tracked trade, then an entry on the SAME bar and
// a later exit: the legs score nothing and the later entry is not moved. Hand: 1 x (95 - 91) = 4.
TEST(TradeStatisticsRollLegs, ARollPairWithNoOpenTradeThenAnEntryOnTheSameBar) {
    BacktestMetricsCalculator calc;
    const std::vector<ExecutionReport> execs = {
        fill("ZZ", Side::SELL, 1.0, 97.0, 4, 0.5, ExecutionType::ROLL, "OLD"),
        fill("ZZ", Side::BUY, 1.0, 91.0, 4, 0.5, ExecutionType::ROLL, "NEW"),
        fill("ZZ", Side::BUY, 1.0, 91.0, 4, 0.0),
        fill("ZZ", Side::SELL, 1.0, 95.0, 6, 0.0),
    };
    const auto s = calc.calculate_trade_statistics(execs);
    EXPECT_EQ(s.total_trades, 1);
    EXPECT_NEAR(s.total_profit, 4.0, 1e-9);
    EXPECT_NEAR(s.roll_costs, 1.0, 1e-12);
}

// The trade CLOSED by a strategy fill on the roll's own confirming bar, the fill stored BEFORE the
// legs. Sleeve A long 1 @ 100 and sleeve B long 1 @ 100; roll 104 -> 110 on day 4; on day 4 sleeve A
// also sells its 1 at 111 (the confirming bar's close, the new contract).
//   A's lot: (104 - 100) in the old contract + (111 - 110) in the new = 5
//   B's lot, out on day 6 at 115: (104 - 100) + (115 - 110) = 9
// 14.0, whichever of the fill and the legs is stored first. Pairing by arrival scored the fill
// first ordering 20.0: the fill at 111 against the un-carried entry 100 (11), then 9.
TEST(TradeStatisticsRollLegs, ACloseOnTheConfirmingBarStoredBeforeTheLegs) {
    BacktestMetricsCalculator calc;
    const std::vector<ExecutionReport> fill_first = {
        fill("ZZ", Side::BUY, 1.0, 100.0, 0, 0.0),
        fill("ZZ", Side::BUY, 1.0, 100.0, 0, 0.0),
        fill("ZZ", Side::SELL, 1.0, 111.0, 4, 0.0),
        fill("ZZ", Side::SELL, 1.0, 104.0, 4, 0.0, ExecutionType::ROLL, "OLD"),
        fill("ZZ", Side::BUY, 1.0, 110.0, 4, 0.0, ExecutionType::ROLL, "NEW"),
        fill("ZZ", Side::SELL, 1.0, 104.0, 4, 0.0, ExecutionType::ROLL, "OLD"),
        fill("ZZ", Side::BUY, 1.0, 110.0, 4, 0.0, ExecutionType::ROLL, "NEW"),
        fill("ZZ", Side::SELL, 1.0, 115.0, 6, 0.0),
    };
    auto legs_first = fill_first;
    std::rotate(legs_first.begin() + 2, legs_first.begin() + 3, legs_first.begin() + 7);
    EXPECT_NEAR(calc.calculate_trade_statistics(legs_first).total_profit, 14.0, 1e-9) << "legs first";
    EXPECT_NEAR(calc.calculate_trade_statistics(fill_first).total_profit, 14.0, 1e-9) << "fill first";
    EXPECT_NEAR(calc.calculate_symbol_pnl(fill_first).at("ZZ"), 14.0, 1e-9) << "fill first, by symbol";
}

// Two sleeves whose legs are stored grouped by LEG (closing, closing, opening, opening) instead of
// by sleeve. Hand: long 2 @ 100; roll 104 -> 110; out @ 115: 2 x ((104 - 100) + (115 - 110)) = 18.
// Pairing by arrival read (104, 104) and (110, 110) as two rolls of gap 0 and scored 30.0.
TEST(TradeStatisticsRollLegs, LegsGroupedByLegNotBySleeve) {
    BacktestMetricsCalculator calc;
    const std::vector<ExecutionReport> execs = {
        fill("ZZ", Side::BUY, 1.0, 100.0, 0, 0.0),
        fill("ZZ", Side::BUY, 1.0, 100.0, 0, 0.0),
        fill("ZZ", Side::SELL, 1.0, 104.0, 4, 0.0, ExecutionType::ROLL, "OLD"),
        fill("ZZ", Side::SELL, 1.0, 104.0, 4, 0.0, ExecutionType::ROLL, "OLD"),
        fill("ZZ", Side::BUY, 1.0, 110.0, 4, 0.0, ExecutionType::ROLL, "NEW"),
        fill("ZZ", Side::BUY, 1.0, 110.0, 4, 0.0, ExecutionType::ROLL, "NEW"),
        fill("ZZ", Side::SELL, 2.0, 115.0, 6, 0.0),
    };
    EXPECT_NEAR(calc.calculate_trade_statistics(execs).total_profit, 18.0, 1e-9);
    EXPECT_NEAR(calc.calculate_symbol_pnl(execs).at("ZZ"), 18.0, 1e-9);
}

namespace {
ExecutionReport with_id(ExecutionReport e, const std::string& exec_id) {
    e.exec_id = exec_id;
    return e;
}
}  // namespace

// The engine's own ids (RL-<sleeve>-<n>: a roll takes n and n + 1, the even one the closing leg)
// carry the role and the pair, so the same answer comes out of any order of a bar's legs, and the
// role never depends on the tracked position. Here the tracker opens SHORT 1 after the warm-up
// (which clears every execution) while the sleeve itself is long and its legs are a long's (SELL
// the old contract at 104, BUY the new at 110): the gap is 110 - 104 = +6 by the ids, and the short
// entry 100 is carried to 106; closed at 103 it scores 1 x (106 - 103) = 3. Reading the role from
// the side against the tracked short would carry -6 and score -9.
TEST(TradeStatisticsRollLegs, TheEnginesExecIdsCarryTheRoleAndThePairInAnyOrder) {
    BacktestMetricsCalculator calc;
    const auto rc = with_id(fill("ZZ", Side::SELL, 4.0, 104.0, 4, 0.0, ExecutionType::ROLL, "OLD"), "RL-TREND-6");
    const auto ro = with_id(fill("ZZ", Side::BUY, 4.0, 110.0, 4, 0.0, ExecutionType::ROLL, "NEW"), "RL-TREND-7");
    const auto rc2 = with_id(fill("ZZ", Side::SELL, 2.0, 104.0, 4, 0.0, ExecutionType::ROLL, "OLD"), "RL-FAST-0");
    const auto ro2 = with_id(fill("ZZ", Side::BUY, 2.0, 110.0, 4, 0.0, ExecutionType::ROLL, "NEW"), "RL-FAST-1");
    const auto open = fill("ZZ", Side::SELL, 1.0, 100.0, 0, 0.0);
    const auto close = fill("ZZ", Side::BUY, 1.0, 103.0, 6, 0.0);
    const std::vector<std::vector<ExecutionReport>> orders = {
        {open, rc, ro, rc2, ro2, close},   // the engine's stored order
        {open, ro, rc, ro2, rc2, close},   // each pair reversed
        {open, rc, rc2, ro, ro2, close},   // grouped by leg
        {open, ro2, ro, rc, rc2, close},
    };
    for (size_t i = 0; i < orders.size(); ++i) {
        const auto s = calc.calculate_trade_statistics(orders[i]);
        EXPECT_EQ(s.total_trades, 1) << "order " << i;
        EXPECT_EQ(s.roll_fills, 4) << "order " << i;
        EXPECT_NEAR(s.total_profit, 3.0, 1e-9) << "order " << i;
        EXPECT_NEAR(calc.calculate_symbol_pnl(orders[i]).at("ZZ"), 3.0, 1e-9) << "order " << i;
    }
}

// Legs that cannot be paired fail loudly, they are never guessed: a closing leg whose opening leg
// is missing (by id), and two different rolls on one bar whose ids carry no pair. With no open
// trade there is no entry to carry and the legs are not read.
TEST(TradeStatisticsRollLegs, LegsThatCannotBePairedFailLoudly) {
    BacktestMetricsCalculator calc;
    const auto open = fill("ZZ", Side::BUY, 1.0, 100.0, 0, 0.0);
    const auto close = fill("ZZ", Side::SELL, 1.0, 115.0, 6, 0.0);
    const std::vector<ExecutionReport> lone_closing = {
        open, with_id(fill("ZZ", Side::SELL, 1.0, 104.0, 4, 0.0, ExecutionType::ROLL, "OLD"), "RL-TREND-0"),
        close};
    EXPECT_THROW(calc.calculate_trade_statistics(lone_closing), std::runtime_error);
    EXPECT_THROW(calc.calculate_symbol_pnl(lone_closing), std::runtime_error);
    const std::vector<ExecutionReport> two_rolls_no_pair = {
        open,
        fill("ZZ", Side::SELL, 1.0, 104.0, 4, 0.0, ExecutionType::ROLL, "A1"),
        fill("ZZ", Side::BUY, 1.0, 110.0, 4, 0.0, ExecutionType::ROLL, "A2"),
        fill("ZZ", Side::SELL, 1.0, 112.0, 4, 0.0, ExecutionType::ROLL, "A2"),
        fill("ZZ", Side::BUY, 1.0, 109.0, 4, 0.0, ExecutionType::ROLL, "A3"),
        close};
    EXPECT_THROW(calc.calculate_trade_statistics(two_rolls_no_pair), std::runtime_error);
    // The same two rolls with the engine's ids are two pairs: 100 + 6 - 3 = 103, 115 - 103 = 12.
    auto two_rolls = two_rolls_no_pair;
    for (int i = 1; i <= 4; ++i) two_rolls[i].exec_id = "RL-TREND-" + std::to_string(i - 1);
    EXPECT_NEAR(calc.calculate_trade_statistics(two_rolls).total_profit, 12.0, 1e-9);
    // No open trade: nothing to carry, nothing read.
    const std::vector<ExecutionReport> flat = {lone_closing[1]};
    EXPECT_NO_THROW(calc.calculate_trade_statistics(flat));
}

// The cost after netting (HD 2026-10-09): a trade's P&L and a symbol's P&L charge each fill's NET
// cost, its own cost minus the signed adjustment on the row. Two sleeves' rows of one symbol:
//
//   | day | row              | own  | adjustment | net  |
//   | 0   | BUY 2 at 100     | 2.00 | 0          | 2.00 |
//   | 5   | SELL 1 at 103    | 1.00 | -0.25      | 1.25 | same side as the row below: charged MORE
//   | 5   | SELL 1 at 103    | 1.00 | -0.25      | 1.25 |
//
// RED when either function reads the own cost, or drops or clamps the negative adjustment.
TEST(TradeStatisticsNetCost, ATradeAndASymbolAreChargedTheCostAfterNetting) {
    BacktestMetricsCalculator calc;
    std::vector<ExecutionReport> execs = {
        fill("ES.v.0", Side::BUY, 2.0, 100.0, 0, 2.0),
        fill("ES.v.0", Side::SELL, 1.0, 103.0, 5, 1.0),
        fill("ES.v.0", Side::SELL, 1.0, 103.0, 5, 1.0),
    };
    execs[1].netting_adjustment = Decimal(-0.25);
    execs[2].netting_adjustment = Decimal(-0.25);
    const auto s = calc.calculate_trade_statistics(execs);
    EXPECT_EQ(s.total_trades, 2);
    EXPECT_NEAR(s.total_profit, 2 * (3.0 - 1.25), 1e-12) << "each close: 3 points less its NET cost 1.25";
    EXPECT_NEAR(s.max_win, 3.0 - 1.25, 1e-12);
    const auto pnl = calc.calculate_symbol_pnl(execs);
    EXPECT_NEAR(pnl.at("ES.v.0"), 6.0 - 2.0 - 1.25 - 1.25, 1e-12);

    // A positive adjustment (the sleeves cross) lowers the charge: a full cross is charged nothing.
    std::vector<ExecutionReport> cross = {
        fill("NQ.v.0", Side::BUY, 1.0, 100.0, 0, 1.5),
        fill("NQ.v.0", Side::SELL, 1.0, 100.0, 0, 1.5),
    };
    cross[0].netting_adjustment = Decimal(1.5);
    cross[1].netting_adjustment = Decimal(1.5);
    EXPECT_NEAR(calc.calculate_symbol_pnl(cross).at("NQ.v.0"), 0.0, 1e-12);
    EXPECT_NEAR(calc.calculate_trade_statistics(cross).total_loss, 0.0, 1e-12);
}

// A ROLL leg is never netted: its adjustment is 0 and the roll total is its own cost.
TEST(TradeStatisticsNetCost, ARollLegsCostIsItsOwn) {
    BacktestMetricsCalculator calc;
    const std::vector<ExecutionReport> execs = {
        fill("NG.v.0", Side::BUY, 1.0, 3.30, 0, 1.0),
        fill("NG.v.0", Side::SELL, 1.0, 3.376, 4, 2.0, ExecutionType::ROLL, "864"),
        fill("NG.v.0", Side::BUY, 1.0, 3.965, 4, 2.0, ExecutionType::ROLL, "863"),
    };
    for (const auto& e : execs) EXPECT_EQ(e.netting_adjustment, Decimal());
    EXPECT_DOUBLE_EQ(calc.calculate_trade_statistics(execs).roll_costs, 4.0);
}
