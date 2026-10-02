// The cost model's spread-vol multiplier reads the adjusted return (LOOP_SPEC v6.1 section 2.3,
// L-08; T-ROLLX commit 1): on a change bar the volatility window takes a 0 (the date kept), and on
// every other bar the engine's own log return. A feed with a contract switch therefore gives the
// multiplier of the same feed with the switch bar's return replaced by 0, never the one that books
// the switch's price gap as a return. Both feed forms (one shot and per cycle) and the manager's own
// record_log_return are covered; a feed without ids is untouched (the control).
#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/futures_cost_feed.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using namespace trade_ngin;
using trade_ngin::transaction_cost::TransactionCostManager;

namespace {

const std::string kNg = "NG.v.0";
constexpr int kSwitch = 20;
constexpr double kGap = 0.6;

Bar bar_at(int day, double close, const std::string& id) {
    const Timestamp ts = std::chrono::system_clock::from_time_t(
        1700000000LL + static_cast<long long>(day) * 86400LL);
    Bar b(ts, close, close, close, close, 60000.0, kNg);
    b.instrument_id = id;
    return b;
}

/// 30 bars of a small zigzag; with_switch: a +kGap step and a new id from bar kSwitch on.
std::vector<Bar> feed(bool with_switch, bool with_ids = true) {
    std::vector<Bar> bars;
    double p = 3.0;
    for (int i = 0; i < 30; ++i) {
        p *= (i % 2 == 0) ? 1.006 : 0.995;
        const bool after = with_switch && i >= kSwitch;
        bars.push_back(bar_at(i, p + (after ? kGap : 0.0), !with_ids ? "" : (after ? "863" : "864")));
    }
    return bars;
}

/// The expected window: every log return of the switched feed except the switch bar's, which is 0.
TransactionCostManager expected_manager() {
    TransactionCostManager m;
    const auto bars = feed(true);
    double prev = 0.0;
    for (int i = 0; i < 30; ++i) {
        const double c = static_cast<double>(bars[i].close);
        if (i == kSwitch) {
            m.record_log_return(kNg, prev, prev);  // log(prev / prev) = 0: the excluded return
        } else {
            m.record_log_return(kNg, c, prev);
        }
        prev = c;
    }
    m.set_own_day_volume(kNg, bars.back().volume);
    return m;
}

Timestamp run_date_after(const std::vector<Bar>& bars) {
    return bars.back().timestamp + std::chrono::hours(24);
}

}  // namespace

TEST(CostFeedAdjustedReturns, ASwitchBarsReturnIsZeroInTheSpreadVolWindow) {
    const auto expected = expected_manager();
    TransactionCostManager rolled, blind;
    feed_futures_cost_model(rolled, feed(true), run_date_after(feed(true)));
    EXPECT_EQ(rolled.get_volatility_multiplier(kNg), expected.get_volatility_multiplier(kNg))
        << "the switch's price gap entered the volatility window as a return";
    // The control: the same bars without ids (no change bar is known) book the gap as a return.
    feed_futures_cost_model(blind, feed(true, false), run_date_after(feed(true)));
    EXPECT_NE(blind.get_volatility_multiplier(kNg), expected.get_volatility_multiplier(kNg));
    EXPECT_LT(expected.get_volatility_multiplier(kNg), 1.5) << "not at the clamp";
    EXPECT_GT(expected.get_volatility_multiplier(kNg), 0.8);
}

TEST(CostFeedAdjustedReturns, ThePerCycleFeedCarriesTheLastIdAcrossCycles) {
    const auto expected = expected_manager();
    TransactionCostManager rolled;
    FuturesCostFeedCarry carry;
    const auto r = feed(true);
    for (size_t i = 0; i < r.size(); ++i) {
        feed_futures_cost_model_step(rolled, {r[i]}, r[i].timestamp + std::chrono::hours(24), carry);
    }
    EXPECT_EQ(carry.last_id.at(kNg), "863");
    EXPECT_EQ(rolled.get_volatility_multiplier(kNg), expected.get_volatility_multiplier(kNg));
}

TEST(CostFeedAdjustedReturns, RecordLogReturnTakesAZeroOnAChangeBarAndTheLogElsewhere) {
    TransactionCostManager a, b, c;
    double prev = 0.0;
    for (int i = 0; i < 25; ++i) {
        const double close = 3.0 + 0.03 * ((i % 2 == 0) ? 1.0 : -1.0) + 0.002 * i;
        a.record_log_return(kNg, close, prev, false);
        b.record_log_return(kNg, close, prev, i == 12);  // bar 12 is a change bar for b
        c.record_log_return(kNg, i == 12 ? prev : close, prev, false);  // the same window by hand
        prev = close;
    }
    EXPECT_NE(a.get_volatility_multiplier(kNg), b.get_volatility_multiplier(kNg));
    EXPECT_EQ(c.get_volatility_multiplier(kNg), b.get_volatility_multiplier(kNg));
    EXPECT_LT(a.get_volatility_multiplier(kNg), 1.5);
    EXPECT_GT(a.get_volatility_multiplier(kNg), 0.8);
    // A change bar with no previous close records nothing, as before.
    TransactionCostManager d;
    d.record_log_return(kNg, 3.0, 0.0, true);
    EXPECT_EQ(d.get_volatility_multiplier(kNg), 1.0);
}
