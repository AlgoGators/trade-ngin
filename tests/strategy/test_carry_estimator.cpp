// The carry rules (trade-ngin #185, strategy/carry_estimator.hpp): both branches and their signs,
// annualisation, the risk adjustment, smoothing, the scalar and the cap, the missing-price rule and
// the window rule. Carver's two worked examples (AFTS pages 221 to 223) are the first test.
#include <gtest/gtest.h>

#include <cmath>

#include "trade_ngin/strategy/carry_estimator.hpp"
#include "trade_ngin/strategy/trend_estimator.hpp"

using namespace trade_ngin;
using namespace trade_ngin::carry_estimator;

namespace {

CarryQuote quote(double held, double other, int months) {
    return CarryQuote{held, other, months, "H", "O"};
}

/// n bars, every bar quoted with `q`, a constant close and forecast volatility.
Window flat(std::size_t n, const CarryQuote& q, double close, double sigma) {
    Window w;
    w.quote.assign(n, q);
    w.close.assign(n, close);
    w.forecast_sigma.assign(n, sigma);
    w.forecast_sigma[0] = 0.0;  // bar 0 has no volatility
    return w;
}

}  // namespace

TEST(CarryEstimator, CarversTwoWorkedExamples) {
    // S&P 500 micro, holding December (the front): December 4578.50, March 4572.00, sigma 16%.
    const CarryQuote sp = quote(4578.50, 4572.00, 3);
    EXPECT_DOUBLE_EQ(raw_carry(sp), 6.50);
    EXPECT_DOUBLE_EQ(expiry_difference(sp), 0.25);
    EXPECT_DOUBLE_EQ(annualised_raw_carry(sp), 26.0);
    const Estimate e_sp = estimate(flat(50, sp, 4578.50, 0.16), default_spans());
    ASSERT_TRUE(e_sp.active);
    EXPECT_NEAR(e_sp.forecast, 26.0 / (0.16 * 4578.50), 1e-15);
    EXPECT_NEAR(e_sp.forecast, 0.035, 0.0005);  // the book's 0.035
    for (double s : e_sp.scaled) EXPECT_NEAR(s, 1.1, 0.05);  // the book's +1.1
    // WTI crude, holding December (not the front): November 62.61, December 62.09, sigma 28%.
    const CarryQuote cl = quote(62.09, 62.61, -1);
    EXPECT_NEAR(raw_carry(cl), 0.52, 1e-12);
    EXPECT_NEAR(expiry_difference(cl), 1.0 / 12.0, 1e-15);
    EXPECT_NEAR(annualised_raw_carry(cl), 6.24, 1e-10);
    const Estimate e_cl = estimate(flat(50, cl, 62.09, 0.28), default_spans());
    EXPECT_NEAR(e_cl.forecast, 0.358, 0.001);  // the book's 0.358 (0.3589 truncated)
    for (double s : e_cl.scaled) EXPECT_NEAR(s, 10.8, 0.05);  // the book's +10.8
    EXPECT_NEAR(e_cl.mean_scaled, 30.0 * 6.24 / (0.28 * 62.09), 1e-9);
}

TEST(CarryEstimator, TheSignOfEachBranch) {
    // Nearer above held (backwardation): long carry. Nearer below held (contango): short.
    EXPECT_GT(raw_carry(quote(100.0, 101.0, -1)), 0.0);
    EXPECT_LT(raw_carry(quote(100.0, 99.0, -1)), 0.0);
    // Held the front, further below held: long carry. Further above held: short.
    EXPECT_GT(raw_carry(quote(100.0, 99.0, 3)), 0.0);
    EXPECT_LT(raw_carry(quote(100.0, 101.0, 3)), 0.0);
    EXPECT_EQ(raw_carry(quote(100.0, 99.0, 0)), 0.0);
    EXPECT_EQ(annualised_raw_carry(quote(100.0, 99.0, 0)), 0.0);
    // The sign reaches the scaled forecast.
    EXPECT_GT(estimate(flat(10, quote(100.0, 101.0, -1), 100.0, 0.2), default_spans()).mean_scaled, 0.0);
    EXPECT_LT(estimate(flat(10, quote(100.0, 101.0, 3), 100.0, 0.2), default_spans()).mean_scaled, 0.0);
}

TEST(CarryEstimator, AnnualisationDividesByTheMonthsBetweenTheContracts) {
    // The same one-point spread is a year's carry of 12 points a month apart, 4 a quarter, 2 at six.
    EXPECT_DOUBLE_EQ(annualised_raw_carry(quote(100.0, 101.0, -1)), 12.0);
    EXPECT_DOUBLE_EQ(annualised_raw_carry(quote(101.0, 100.0, 3)), 4.0);
    EXPECT_DOUBLE_EQ(annualised_raw_carry(quote(101.0, 100.0, 6)), 2.0);
    EXPECT_DOUBLE_EQ(annualised_raw_carry(quote(100.0, 101.0, -2)), 6.0);
}

TEST(CarryEstimator, EachSpanIsAnEwmaSeededAtTheFirstForecast) {
    // Forecast 0.1 for bars 1..5, then 0.3: every span seeds at 0.1, then moves by its lambda.
    Window w = flat(12, quote(100.0, 100.0 + 0.1 * 0.2 * 100.0 / 12.0, -1), 100.0, 0.2);
    for (std::size_t k = 6; k < 12; ++k) w.quote[k] = quote(100.0, 100.0 + 0.3 * 0.2 * 100.0 / 12.0, -1);
    const Estimate e = estimate(w, default_spans());
    ASSERT_EQ(e.smoothed.size(), 4u);
    for (std::size_t s = 0; s < 4; ++s) {
        const double lambda = 2.0 / (default_spans()[s] + 1.0);
        double hand = 0.1;
        for (int k = 6; k < 12; ++k) hand = lambda * 0.3 + (1.0 - lambda) * hand;
        EXPECT_NEAR(e.smoothed[s], hand, 1e-12) << default_spans()[s];
        EXPECT_NEAR(e.scaled[s], 30.0 * hand, 1e-10) << default_spans()[s];
    }
    // the faster span is nearer the new value
    EXPECT_GT(e.smoothed[0], e.smoothed[1]);
    EXPECT_GT(e.smoothed[1], e.smoothed[2]);
    EXPECT_GT(e.smoothed[2], e.smoothed[3]);
    EXPECT_NEAR(e.forecast, 0.3, 1e-12);
    EXPECT_EQ(e.forecast_bars, 11u);
}

TEST(CarryEstimator, EachSpanIsCappedAtTwenty) {
    // annualised 120 over a divisor of 100 x 0.1 = 12 per bar, x 30 = 360: capped at +20 and -20.
    EXPECT_EQ(estimate(flat(5, quote(100.0, 110.0, -1), 100.0, 0.1), default_spans()).mean_scaled, 20.0);
    EXPECT_EQ(estimate(flat(5, quote(100.0, 90.0, -1), 100.0, 0.1), default_spans()).mean_scaled, -20.0);
    // The cap acts per span, after smoothing: one span over, the others under.
    Window w = flat(40, quote(100.0, 100.0 + 0.5 * 0.1 * 100.0 / 12.0, -1), 100.0, 0.1);  // x 30 = 15
    w.quote[39] = quote(100.0, 100.0 + 4.0 * 0.1 * 100.0 / 12.0, -1);                     // x 30 = 120
    const Estimate e = estimate(w, default_spans());
    EXPECT_EQ(e.scaled[0], 20.0) << "Carry5 moves a third of the way: 15 + (120 - 15) / 3 = 50";
    EXPECT_EQ(e.scaled[1], 20.0) << "Carry20: 30 x (0.5 + 3.5 x 2 / 21) = 25";
    EXPECT_NEAR(e.scaled[2], 30.0 * (0.5 + (2.0 / 61.0) * 3.5), 1e-9) << "Carry60 under the cap";
    EXPECT_LT(e.scaled[3], 20.0);
    EXPECT_NEAR(e.mean_scaled, (e.scaled[0] + e.scaled[1] + e.scaled[2] + e.scaled[3]) / 4.0, 1e-12);
}

TEST(CarryEstimator, ADayWithNoQuoteKeepsTheLastKnownRawCarry) {
    // Quoted on bars 0..4; bars 5..9 have no second-contract price and their volatility doubles.
    Window w = flat(10, quote(100.0, 101.0, -1), 100.0, 0.2);
    for (std::size_t k = 5; k < 10; ++k) {
        w.quote[k].reset();
        w.forecast_sigma[k] = 0.4;
    }
    const Estimate e = estimate(w, {5});
    ASSERT_TRUE(e.active);
    EXPECT_FALSE(e.quoted);
    EXPECT_DOUBLE_EQ(e.annualised, 12.0) << "the last known annualised raw carry stands";
    EXPECT_DOUBLE_EQ(e.divisor, 100.0 * 0.4) << "divided by its own bar's divisor";
    EXPECT_DOUBLE_EQ(e.forecast, 12.0 / 40.0);
    double hand = 12.0 / 20.0;
    for (int k = 5; k < 10; ++k) hand = (1.0 / 3.0) * (12.0 / 40.0) + (2.0 / 3.0) * hand;
    EXPECT_NEAR(e.smoothed[0], hand, 1e-12);
    EXPECT_EQ(e.forecast_bars, 9u);
    // A new quote replaces it the day it comes.
    w.quote[9] = quote(100.0, 102.0, -1);
    EXPECT_DOUBLE_EQ(estimate(w, {5}).annualised, 24.0);
    EXPECT_TRUE(estimate(w, {5}).quoted);
}

TEST(CarryEstimator, NoQuoteInTheWindowIsTrendOnly) {
    Window w = flat(20, quote(100.0, 101.0, -1), 100.0, 0.2);
    for (auto& q : w.quote) q.reset();
    const Estimate none = estimate(w, default_spans());
    EXPECT_FALSE(none.active);
    EXPECT_EQ(none.mean_scaled, 0.0);
    EXPECT_EQ(none.forecast_bars, 0u);
    // A first quote on bar 15: the forecasts start there, each span seeded at its first value.
    w.quote[15] = quote(100.0, 101.0, -1);
    const Estimate later = estimate(w, default_spans());
    ASSERT_TRUE(later.active);
    EXPECT_EQ(later.forecast_bars, 5u);
    for (double s : later.smoothed) EXPECT_NEAR(s, 12.0 / 20.0, 1e-15);
    // A window of one bar has no volatility: no forecast.
    EXPECT_FALSE(estimate(flat(1, quote(100.0, 101.0, -1), 100.0, 0.2), default_spans()).active);
    // Vectors of different lengths are no window.
    Window bad = flat(5, quote(100.0, 101.0, -1), 100.0, 0.2);
    bad.close.pop_back();
    EXPECT_FALSE(estimate(bad, default_spans()).active);
}

TEST(CarryEstimator, OnlyTheTrendWindowsBarsAreRead) {
    // A history longer than the window reads its last kWindowBars bars: the bars before do not
    // count, whatever they hold, and the EWMA starts at the window's first bar.
    const std::size_t extra = 400;
    Window long_w = flat(trend_estimator::kWindowBars + extra, quote(100.0, 101.0, -1), 100.0, 0.2);
    for (std::size_t k = 0; k < extra; ++k) long_w.quote[k] = quote(100.0, 140.0, -1);
    for (std::size_t k = 0; k < long_w.quote.size(); k += 7) long_w.forecast_sigma[k] = 0.25;
    Window tail;
    tail.quote.assign(long_w.quote.begin() + extra, long_w.quote.end());
    tail.close.assign(long_w.close.begin() + extra, long_w.close.end());
    tail.forecast_sigma.assign(long_w.forecast_sigma.begin() + extra, long_w.forecast_sigma.end());
    const Estimate a = estimate(long_w, default_spans());
    const Estimate b = estimate(tail, default_spans());
    ASSERT_EQ(a.smoothed.size(), b.smoothed.size());
    for (std::size_t s = 0; s < a.smoothed.size(); ++s) EXPECT_EQ(a.smoothed[s], b.smoothed[s]);
    EXPECT_EQ(a.mean_scaled, b.mean_scaled);
}

TEST(CarryEstimator, TheTrendEstimatorHandsOutItsForecastVolatilityAndNothingElseChanges) {
    trend_estimator::Window w;
    double level = 100.0;
    for (int k = 0; k < 400; ++k) {
        const double r = 0.01 * std::sin(0.3 * k) + 0.002;
        w.day.push_back(k * 1.4);
        w.returns.push_back(k == 0 ? 0.0 : r);
        level *= 1.0 + (k == 0 ? 0.0 : r);
        w.close.push_back(level);
        w.level.push_back(level);
    }
    const std::vector<std::pair<int, int>> pairs{{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}};
    const auto plain = trend_estimator::estimate(w, 32, pairs, 1.26);
    std::vector<double> sigma_bars{1.0, 2.0};
    const auto with = trend_estimator::estimate(w, 32, pairs, 1.26, true, &sigma_bars);
    ASSERT_EQ(sigma_bars.size(), w.close.size());
    EXPECT_EQ(sigma_bars[0], 0.0);
    EXPECT_EQ(sigma_bars.back(), plain.forecast_sigma);
    EXPECT_EQ(with.forecast_sigma, plain.forecast_sigma);
    EXPECT_EQ(with.combined, plain.combined);
    EXPECT_EQ(with.sigma, plain.sigma);
    EXPECT_EQ(with.scaled, plain.scaled);
    EXPECT_EQ(with.mean_scaled, plain.mean_scaled);
}

TEST(CarryBlend, TheWeightsAreTable51) {
    // Every row of table 51 that has both styles, and its carry-only rows (trend weight 0).
    struct Row {
        int trend;
        int carry;
        double trend_each;
        double carry_each;
    };
    for (const Row& r : {Row{6, 4, 0.10, 0.10}, Row{5, 4, 0.12, 0.10}, Row{4, 4, 0.15, 0.10},
                         Row{3, 4, 0.20, 0.10}, Row{2, 4, 0.30, 0.10}, Row{2, 3, 0.30, 0.13333},
                         Row{1, 3, 0.60, 0.13333}}) {
        const Blend b = blend(1.0, r.trend, 1.0, r.carry, 0.4);
        EXPECT_NEAR(b.trend_weight, r.trend_each, 5e-5) << r.trend << "+" << r.carry;
        EXPECT_NEAR(b.carry_weight, r.carry_each, 5e-5) << r.trend << "+" << r.carry;
        EXPECT_NEAR(b.trend_weight * r.trend + b.carry_weight * r.carry, 1.0, 1e-12);
        EXPECT_EQ(b.rules, r.trend + r.carry);
    }
    for (const auto& [carry, each] : {std::pair<int, double>{3, 0.3333}, {2, 0.5}, {1, 1.0}}) {
        const Blend b = blend(5.0, 6, 1.0, carry, 1.0);
        EXPECT_EQ(b.trend_weight, 0.0);
        EXPECT_EQ(b.trend_rules, 0) << "a style of weight 0 counts no rule";
        EXPECT_NEAR(b.carry_weight, each, 5e-5);
        EXPECT_EQ(b.rules, carry);
    }
}

TEST(CarryBlend, TheFdmIsTable52ByTheRulesTheInstrumentKeeps) {
    EXPECT_EQ(rule_count_fdm(10), 1.35);
    EXPECT_EQ(rule_count_fdm(9), 1.34);
    EXPECT_EQ(rule_count_fdm(8), 1.32);
    EXPECT_EQ(rule_count_fdm(7), 1.29);
    EXPECT_EQ(rule_count_fdm(6), 1.27);
    EXPECT_EQ(rule_count_fdm(4), 1.23);
    EXPECT_EQ(rule_count_fdm(3), 1.03);
    EXPECT_EQ(rule_count_fdm(1), 1.00);
    EXPECT_EQ(rule_count_fdm(13), 1.39);
    EXPECT_EQ(rule_count_fdm(22), 1.55);
    EXPECT_EQ(rule_count_fdm(25), 1.69);
    EXPECT_EQ(rule_count_fdm(40), 2.00);
    EXPECT_EQ(rule_count_fdm(55), 2.00);
    EXPECT_EQ(rule_count_fdm(0), 0.0);
    EXPECT_EQ(rule_count_fdm(23), 0.0) << "not printed: refused, never guessed";
    // The four rows the speed limit leaves (ticket #185's table).
    EXPECT_EQ(blend(1.0, 6, 1.0, 4, 0.4).fdm, 1.35);
    EXPECT_EQ(blend(1.0, 5, 1.0, 4, 0.4).fdm, 1.34);
    EXPECT_EQ(blend(1.0, 4, 1.0, 4, 0.4).fdm, 1.32);
    EXPECT_EQ(blend(1.0, 3, 1.0, 4, 0.4).fdm, 1.29);
}

TEST(CarryBlend, TheBlendedForecastByHandAndItsCap) {
    // Trend average 8, carry average -5, ten rules: 1.35 x (0.6 x 8 + 0.4 x -5) = 1.35 x 2.8 = 3.78.
    const Blend b = blend(8.0, 6, -5.0, 4, 0.4);
    EXPECT_NEAR(b.trend_part, 4.8, 1e-12);
    EXPECT_NEAR(b.carry_part, -2.0, 1e-12);
    EXPECT_NEAR(b.forecast, 1.35 * 2.8, 1e-12);
    // Three trend speeds left: 1.29 x (0.6 x 12 + 0.4 x 10) = 14.448.
    EXPECT_NEAR(blend(12.0, 3, 10.0, 4, 0.4).forecast, 1.29 * 11.2, 1e-12);
    // The cap: 1.35 x (0.6 x 20 + 0.4 x 20) = 27 is 20; and -20 the other way.
    EXPECT_EQ(blend(20.0, 6, 20.0, 4, 0.4).forecast, 20.0);
    EXPECT_EQ(blend(-20.0, 6, -20.0, 4, 0.4).forecast, -20.0);
    // Carry only: the carry average times table 52 for the carry spans.
    EXPECT_NEAR(blend(9.0, 6, 5.0, 4, 1.0).forecast, 1.23 * 5.0, 1e-12);
}
