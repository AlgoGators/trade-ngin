// The risk gate's return rows and covariance read the ADJUSTED returns (LOOP_SPEC v6.1 sections
// 2.3 and 4; T-ROLLX commit 1): a contract switch is a zero return row for its symbol, every other
// row is the adjusted change over the raw previous close (the roll_series rule, recomputed by hand),
// a window without a switch is the raw path bit for bit, and the levels of the WHOLE window can be
// handed in so a switch on a date F5 drops is still removed from the level.
#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../core/test_base.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/roll_series.hpp"
#include "trade_ngin/risk/risk_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

Timestamp at(int day) {
    return std::chrono::system_clock::time_point(std::chrono::hours(24 * (19000 + day)));
}

Bar bar(const std::string& symbol, int day, double close, const std::string& id) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = at(day);
    b.open = b.high = b.low = b.close = Decimal(close);
    b.volume = 1000.0;
    b.instrument_id = id;
    return b;
}

RiskConfig config() {
    RiskConfig c;
    c.var_limit = 0.15;
    c.jump_risk_limit = 0.10;
    c.max_correlation = 0.7;
    c.max_gross_leverage = 4.0;
    c.max_net_leverage = 2.0;
    c.capital = 1000000.0;
    c.confidence_level = 0.99;
    c.lookback_period = 252;
    return c;
}

/// Two symbols over `n` days; NG switches contract on `switch_day` with a step of `gap`.
std::vector<Bar> window(int n, int switch_day, double gap) {
    std::vector<Bar> bars;
    double ng = 3.0, es = 5000.0;
    for (int d = 1; d <= n; ++d) {
        ng *= 1.0 + 0.01 * std::sin(d * 0.7);
        es *= 1.0 + 0.004 * std::cos(d * 0.5);
        const bool after = switch_day > 0 && d >= switch_day;
        bars.push_back(bar("NG", d, ng + (after ? gap : 0.0), after ? "863" : "864"));
        bars.push_back(bar("ES", d, es, "1"));
    }
    return bars;
}

double close_of(const std::vector<Bar>& bars, const std::string& symbol, int day) {
    for (const auto& b : bars) {
        if (b.symbol == symbol && b.timestamp == at(day)) return static_cast<double>(b.close);
    }
    return std::nan("");
}

}  // namespace

TEST(RiskAdjustedReturns, AContractSwitchIsAZeroReturnRowForItsSymbol) {
    RiskManager rm(config());
    const auto md = rm.create_market_data(window(6, 4, 0.6));
    ASSERT_EQ(md.ordered_symbols, (std::vector<std::string>{"ES", "NG"}));
    ASSERT_EQ(md.returns.size(), 5u);
    const size_t ng = md.symbol_indices.at("NG");
    EXPECT_DOUBLE_EQ(md.returns[2][ng], 0.0) << "day 3 -> day 4: the switch";
    EXPECT_NE(md.returns[1][ng], 0.0);
    EXPECT_NE(md.returns[3][ng], 0.0);
}

TEST(RiskAdjustedReturns, EveryReturnRowIsTheAdjustedChangeOverTheRawPreviousClose) {
    RiskManager rm(config());
    const auto bars = window(40, 25, 0.6);
    const auto md = rm.create_market_data(bars);
    ASSERT_EQ(md.returns.size(), 39u);
    // The expectation by hand, per symbol: the adjusted level from roll_series on the symbol's own
    // closes and ids, the return (A_t - A_t-1) / P_t-1.
    for (const std::string& s : {"NG", "ES"}) {
        std::vector<double> raw;
        std::vector<std::string> ids;
        for (const auto& b : bars) {
            if (b.symbol == s) {
                raw.push_back(static_cast<double>(b.close));
                ids.push_back(b.instrument_id);
            }
        }
        const auto series = roll_series::build_series(raw, ids);
        const size_t i = md.symbol_indices.at(s);
        for (size_t t = 0; t < 39; ++t) {
            EXPECT_NEAR(md.returns[t][i], series.returns[t], 1e-12) << s << " row " << t;
        }
        if (s == "NG") {
            EXPECT_DOUBLE_EQ(md.returns[23][i], 0.0) << "the switch bar";
            EXPECT_NEAR(md.returns[24][i], (raw[25] - raw[24]) / raw[24], 1e-12)
                << "after the switch the raw previous close is the new contract's level";
        }
    }
    // A window without a switch: the adjusted path is the raw path, bit for bit.
    const auto plain = window(40, -1, 0.0);
    const auto md_plain = rm.create_market_data(plain);
    const auto raw_levels = RiskManager::adjusted_levels_of(plain);
    for (const auto& b : plain) {
        EXPECT_EQ(raw_levels.at(b.symbol).at(b.timestamp), static_cast<double>(b.close));
    }
    for (size_t t = 0; t < md_plain.returns.size(); ++t) {
        for (const std::string& s : {"NG", "ES"}) {
            const double prev = close_of(plain, s, static_cast<int>(t) + 1);
            const double curr = close_of(plain, s, static_cast<int>(t) + 2);
            EXPECT_EQ(md_plain.returns[t][md_plain.symbol_indices.at(s)], (curr - prev) / prev)
                << s << " row " << t;
        }
    }
}

// The gate's F5 filter drops incomplete dates BEFORE create_market_data: with the levels of the
// whole window handed in, a switch on a dropped date is removed from the level and the return across
// the gap is the real move; from the filtered bars alone the step would land on the next kept bar as
// a (wrongly) excluded return.
TEST(RiskAdjustedReturns, TheWholeWindowsLevelsRemoveASwitchOnADateTheFilterDrops) {
    RiskManager rm(config());
    const auto all = window(8, 5, 0.6);
    std::vector<Bar> filtered;
    for (const auto& b : all) {
        if (!(b.symbol == "ES" && b.timestamp == at(5))) filtered.push_back(b);  // ES misses day 5
    }
    // The F5 rule: day 5 is incomplete, dropped for every symbol.
    std::vector<Bar> complete;
    for (const auto& b : filtered) {
        if (b.timestamp != at(5)) complete.push_back(b);
    }
    const auto levels = RiskManager::adjusted_levels_of(filtered);
    const auto md = rm.create_market_data(complete, &levels);
    const size_t ng = md.symbol_indices.at("NG");
    // day 4 -> day 6 for NG: (A6 - A4) / P4. The step on the switch bar (day 5) is the whole close-
    // to-close change p5 - p4 (the gap plus the unknown true move of that day: section 2.3, one
    // zero per roll), so A4 = p4 + (p5 - p4) = p5 and the return is (p6 - p5) / p4: the day-6 move
    // over the pre-switch raw close, the step removed.
    const double p4 = close_of(all, "NG", 4), p5 = close_of(all, "NG", 5), p6 = close_of(all, "NG", 6);
    ASSERT_EQ(md.returns.size(), 6u);
    EXPECT_NEAR(md.returns[3][ng], (p6 - p5) / p4, 1e-12);
    EXPECT_NE(md.returns[3][ng], 0.0) << "a real move, not an excluded one";
    const auto from_filtered = rm.create_market_data(complete);
    EXPECT_DOUBLE_EQ(from_filtered.returns[3][ng], 0.0) << "without the whole window's levels the "
                                                           "switch lands on the next kept bar";
}
