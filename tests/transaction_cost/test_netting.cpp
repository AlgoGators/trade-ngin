// K3, the netting adjustment (T-7b-2 8b; HD 2026-09-25 item 23: two credited fills).
//
// Every number below is a real stored row: 6C.v.0 on 2026-04-24 and 2026-04-29 from the BASE
// ten-day chain at 64db63f9 (runs/C9h/snap_base), and 6L.v.0 on 2025-11-13 from the BASE history
// in the stage-3 clone (a partial cross: TF BUY 2, FAST SELL 1 at one price). The 04-29 account
// cost C(2) is priced by the real TransactionCostManager on that fill's feed (own-day volume
// 57,392, vol_mult 0.8, T-7b-2 C9h log), which also reproduces the stored C(1) to the digit.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/transaction_cost/netting.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"
#include "session_metadata_rows.hpp"

using namespace trade_ngin;
using namespace trade_ngin::transaction_cost;

namespace {

Decimal d(const char* s) { return Decimal(std::stod(s)); }

AccountCostFn never_called() {
    return [](double, double) -> double {
        throw std::logic_error("C(Q) must not be priced for this symbol-day");
    };
}

int64_t sum_raw(const std::vector<Decimal>& v) {
    int64_t s = 0;
    for (const auto& x : v) s += x.raw_value();
    return s;
}

ExecutionReport exec(const std::string& symbol, Side side, double qty, double price,
                     const char* cost) {
    ExecutionReport e;
    e.symbol = symbol;
    e.side = side;
    e.filled_quantity = Quantity(qty);
    e.fill_price = Price(price);
    e.total_transaction_costs = d(cost);
    return e;
}

}  // namespace

TEST(Netting, OneLegIsNeverNetted) {
    const auto out = net_symbol_day({{"TREND_FOLLOWING_FAST", 1.0, 77960.0, d("2.87279481")}},
                                    never_called());
    EXPECT_EQ(out.status, NettingStatus::SINGLE);
    ASSERT_EQ(out.adjustment.size(), 1u);
    EXPECT_EQ(out.adjustment[0], Decimal());
}

// 6C.v.0 2026-04-24: a lot moves FAST -> TF, the account's book stays at 2, no order is sent.
TEST(Netting, AFullCrossCreditsEachRowItsOwnCostAndTheAccountPaysZero) {
    const std::vector<NettingLeg> legs{{"TREND_FOLLOWING", +1.0, 0.73155, d("3.94919180")},
                                       {"TREND_FOLLOWING_FAST", -1.0, 0.73155, d("3.94919180")}};
    const auto out = net_symbol_day(legs, never_called());  // Q = 0: C(Q) is never priced
    EXPECT_EQ(out.status, NettingStatus::FULL_CROSS);
    EXPECT_DOUBLE_EQ(out.account_quantity, 0.0);
    EXPECT_EQ(out.account_cost, Decimal());
    EXPECT_EQ(out.credit_total, d("7.89838360"));
    ASSERT_EQ(out.adjustment.size(), 2u);
    EXPECT_EQ(out.adjustment[0], d("3.94919180"));
    EXPECT_EQ(out.adjustment[1], d("3.94919180"));
    EXPECT_EQ(legs[0].own_cost - out.adjustment[0], Decimal()) << "TF's net cost";
    EXPECT_EQ(legs[1].own_cost - out.adjustment[1], Decimal()) << "FAST's net cost";
}

TEST(Netting, AThreeSleeveFullCrossOfUnequalSizesIsStillExact) {
    const std::vector<NettingLeg> legs{{"TREND_FOLLOWING", +2.0, 50.0, d("7.59116469")},
                                       {"TREND_FOLLOWING_FAST", -1.0, 50.0, d("3.69527891")},
                                       {"TREND_FOLLOWING_SLOW", -1.0, 50.0, d("3.69527891")}};
    const auto out = net_symbol_day(legs, never_called());
    EXPECT_EQ(out.status, NettingStatus::FULL_CROSS);
    for (size_t i = 0; i < legs.size(); ++i) EXPECT_EQ(out.adjustment[i], legs[i].own_cost);
}

// 6C.v.0 2026-04-29: both sleeves SELL 1, the account sells 2. One order of 2 costs more than
// two orders of 1 (impact ~ |q|^1.5), so the adjustment is NEGATIVE (T-4b sign option (i)).
TEST(Netting, SameDirectionIsADebitPricedByTheRealCostModel) {
    trade_ngin::testing::register_session_metadata_futures();  // CM1: specs come from the metadata
    TransactionCostManager tcm;
    const double adv = 57392.0, vol_mult = 0.8, px = 0.7324;
    const Decimal c1(
        tcm.calculate_costs("6C.v.0", -1.0, px, adv, vol_mult).total_transaction_costs);
    ASSERT_EQ(c1, d("3.97287664")) << "the stored C(1) of both 04-29 rows";

    const std::vector<NettingLeg> legs{{"TREND_FOLLOWING", -1.0, px, c1},
                                       {"TREND_FOLLOWING_FAST", -1.0, px, c1}};
    int calls = 0;
    const auto out = net_symbol_day(legs, [&](double q, double p) {
        ++calls;
        EXPECT_DOUBLE_EQ(q, -2.0) << "the account's order is the SIGNED sum";
        EXPECT_DOUBLE_EQ(p, px);
        return tcm.calculate_costs("6C.v.0", q, p, adv, vol_mult).total_transaction_costs;
    });
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(out.status, NettingStatus::NETTED);
    EXPECT_EQ(out.account_cost, d("8.95881745")) << "C(2)";
    EXPECT_EQ(out.sum_own_cost, d("7.94575328"));
    EXPECT_EQ(out.credit_total, d("-1.01306417"));
    // 101,306,417 units split equally: 50,653,208 each and one left over, which goes to the
    // tie's smaller sleeve name; the sign is applied after.
    EXPECT_EQ(out.adjustment[0], d("-0.50653209")) << "TREND_FOLLOWING";
    EXPECT_EQ(out.adjustment[1], d("-0.50653208")) << "TREND_FOLLOWING_FAST";
    EXPECT_EQ((legs[0].own_cost - out.adjustment[0]) + (legs[1].own_cost - out.adjustment[1]),
              out.account_cost)
        << "the net costs sum to C(Q) to the last stored digit";
}

TEST(Netting, TheTieGoesToTheSmallerSleeveNameWhateverTheInputOrder) {
    trade_ngin::testing::register_session_metadata_futures();  // CM1: specs come from the metadata
    TransactionCostManager tcm;
    const double adv = 57392.0, vol_mult = 0.8, px = 0.7324;
    const Decimal c1(
        tcm.calculate_costs("6C.v.0", -1.0, px, adv, vol_mult).total_transaction_costs);
    const std::vector<NettingLeg> legs{{"TREND_FOLLOWING_FAST", -1.0, px, c1},
                                       {"TREND_FOLLOWING", -1.0, px, c1}};
    const auto out = net_symbol_day(legs, [&](double q, double p) {
        return tcm.calculate_costs("6C.v.0", q, p, adv, vol_mult).total_transaction_costs;
    });
    EXPECT_EQ(out.adjustment[0], d("-0.50653208")) << "TREND_FOLLOWING_FAST";
    EXPECT_EQ(out.adjustment[1], d("-0.50653209")) << "TREND_FOLLOWING";
}

// 6L.v.0 2025-11-13 (stored BASE history): TF BUY 2, FAST SELL 1 at 0.18805; the account buys 1,
// and FAST's own row is C(1) on the same state.
TEST(Netting, APartialCrossSplitsProRataToOwnCost) {
    const std::vector<NettingLeg> legs{{"TREND_FOLLOWING", +2.0, 0.18805, d("14.93890738")},
                                       {"TREND_FOLLOWING_FAST", -1.0, 0.18805, d("7.18550728")}};
    const auto out = net_symbol_day(legs, [](double q, double) {
        EXPECT_DOUBLE_EQ(q, 1.0);
        return 7.18550728;
    });
    EXPECT_EQ(out.status, NettingStatus::NETTED);
    EXPECT_EQ(out.credit_total, d("14.93890738"));
    EXPECT_EQ(out.adjustment[0], d("10.08708963"));
    EXPECT_EQ(out.adjustment[1], d("4.85181775"));
    EXPECT_EQ(legs[0].own_cost - out.adjustment[0], d("4.85181775"));
    EXPECT_EQ(legs[1].own_cost - out.adjustment[1], d("2.33368953"));
}

TEST(Netting, RowsAtDifferentPricesAreNotNetted) {
    const std::vector<NettingLeg> legs{{"TREND_FOLLOWING", +1.0, 0.73155, d("3.94919180")},
                                       {"TREND_FOLLOWING_FAST", -1.0, 0.73160, d("3.94919180")}};
    const auto out = net_symbol_day(legs, never_called());
    EXPECT_EQ(out.status, NettingStatus::MIXED_PRICES);
    EXPECT_EQ(out.adjustment[0], Decimal());
    EXPECT_EQ(out.adjustment[1], Decimal());
}

TEST(Netting, ZeroCostLegsAreNotSplit) {
    const auto out = net_symbol_day({{"A", 1.0, 10.0, Decimal()}, {"B", 1.0, 10.0, Decimal()}},
                                    [](double, double) { return 3.0; });
    EXPECT_EQ(out.status, NettingStatus::ZERO_COST);
    EXPECT_EQ(out.adjustment[0], Decimal());
    EXPECT_EQ(out.adjustment[1], Decimal());
}

// The identity on a sweep of sizes, both directions and crosses, priced by the real cost model:
// the adjustments sum to credit_total exactly, the net costs sum to C(Q) exactly, and no
// sleeve's net cost is negative (the C(q_i) weight; T-4b adversary).
TEST(Netting, TheIdentityHoldsExactlyAndNoNetCostIsNegative) {
    TransactionCostManager tcm;
    const double px = 0.7324;
    for (double adv : {800.0, 5558.0, 57392.0}) {
        for (int a = -7; a <= 7; ++a) {
            for (int b = -7; b <= 7; ++b) {
                if (a == 0 || b == 0) continue;
                auto cost = [&](double q) {
                    return tcm.calculate_costs("6C.v.0", q, px, adv, 0.8).total_transaction_costs;
                };
                const std::vector<NettingLeg> legs{{"TF", double(a), px, Decimal(cost(a))},
                                                   {"TFF", double(b), px, Decimal(cost(b))}};
                const auto out = net_symbol_day(legs, [&](double q, double) { return cost(q); });
                EXPECT_EQ(sum_raw(out.adjustment), out.credit_total.raw_value()) << a << "," << b;
                const Decimal net0 = legs[0].own_cost - out.adjustment[0];
                const Decimal net1 = legs[1].own_cost - out.adjustment[1];
                EXPECT_EQ(net0 + net1, out.account_cost) << a << "," << b;
                EXPECT_GE(net0.raw_value(), 0) << a << "," << b;
                EXPECT_GE(net1.raw_value(), 0) << a << "," << b;
            }
        }
    }
}

// The grouping writes the column on every row: 0 on a one-row symbol, the netted value on a
// multi-row symbol, and one NETTING line per netted symbol-day.
TEST(Netting, ApplyGroupsBySymbolAndWritesEveryRow) {
    // C9h basechain 2026-04-25: the 6C lot moves back (TF SELL 1, FAST BUY 1) and FAST buys MBT.
    ExecutionReport tf_6c = exec("6C.v.0", Side::SELL, 1, 0.73325, "4.75835981");
    ExecutionReport ff_6c = exec("6C.v.0", Side::BUY, 1, 0.73325, "4.75835981");
    ExecutionReport ff_mbt = exec("MBT.v.0", Side::BUY, 1, 77960, "2.87279481");
    ff_mbt.netting_adjustment = d("9.99");  // a stale value must be overwritten with 0
    std::vector<SleeveExecution> rows{{"TREND_FOLLOWING", &tf_6c},
                                      {"TREND_FOLLOWING_FAST", &ff_6c},
                                      {"TREND_FOLLOWING_FAST", &ff_mbt}};
    const auto rep = apply_netting_adjustments(
        rows, [](const std::string& s, double, double) -> double {
            throw std::logic_error("no account cost to price on this day: " + s);
        });
    EXPECT_EQ(tf_6c.netting_adjustment, d("4.75835981"));
    EXPECT_EQ(ff_6c.netting_adjustment, d("4.75835981"));
    EXPECT_EQ(ff_mbt.netting_adjustment, Decimal());
    EXPECT_EQ(rep.symbol_days_netted, 1);
    ASSERT_EQ(rep.info_lines.size(), 1u);
    EXPECT_NE(rep.info_lines[0].find("NETTING sym=6C.v.0 status=FULL_CROSS Q=0"), std::string::npos)
        << rep.info_lines[0];
    EXPECT_TRUE(rep.warn_lines.empty());
}

TEST(Netting, ApplyPricesTheAccountOrderWithItsSymbolAndSignedQuantity) {
    ExecutionReport tf = exec("6C.v.0", Side::SELL, 1, 0.7324, "3.97287664");
    ExecutionReport ff = exec("6C.v.0", Side::SELL, 1, 0.7324, "3.97287664");
    std::vector<SleeveExecution> rows{{"TREND_FOLLOWING_FAST", &ff}, {"TREND_FOLLOWING", &tf}};
    std::string seen;
    double seen_q = 0.0;
    apply_netting_adjustments(rows, [&](const std::string& s, double q, double) {
        seen = s;
        seen_q = q;
        return 8.95881745;
    });
    EXPECT_EQ(seen, "6C.v.0");
    EXPECT_DOUBLE_EQ(seen_q, -2.0);
    EXPECT_EQ(tf.netting_adjustment, d("-0.50653209"));
    EXPECT_EQ(ff.netting_adjustment, d("-0.50653208"));
}

// ---------------------------------------------------------------------------------------------
// The cost after netting (HD 2026-10-09): every total charges net_cost(fill), the fill's own cost
// MINUS the signed adjustment already on it. The three symbol-days below are real rows of the BASE
// lookback-3 backtest (IBKR fees), priced here by a table of the cost model's own prices for them.
// ---------------------------------------------------------------------------------------------

namespace {

struct Day {
    ExecutionReport fast, trend;
    double account_cost;  // the cost model's price for the account's one order, C(Q); 0 = no order
};

// Nets the day's two sleeve rows the way the runners do and returns the fills.
std::vector<ExecutionReport> netted(Day day) {
    std::vector<SleeveExecution> rows{{"TREND_FOLLOWING_FAST", &day.fast},
                                      {"TREND_FOLLOWING", &day.trend}};
    apply_netting_adjustments(rows, [&](const std::string&, double, double) {
        return day.account_cost;
    });
    return {day.fast, day.trend};
}

Decimal sum_own(const std::vector<ExecutionReport>& fills) {
    Decimal s;
    for (const auto& f : fills) s += f.total_transaction_costs;
    return s;
}

// 2026-09-16 MES: FAST SELL 1, TREND BUY 1: the account sends no order.
Day full_cross() {
    return {exec("MES.v.0", Side::SELL, 1, 6650.0, "1.62"), exec("MES.v.0", Side::BUY, 1, 6650.0, "1.62"),
            0.0};
}
// 2026-03-01 MYM: FAST BUY 1, TREND SELL 2: the account sells 1.
Day partial_offset() {
    return {exec("MYM.v.0", Side::BUY, 1, 49000.0, "1.10"), exec("MYM.v.0", Side::SELL, 2, 49000.0, "2.40"),
            1.10};
}
// 2026-09-13 MBT: FAST SELL 1, TREND SELL 1: the account sells 2, and one order of two costs more
// than two orders of one.
Day same_side() {
    return {exec("MBT.v.0", Side::SELL, 1, 115000.0, "4.50"), exec("MBT.v.0", Side::SELL, 1, 115000.0, "4.50"),
            10.83};
}

}  // namespace

TEST(NetCost, AFullCrossIsChargedNothing) {
    const auto fills = netted(full_cross());
    EXPECT_EQ(sum_own(fills), d("3.24")) << "the rows keep their own cost";
    EXPECT_EQ(net_cost(fills[0]), Decimal());
    EXPECT_EQ(net_cost(fills[1]), Decimal());
    EXPECT_DOUBLE_EQ(add_net_costs(0.0, fills), 0.0) << "3.24 own, 0.00 charged";
}

TEST(NetCost, APartialOffsetIsChargedTheAccountOrder) {
    const auto fills = netted(partial_offset());
    EXPECT_EQ(sum_own(fills), d("3.50"));
    EXPECT_GT(fills[0].netting_adjustment, Decimal());
    EXPECT_GT(fills[1].netting_adjustment, Decimal());
    EXPECT_EQ(net_cost(fills[0]) + net_cost(fills[1]), d("1.10"))
        << "the legs' net costs sum to the cost of the account's SELL 1";
    EXPECT_NEAR(add_net_costs(0.0, fills), 1.10, 1e-9) << "3.50 own, 1.10 charged";
}

// The sign: a NEGATIVE adjustment RAISES the cost. Fails if a site drops a negative adjustment
// (9.00), takes its absolute value (7.17) or clamps the net at the own cost (9.00).
TEST(NetCost, ASameSidePairIsChargedMoreThanItsOwnCosts) {
    const auto fills = netted(same_side());
    EXPECT_EQ(sum_own(fills), d("9.00"));
    EXPECT_EQ(fills[0].netting_adjustment, d("-0.915"));
    EXPECT_EQ(fills[1].netting_adjustment, d("-0.915"));
    EXPECT_EQ(net_cost(fills[0]), d("5.415")) << "own 4.50 minus (-0.915)";
    EXPECT_GT(net_cost(fills[0]), fills[0].total_transaction_costs);
    EXPECT_EQ(net_cost(fills[0]) + net_cost(fills[1]), d("10.83"))
        << "the legs' net costs sum to the cost of the account's SELL 2";
    EXPECT_NEAR(add_net_costs(0.0, fills), 10.83, 1e-9) << "9.00 own, 10.83 charged";
    EXPECT_GT(add_net_costs(0.0, fills), static_cast<double>(sum_own(fills)));
}

// A book with one sleeve: one row per symbol-day, adjustment 0, net = own on every fill, and the
// day's charge is the same double the sum of own costs is (no last-digit movement).
TEST(NetCost, AOneSleeveBookIsChargedItsOwnCosts) {
    ExecutionReport a = exec("MES.v.0", Side::BUY, 3, 6650.0, "4.86000001");
    ExecutionReport b = exec("ZN.v.0", Side::SELL, 2, 112.5, "7.12345678");
    ExecutionReport c = exec("6C.v.0", Side::BUY, 1, 0.7324, "3.97287664");
    std::vector<SleeveExecution> rows{{"TREND_FOLLOWING", &a}, {"TREND_FOLLOWING", &b},
                                      {"TREND_FOLLOWING", &c}};
    apply_netting_adjustments(rows, [](const std::string& s, double, double) -> double {
        throw std::logic_error("a one-row symbol-day prices no account order: " + s);
    });
    const std::vector<ExecutionReport> fills{a, b, c};
    double own = 0.0;
    for (const auto& f : fills) {
        EXPECT_EQ(f.netting_adjustment, Decimal());
        EXPECT_EQ(net_cost(f), f.total_transaction_costs);
        own += static_cast<double>(f.total_transaction_costs);
    }
    EXPECT_EQ(add_net_costs(0.0, fills), own) << "bit for bit";
}

// A ROLL leg is never netted: the pass that nets a day's rows never sees it, so its adjustment is
// the 0 it was made with and its net cost is its own cost.
TEST(NetCost, ARollLegIsChargedItsOwnCost) {
    ExecutionReport leg = exec("MES.v.0", Side::SELL, 2, 6650.0, "3.24");
    leg.execution_type = ExecutionType::ROLL;
    EXPECT_EQ(leg.netting_adjustment, Decimal());
    EXPECT_EQ(net_cost(leg), d("3.24"));
    EXPECT_DOUBLE_EQ(add_net_costs(0.0, {leg}), 3.24);
}

// Legs at different fill prices are not netted (a warning, adjustment 0): each is charged its own
// cost, and no account order is priced.
TEST(NetCost, APairAtDifferentPricesIsChargedItsOwnCosts) {
    ExecutionReport tf = exec("6C.v.0", Side::BUY, 1, 0.73155, "3.94919180");
    ExecutionReport ff = exec("6C.v.0", Side::SELL, 1, 0.73160, "3.94919180");
    std::vector<SleeveExecution> rows{{"TREND_FOLLOWING", &tf}, {"TREND_FOLLOWING_FAST", &ff}};
    const auto rep = apply_netting_adjustments(
        rows, [](const std::string& s, double, double) -> double {
            throw std::logic_error("rows at different prices price no account order: " + s);
        });
    EXPECT_EQ(rep.warn_lines.size(), 1u);
    EXPECT_EQ(net_cost(tf), d("3.94919180"));
    EXPECT_EQ(net_cost(ff), d("3.94919180"));
    EXPECT_NEAR(add_net_costs(0.0, {tf, ff}), 7.8983836, 1e-9);
}

// The running total and the starting index (the backtest sums only the bar's new fills).
TEST(NetCost, AddNetCostsStartsFromTheGivenFillAndKeepsTheRunningTotal) {
    auto fills = netted(same_side());             // 10.83
    const auto more = netted(partial_offset());   // 1.10
    fills.insert(fills.end(), more.begin(), more.end());
    EXPECT_NEAR(add_net_costs(100.0, fills), 111.93, 1e-9);
    EXPECT_NEAR(add_net_costs(0.0, fills, 2), 1.10, 1e-9);
    EXPECT_DOUBLE_EQ(add_net_costs(5.0, fills, fills.size()), 5.0);
}

// T-NETTING fix round: a ROLL leg and a BORROW row are never netted, so each must carry an
// adjustment of exactly 0. One that does not is REFUSED wherever fills are summed (and by the
// executions writer: test_executions_netting_column_db.cpp), never silently charged own cost in
// one total and net cost in another, and never corrected.
TEST(UnnettedRows, ARollOrBorrowRowWithAnAdjustmentIsRefusedWhereFillsAreSummed) {
    for (const ExecutionType type : {ExecutionType::ROLL, ExecutionType::BORROW}) {
        ExecutionReport row = exec("MES.v.0", Side::SELL, 2, 6650.0, "3.24");
        row.exec_id = "RL-TREND_FOLLOWING-7";
        row.execution_type = type;
        EXPECT_TRUE(unnetted_row_refusal(row).empty()) << "adjustment 0: nothing to refuse";
        EXPECT_EQ(unnetted_cost(row), d("3.24")) << "its cost is its own cost";
        EXPECT_DOUBLE_EQ(add_net_costs(0.0, {row}), 3.24);

        for (const char* adjustment : {"0.50", "-0.50", "0.00000001"}) {
            row.netting_adjustment = d(adjustment);
            const std::string refusal = unnetted_row_refusal(row);
            EXPECT_NE(refusal.find("NETTING_REFUSED"), std::string::npos) << refusal;
            EXPECT_NE(refusal.find("RL-TREND_FOLLOWING-7"), std::string::npos) << refusal;
            EXPECT_NE(refusal.find(type == ExecutionType::ROLL ? "ROLL" : "BORROW"),
                      std::string::npos)
                << refusal;
            EXPECT_THROW(unnetted_cost(row), std::logic_error) << adjustment;
            EXPECT_THROW(add_net_costs(0.0, {row}), std::logic_error) << adjustment;
            ExecutionReport ordinary = exec("ZN.v.0", Side::BUY, 1, 112.5, "5.00");
            EXPECT_THROW(add_net_costs(0.0, {ordinary, row}), std::logic_error)
                << "one bad row among good ones refuses the whole sum";
            EXPECT_DOUBLE_EQ(add_net_costs(0.0, {row, ordinary}, 1), 5.00)
                << "a row before the starting index is not summed and not judged";
        }
        row.netting_adjustment = Decimal();
    }
    // A STRATEGY row carries any adjustment: that is what netting is.
    ExecutionReport strategy = exec("MBT.v.0", Side::SELL, 1, 115000.0, "4.50");
    strategy.netting_adjustment = d("-0.915");
    EXPECT_TRUE(unnetted_row_refusal(strategy).empty());
    EXPECT_NEAR(add_net_costs(0.0, {strategy}), 5.415, 1e-12);
}

// T-NETTING fix round 2: the run's cost totals (backtest.results) through one function. Its ROLL
// AND its BORROW rows pass the refusal: a BORROW row is appended after its cycle's sum, so the
// end-of-run totals are where one carrying an adjustment is first met.
//
//   | row                 | own  | adjustment | charged | in roll_costs |
//   | STRATEGY, netted    | 4.50 | -0.915     | 5.415   | no            |
//   | STRATEGY, crossed   | 1.62 | 1.62       | 0       | no            |
//   | ROLL leg            | 3.00 | 0          | 3.00    | 3.00          |
//   | BORROW row          | 0.75 | 0          | 0.75    | no            |
TEST(RunCostTotals, AddsNetCostsAndTheRollLegsOwnCosts) {
    ExecutionReport same_side = exec("MBT.v.0", Side::SELL, 1, 115000.0, "4.50");
    same_side.netting_adjustment = d("-0.915");
    ExecutionReport crossed = exec("MES.v.0", Side::BUY, 1, 6650.0, "1.62");
    crossed.netting_adjustment = d("1.62");
    ExecutionReport roll = exec("ZN.v.0", Side::SELL, 2, 112.0, "3.00");
    roll.execution_type = ExecutionType::ROLL;
    ExecutionReport borrow = exec("AAPL", Side::SELL, 0, 190.0, "0.75");
    borrow.execution_type = ExecutionType::BORROW;
    const auto totals = run_cost_totals({same_side, crossed, roll, borrow});
    EXPECT_NEAR(totals.transaction_costs, 5.415 + 0.0 + 3.00 + 0.75, 1e-12);
    EXPECT_DOUBLE_EQ(totals.roll_costs, 3.00);
    EXPECT_EQ(totals.roll_fills, 1);
    const auto none = run_cost_totals({});
    EXPECT_DOUBLE_EQ(none.transaction_costs, 0.0);
    EXPECT_EQ(none.roll_fills, 0);
}

// RED if the totals stop judging BORROW rows (the check deleted, or narrowed to ROLL legs).
TEST(RunCostTotals, ABorrowRowWithAnAdjustmentIsRefused) {
    ExecutionReport ordinary = exec("ZN.v.0", Side::BUY, 1, 112.5, "5.00");
    ExecutionReport borrow = exec("AAPL", Side::SELL, 0, 190.0, "0.75");
    borrow.exec_id = "BORROW_MR_AAPL";
    borrow.execution_type = ExecutionType::BORROW;
    EXPECT_NO_THROW(run_cost_totals({ordinary, borrow}));
    borrow.netting_adjustment = d("0.25");
    try {
        run_cost_totals({ordinary, borrow});
        FAIL() << "a BORROW row carrying an adjustment passed the run's totals";
    } catch (const NettingRefused& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("the BORROW row BORROW_MR_AAPL"), std::string::npos) << what;
    }
    ExecutionReport roll = exec("ZN.v.0", Side::SELL, 2, 112.0, "3.00");
    roll.execution_type = ExecutionType::ROLL;
    roll.netting_adjustment = d("-0.10");
    EXPECT_THROW(run_cost_totals({ordinary, roll}), NettingRefused);
    EXPECT_THROW(run_cost_totals({ordinary, roll}), std::logic_error) << "still a std::logic_error";
}
