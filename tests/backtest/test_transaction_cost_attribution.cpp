// TransactionCostAnalyzer attribution paths with hand-computed values.
//
// test_transaction_cost_analysis.cpp checks signs and key presence on random
// bars; these build a few bars at fixed times so every expected number can be
// worked out by hand in the comment next to it. Several tests pin today's
// behaviour where it is wrong (FIXME). TransactionCostAnalyzer is not
// instantiated by any runner in apps/ (bt_portfolio and bt_portfolio_conservative
// include the header only), so none of these moves a stored number today.

#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>
#include "trade_ngin/backtest/transaction_cost_analysis.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;

namespace {

// 2026-01-01 00:00:00 UTC
const Timestamp kT0 = std::chrono::system_clock::from_time_t(1767225600);

Timestamp at_min(int m) {
    return kT0 + std::chrono::minutes(m);
}

Bar bar(int minute, double high, double low, double close, double volume = 1000.0,
        const std::string& symbol = "ES") {
    return Bar(at_min(minute), Price(close), Price(high), Price(low), Price(close), volume,
               symbol);
}

ExecutionReport fill(int minute, Side side, double qty, double price,
                     const std::string& symbol = "ES",
                     ExecutionType type = ExecutionType::STRATEGY) {
    ExecutionReport e;
    e.symbol = symbol;
    e.side = side;
    e.filled_quantity = Quantity(qty);
    e.fill_price = Price(price);
    e.fill_time = at_min(minute);
    e.execution_type = type;
    return e;
}

Position target(double qty, int decided_at_minute) {
    Position p;
    p.symbol = "ES";
    p.quantity = Quantity(qty);
    p.last_update = at_min(decided_at_minute);
    return p;
}

double cost(const TransactionCostAnalyzer& tca, const ExecutionReport& e,
            const std::vector<Bar>& bars, double TransactionCostMetrics::*field) {
    auto r = tca.analyze_trade(e, bars);
    EXPECT_TRUE(r.is_ok());
    return r.value().*field;
}

}  // namespace

// ---------------------------------------------------------------------------
// Spread cost: half the high-low range of the last bar strictly before the
// fill, as a fraction of its close, times the traded notional.
// ---------------------------------------------------------------------------

TEST(TCAAttribution, SpreadCostUsesTheLastBarBeforeTheFill) {
    TransactionCostAnalyzer tca{TCAConfig{}};
    const std::vector<Bar> bars = {bar(0, 101.0, 99.0, 100.0), bar(2, 105.0, 95.0, 100.0)};
    const auto spread = &TransactionCostMetrics::spread_cost;

    // Fill at minute 1: bar 0 ranges 2 on a close of 100 -> 0.02 * 0.5 * 100 * 10 = 10.
    EXPECT_NEAR(cost(tca, fill(1, Side::BUY, 10, 100.0), bars, spread), 10.0, 1e-9);
    // Sells pay it too (quantity enters as an absolute value).
    EXPECT_NEAR(cost(tca, fill(1, Side::SELL, 10, 100.0), bars, spread), 10.0, 1e-9);
    // Fill after every bar: the last bar, range 10 -> 0.1 * 0.5 * 100 * 10 = 50.
    EXPECT_NEAR(cost(tca, fill(5, Side::BUY, 10, 100.0), bars, spread), 50.0, 1e-9);
    // No bar before the fill, including a bar stamped at the fill itself: 0.
    EXPECT_EQ(cost(tca, fill(-1, Side::BUY, 10, 100.0), bars, spread), 0.0);
    EXPECT_EQ(cost(tca, fill(0, Side::BUY, 10, 100.0), bars, spread), 0.0);
}

TEST(TCAAttribution, SpreadAndImpactIgnoreTheBarsSymbol) {
    // FIXME: calculate_spread_cost and calculate_market_impact pick the bar by time
    // only, so with several symbols in market_data an ES fill is costed on whatever
    // symbol's bar is nearest (here an NQ bar with a 20-point range). The right value
    // is the ES bar's: 0.02 * 0.5 * 100 * 10 = 10.
    TransactionCostAnalyzer tca{TCAConfig{}};
    const std::vector<Bar> bars = {bar(0, 101.0, 99.0, 100.0, 1000.0, "ES"),
                                   bar(1, 110.0, 90.0, 100.0, 1000.0, "NQ")};
    // NQ bar: 0.2 * 0.5 * 100 * 10 = 100.
    EXPECT_NEAR(cost(tca, fill(2, Side::BUY, 10, 100.0), bars, &TransactionCostMetrics::spread_cost),
                100.0, 1e-9);
}

// ---------------------------------------------------------------------------
// Market impact: the fill against the pre-trade close moved by the next bar's
// return, adverse side only, times the traded notional.
// ---------------------------------------------------------------------------

TEST(TCAAttribution, MarketImpactIsAdverseMoveBeyondTheMarketsOwn) {
    TransactionCostAnalyzer tca{TCAConfig{}};
    // Pre-trade close 100; the next bar closes at 102, so the market alone moved +2%
    // and the market-adjusted price is 102.
    const std::vector<Bar> bars = {bar(0, 100.0, 100.0, 100.0), bar(2, 102.0, 102.0, 102.0)};
    const auto impact = &TransactionCostMetrics::market_impact;

    // Buy at 103: (103 - 102) / 100 = 0.01 -> 0.01 * 103 * 10 = 10.3.
    EXPECT_NEAR(cost(tca, fill(1, Side::BUY, 10, 103.0), bars, impact), 10.3, 1e-9);
    // Buy at 101, below the adjusted price: favourable, so 0.
    EXPECT_EQ(cost(tca, fill(1, Side::BUY, 10, 101.0), bars, impact), 0.0);
    // Sell at 101: (101 - 102) / 100 = -0.01, adverse for a sell -> 0.01 * 101 * 10 = 10.1.
    EXPECT_NEAR(cost(tca, fill(1, Side::SELL, 10, 101.0), bars, impact), 10.1, 1e-9);
    // Sell at 103: favourable, 0.
    EXPECT_EQ(cost(tca, fill(1, Side::SELL, 10, 103.0), bars, impact), 0.0);
    // No bar after the fill to measure against, or none before it: 0.
    EXPECT_EQ(cost(tca, fill(3, Side::BUY, 10, 150.0), bars, impact), 0.0);
    EXPECT_EQ(cost(tca, fill(-1, Side::BUY, 10, 150.0), bars, impact), 0.0);
}

// ---------------------------------------------------------------------------
// Timing cost: distance from the best price inside the +-window around the
// fill (lowest low for a buy, highest high for a sell), times the quantity.
// ---------------------------------------------------------------------------

TEST(TCAAttribution, TimingCostAgainstTheBestPriceInTheWindow) {
    TCAConfig cfg;
    cfg.pre_trade_window = std::chrono::minutes(5);
    cfg.post_trade_window = std::chrono::minutes(5);
    TransactionCostAnalyzer tca(cfg);
    const std::vector<Bar> bars = {
        bar(-20, 200.0, 50.0, 100.0),  // outside the window: ignored
        bar(-3, 101.0, 99.0, 100.0),
        bar(4, 102.0, 98.5, 100.0),
        bar(20, 200.0, 50.0, 100.0),  // outside the window: ignored
    };
    const auto timing = &TransactionCostMetrics::timing_cost;

    // Buy at 100 against the window low 98.5: 1.5 * 10 = 15.
    EXPECT_NEAR(cost(tca, fill(0, Side::BUY, 10, 100.0), bars, timing), 15.0, 1e-9);
    // Sell at 100 against the window high 102: 2 * 10 = 20.
    EXPECT_NEAR(cost(tca, fill(0, Side::SELL, 10, 100.0), bars, timing), 20.0, 1e-9);
    // A buy below every low in the window was already the best price: 0.
    EXPECT_EQ(cost(tca, fill(0, Side::BUY, 10, 90.0), bars, timing), 0.0);

    // Narrowing the window to +-2 minutes drops both inner bars.
    cfg.pre_trade_window = std::chrono::minutes(2);
    cfg.post_trade_window = std::chrono::minutes(2);
    TransactionCostAnalyzer narrow(cfg);
    EXPECT_EQ(cost(narrow, fill(0, Side::BUY, 10, 100.0), bars, timing), 0.0);
}

// ---------------------------------------------------------------------------
// Participation and reversion.
// ---------------------------------------------------------------------------

TEST(TCAAttribution, ParticipationUsesTheSameSymbolsBarWithinFiveMinutes) {
    TransactionCostAnalyzer tca{TCAConfig{}};
    const std::vector<Bar> bars = {bar(0, 100.0, 100.0, 100.0, 9000.0, "NQ"),
                                   bar(2, 100.0, 100.0, 100.0, 2000.0, "ES")};
    // 50 of 2000 lots on the ES bar two minutes later; the NQ bar is skipped.
    EXPECT_NEAR(cost(tca, fill(0, Side::BUY, 50, 100.0), bars,
                     &TransactionCostMetrics::participation_rate),
                0.025, 1e-12);
    // No ES bar within 5 minutes: 0.
    EXPECT_EQ(cost(tca, fill(10, Side::BUY, 50, 100.0), bars,
                   &TransactionCostMetrics::participation_rate),
              0.0);
}

TEST(TCAAttribution, ReversionIsTheCloseThirtyMinutesLaterAgainstTheFill) {
    TransactionCostAnalyzer tca{TCAConfig{}};
    const std::vector<Bar> bars = {bar(0, 100.0, 100.0, 100.0), bar(29, 120.0, 120.0, 120.0),
                                   bar(31, 99.0, 99.0, 99.0), bar(60, 80.0, 80.0, 80.0)};
    // First bar 30+ minutes after the fill closes at 99: (99 - 100) / 100.
    EXPECT_NEAR(cost(tca, fill(0, Side::BUY, 10, 100.0), bars,
                     &TransactionCostMetrics::price_reversion),
                -0.01, 1e-12);
}

// ---------------------------------------------------------------------------
// A sequence of fills.
// ---------------------------------------------------------------------------

TEST(TCAAttribution, SequenceWeightsEachFillByItsNotionalAndSkipsRolls) {
    TransactionCostAnalyzer tca{TCAConfig{}};
    const std::vector<Bar> bars = {bar(0, 101.0, 99.0, 100.0), bar(2, 104.0, 96.0, 100.0),
                                   bar(4, 100.0, 100.0, 100.0)};
    const auto a = fill(1, Side::BUY, 10, 100.0);   // notional 1,000
    const auto b = fill(3, Side::BUY, 30, 100.0);   // notional 3,000
    const auto roll = fill(3, Side::BUY, 500, 100.0, "ES", ExecutionType::ROLL);

    const double sa = cost(tca, a, bars, &TransactionCostMetrics::spread_cost);  // 10
    const double sb = cost(tca, b, bars, &TransactionCostMetrics::spread_cost);  // 120
    ASSERT_NEAR(sa, 10.0, 1e-9);
    ASSERT_NEAR(sb, 120.0, 1e-9);

    auto r = tca.analyze_trade_sequence({a, roll, b}, bars);
    ASSERT_TRUE(r.is_ok());
    // (10 * 1000 + 120 * 3000) / 4000 = 92.5. The ROLL fill is excluded entirely.
    EXPECT_NEAR(r.value().spread_cost, 92.5, 1e-9);
    EXPECT_EQ(r.value().num_child_orders, 2);
    EXPECT_EQ(r.value().execution_time, std::chrono::milliseconds(2 * 60 * 1000));
    // FIXME: participation_rate and price_reversion are computed per fill by
    // analyze_trade but never aggregated, so a sequence always reports 0 for both
    // (the existing TradeSequenceAnalysis check participation_rate < 0.5 passes
    // vacuously). The right value is the notional-weighted mean of the per-fill rates.
    EXPECT_EQ(r.value().participation_rate, 0.0);
    EXPECT_EQ(r.value().price_reversion, 0.0);
}

TEST(TCAAttribution, EmptyOrRollOnlySequenceIsAllZero) {
    TransactionCostAnalyzer tca{TCAConfig{}};
    const std::vector<Bar> bars = {bar(0, 101.0, 99.0, 100.0)};
    auto r = tca.analyze_trade_sequence(
        {fill(1, Side::BUY, 10, 100.0, "ES", ExecutionType::ROLL),
         fill(1, Side::BUY, 10, 100.0, "ES", ExecutionType::BORROW)},
        bars);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().num_child_orders, 0);
    EXPECT_EQ(r.value().spread_cost, 0.0);
    EXPECT_EQ(r.value().execution_time.count(), 0);
}

// ---------------------------------------------------------------------------
// Implementation shortfall.
// ---------------------------------------------------------------------------

TEST(TCAAttribution, ShortfallOnAPartlyFilledBuy) {
    TransactionCostAnalyzer tca{TCAConfig{}};
    const std::vector<Bar> bars = {bar(0, 100.0, 100.0, 100.0), bar(5, 101.0, 101.0, 101.0),
                                   bar(10, 105.0, 105.0, 105.0)};
    // Decided at minute 0 (arrival 100); 60 @ 101 and 20 @ 102 -> VWAP 101.25.
    auto r = tca.calculate_implementation_shortfall(
        target(100, 0), {fill(1, Side::BUY, 60, 101.0), fill(2, Side::BUY, 20, 102.0)}, bars);
    ASSERT_TRUE(r.is_ok());
    // FIXME: delay_cost is per share (VWAP - arrival = 1.25) while opportunity_cost
    // below is in currency (multiplied by the unfilled quantity). On the same scale as
    // opportunity_cost it would be 1.25 * 80 = 100.
    EXPECT_NEAR(r.value().delay_cost, 1.25, 1e-9);
    // 20 unfilled, final close 105 against arrival 100: 5 * 20 = 100.
    EXPECT_NEAR(r.value().opportunity_cost, 100.0, 1e-9);
}

TEST(TCAAttribution, ShortfallOnASellCountsFilledQuantityWithTheWrongSign) {
    // FIXME: fills carry filled_quantity as a magnitude (the side says which way),
    // but the shortfall subtracts it from a signed target. A sell target of -100
    // with 80 filled leaves "unfilled" = -100 - 80 = -180 instead of -20, so the
    // opportunity cost is 9x too large. Right value: (100 - 95) * 20 = 100.
    TransactionCostAnalyzer tca{TCAConfig{}};
    const std::vector<Bar> bars = {bar(0, 100.0, 100.0, 100.0), bar(10, 95.0, 95.0, 95.0)};
    auto r = tca.calculate_implementation_shortfall(
        target(-100, 0), {fill(1, Side::SELL, 40, 99.0), fill(2, Side::SELL, 40, 98.0)}, bars);
    ASSERT_TRUE(r.is_ok());
    // Sell delay: arrival - VWAP = 100 - 98.5.
    EXPECT_NEAR(r.value().delay_cost, 1.5, 1e-9);
    EXPECT_NEAR(r.value().opportunity_cost, 5.0 * 180.0, 1e-9);
}

TEST(TCAAttribution, ShortfallWithoutAnArrivalPriceIsZero) {
    TransactionCostAnalyzer tca{TCAConfig{}};
    const std::vector<Bar> bars = {bar(0, 100.0, 100.0, 100.0)};
    // Decided after the last bar: no arrival price, nothing is measured.
    auto r = tca.calculate_implementation_shortfall(target(100, 5),
                                                    {fill(6, Side::BUY, 10, 120.0)}, bars);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().delay_cost, 0.0);
    EXPECT_EQ(r.value().opportunity_cost, 0.0);
}

// ---------------------------------------------------------------------------
// Benchmarks.
// ---------------------------------------------------------------------------

TEST(TCAAttribution, BenchmarksAgainstVwapTwapAndArrival) {
    TransactionCostAnalyzer tca{TCAConfig{}};
    const std::vector<Bar> bars = {bar(0, 100.0, 100.0, 100.0, 1.0), bar(1, 101.0, 101.0, 101.0, 1.0),
                                   bar(2, 102.0, 102.0, 102.0, 1.0), bar(3, 103.0, 103.0, 103.0, 2.0),
                                   bar(4, 104.0, 104.0, 104.0, 9.0)};
    // Execution VWAP: (10 * 101 + 10 * 103) / 20 = 102.
    auto r = tca.analyze_benchmark_performance(
        {fill(1, Side::BUY, 10, 101.0), fill(3, Side::BUY, 10, 103.0)}, bars);
    ASSERT_TRUE(r.is_ok());
    const auto& m = r.value();
    // Market VWAP over minutes 1..3: (101 + 102 + 2 * 103) / 4 = 102.25.
    EXPECT_NEAR(m.at("vwap_performance"), (102.0 - 102.25) / 102.25, 1e-12);
    // TWAP over minutes 1..3: 102.
    EXPECT_NEAR(m.at("twap_performance"), 0.0, 1e-12);
    // Arrival: the first bar's close, 100.
    EXPECT_NEAR(m.at("arrival_price_performance"), 0.02, 1e-12);
}

TEST(TCAAttribution, BenchmarkFlagsSelectTheKeys) {
    TCAConfig cfg;
    cfg.use_vwap = false;
    cfg.use_twap = false;
    cfg.use_arrival_price = true;
    TransactionCostAnalyzer tca(cfg);
    auto r = tca.analyze_benchmark_performance({fill(1, Side::BUY, 10, 101.0)},
                                               {bar(0, 100.0, 100.0, 100.0)});
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value().count("arrival_price_performance"), 1u);

    auto empty = TransactionCostAnalyzer(TCAConfig{}).analyze_benchmark_performance({}, {bar(0, 1, 1, 1)});
    ASSERT_TRUE(empty.is_ok());
    EXPECT_TRUE(empty.value().empty());
}

TEST(TCAAttribution, VwapBenchmarkWithNoBarInTheExecutionWindowIsInfinite) {
    // FIXME: with no bar between the first and last fill, market VWAP is 0 and
    // vwap_performance divides by it, storing inf. TWAP guards the same case by
    // leaving its key out; VWAP should do the same.
    TransactionCostAnalyzer tca{TCAConfig{}};
    auto r = tca.analyze_benchmark_performance({fill(10, Side::BUY, 10, 101.0)},
                                               {bar(0, 100.0, 100.0, 100.0)});
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(std::isinf(r.value().at("vwap_performance")));
    EXPECT_EQ(r.value().count("twap_performance"), 0u);
}

// ---------------------------------------------------------------------------
// Config knobs the analyzer never reads.
// ---------------------------------------------------------------------------

TEST(TCAAttribution, UnreadConfigKnobsChangeNothing) {
    // FIXME: spread_factor, market_impact_coefficient, volatility_multiplier,
    // calculate_opportunity_costs, analyze_timing_costs and max_child_orders_analyzed
    // are declared in TCAConfig and never read by TransactionCostAnalyzer, so setting
    // them has no effect. Either wire them or delete them.
    TCAConfig off;
    off.spread_factor = 7.0;
    off.market_impact_coefficient = 7.0;
    off.volatility_multiplier = 7.0;
    off.calculate_opportunity_costs = false;
    off.analyze_timing_costs = false;
    off.max_child_orders_analyzed = 1;
    const std::vector<Bar> bars = {bar(0, 101.0, 99.0, 100.0), bar(2, 104.0, 96.0, 102.0)};
    const auto e = fill(1, Side::BUY, 10, 103.0);

    auto base = TransactionCostAnalyzer(TCAConfig{}).analyze_trade(e, bars);
    auto knobs = TransactionCostAnalyzer(off).analyze_trade(e, bars);
    ASSERT_TRUE(base.is_ok());
    ASSERT_TRUE(knobs.is_ok());
    EXPECT_EQ(knobs.value().spread_cost, base.value().spread_cost);
    EXPECT_EQ(knobs.value().market_impact, base.value().market_impact);
    EXPECT_EQ(knobs.value().timing_cost, base.value().timing_cost);
    EXPECT_GT(knobs.value().timing_cost, 0.0);  // computed although analyze_timing_costs=false

    auto is = TransactionCostAnalyzer(off).calculate_implementation_shortfall(
        target(100, 0), {fill(1, Side::BUY, 10, 101.0)}, bars);
    ASSERT_TRUE(is.is_ok());
    EXPECT_GT(is.value().opportunity_cost, 0.0);  // computed although calculate_opportunity_costs=false
}

// ---------------------------------------------------------------------------
// Report.
// ---------------------------------------------------------------------------

TEST(TCAAttribution, ReportShowsTheFiguresAndTheirTotal) {
    TransactionCostMetrics m;
    m.spread_cost = 1.5;
    m.market_impact = 2.25;
    m.timing_cost = 0.25;
    m.delay_cost = 1.0;
    m.participation_rate = 0.025;
    m.price_reversion = -0.01;
    m.num_child_orders = 3;
    m.execution_time = std::chrono::milliseconds(1500);

    const std::string report = TransactionCostAnalyzer(TCAConfig{}).generate_report(m);
    EXPECT_NE(report.find("  Spread Cost: 1.5000\n"), std::string::npos) << report;
    EXPECT_NE(report.find("  Market Impact: 2.2500\n"), std::string::npos);
    EXPECT_NE(report.find("  Total Cost: 5.0000\n"), std::string::npos);
    EXPECT_NE(report.find("  Participation Rate: 2.5000%\n"), std::string::npos);
    EXPECT_NE(report.find("  Number of Orders: 3\n"), std::string::npos);
    EXPECT_NE(report.find("  Execution Time: 1500ms\n"), std::string::npos);
    EXPECT_NE(report.find("  Price Reversion: -1.0000%\n"), std::string::npos);
    // Opportunity cost is printed only when non-zero.
    EXPECT_EQ(report.find("Opportunity Cost"), std::string::npos);

    m.opportunity_cost = 12.0;
    EXPECT_NE(TransactionCostAnalyzer(TCAConfig{}).generate_report(m).find("  Opportunity Cost: 12.0000\n"),
              std::string::npos);
}
