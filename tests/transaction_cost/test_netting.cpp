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
