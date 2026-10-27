// The trend sleeve's estimators (LOOP_SPEC v6.2 sections 1 and 2.4 to 2.6; trend_estimator.hpp):
// a pure function of the last W consumed bars, every recursion started at the window's first bar.
//   - the volatility: a mean-centred EWMA standard deviation, the variance seeded at 0.1 r^2 and
//     floored at 1e-6, annualised bar by bar and clamped to [0.005, 5]; its long-run mean over the
//     trailing 2,520 values including the signal bar; the 0.7 / 0.3 blend; the forecast's own
//     volatility from the same variance at the fixed factor 16;
//   - the fixed forecast scalars; the attenuation 2 - 1.5 q, q counted over the trailing values and
//     smoothed over ten bars from its first value; the caps at +/-20; the diversification multiplier;
//   - the window: bars before the last W are not read.
// Every expected value is recomputed here by the plain definition (a direct loop, a direct count).
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "trade_ngin/strategy/trend_estimator.hpp"

using namespace trade_ngin;
using trend_estimator::Estimate;
using trend_estimator::Window;

namespace {

const std::vector<std::pair<int, int>> kTrendPairs = {{2, 8},   {4, 16},   {8, 32},
                                                      {16, 64}, {32, 128}, {64, 256}};

// A deterministic series of n bars: closes from the given returns (r[0] unused), one bar a calendar
// day unless `days` is given; the level is the close (no contract switch).
Window window_of(const std::vector<double>& returns, double first_close = 100.0,
                 const std::vector<double>& days = {}) {
    Window w;
    double close = first_close;
    for (size_t k = 0; k < returns.size(); ++k) {
        if (k > 0) close *= 1.0 + returns[k];
        w.close.push_back(close);
        w.level.push_back(close);
        w.returns.push_back(k == 0 ? 0.0 : returns[k]);
        w.day.push_back(days.empty() ? static_cast<double>(k) : days[k]);
    }
    return w;
}

// A reproducible pseudo-random return series (a linear congruential generator; no library state).
std::vector<double> noisy_returns(size_t n, double scale, unsigned seed = 12345u) {
    std::vector<double> r(n, 0.0);
    unsigned state = seed;
    for (size_t k = 1; k < n; ++k) {
        state = state * 1664525u + 1013904223u;
        const double u = static_cast<double>(state >> 8) / static_cast<double>(1u << 24);  // [0, 1)
        // a slow swell in the scale so that the volatility's rank moves
        const double swell = 1.0 + 0.6 * std::sin(static_cast<double>(k) / 97.0);
        r[k] = scale * swell * (u - 0.5);
    }
    return r;
}

// The plain definition of the short-run volatility series (annualised by `factor_of(k)`).
template <class FactorOf>
std::vector<double> short_vol(const std::vector<double>& r, int span, FactorOf factor_of) {
    std::vector<double> out(r.size(), 0.0);
    const double lambda = 2.0 / (span + 1.0);
    double mean = r[1];
    double var = std::max(0.1 * r[1] * r[1], 1e-6);
    out[1] = std::clamp(std::sqrt(var) * factor_of(1), 0.005, 5.0);
    for (size_t k = 2; k < r.size(); ++k) {
        mean = lambda * r[k] + (1.0 - lambda) * mean;
        var = std::max(lambda * (r[k] - mean) * (r[k] - mean) + (1.0 - lambda) * var, 1e-6);
        out[k] = std::clamp(std::sqrt(var) * factor_of(k), 0.005, 5.0);
    }
    return out;
}

// One bar a calendar day: the factor of bar k over its trailing 256 bars.
double daily_factor(size_t k) {
    if (k == 0) return 16.0;
    const size_t s = k + 1 > 256 ? k + 1 - 256 : 0;
    return std::sqrt(static_cast<double>(k - s) / (static_cast<double>(k - s) / 365.25));
}

}  // namespace

TEST(TrendEstimator, TheFixedScalarsAreTableTwentyNines) {
    EXPECT_DOUBLE_EQ(trend_estimator::forecast_scalar(2, 8), 12.1);
    EXPECT_DOUBLE_EQ(trend_estimator::forecast_scalar(4, 16), 8.53);
    EXPECT_DOUBLE_EQ(trend_estimator::forecast_scalar(8, 32), 5.95);
    EXPECT_DOUBLE_EQ(trend_estimator::forecast_scalar(16, 64), 4.10);
    EXPECT_DOUBLE_EQ(trend_estimator::forecast_scalar(32, 128), 2.79);
    EXPECT_DOUBLE_EQ(trend_estimator::forecast_scalar(64, 256), 1.91);
    // A pair outside the table has no scalar, and a configuration naming it is not supported.
    EXPECT_EQ(trend_estimator::forecast_scalar(1, 4), 0.0);
    EXPECT_EQ(trend_estimator::forecast_scalar(8, 30), 0.0);
    std::string which;
    EXPECT_TRUE(trend_estimator::pairs_supported(kTrendPairs));
    EXPECT_FALSE(trend_estimator::pairs_supported({{1, 4}, {2, 8}}, &which));
    EXPECT_EQ(which, "(1, 4)");
}

// Five bars, by hand. Returns r1..r4 = +1%, -2%, +0.5%, +3%, span 3 (lambda 0.5), one bar a day.
//   mean_1 = 0.01, var_1 = max(0.1 x 0.0001, 1e-6) = 1e-5
//   mean_2 = -0.005, dev -0.015, var_2 = 0.5 x 0.000225 + 0.5 x 1e-5 = 0.0001175
//   mean_3 = 0, dev 0.005, var_3 = 0.5 x 0.000025 + 0.5 x 0.0001175 = 0.00007125
//   mean_4 = 0.015, dev 0.015, var_4 = 0.5 x 0.000225 + 0.5 x 0.00007125 = 0.000148125
// factor = sqrt(365.25) on every bar from the second (k bars over k days).
TEST(TrendEstimator, TheVolatilityOfFiveBarsByHand) {
    const std::vector<double> r = {0.0, 0.01, -0.02, 0.005, 0.03};
    const Estimate e = trend_estimator::estimate(window_of(r), 3, {{2, 8}}, 1.0);
    ASSERT_TRUE(e.valid);
    EXPECT_EQ(e.window_bars, 5u);
    EXPECT_EQ(e.long_values, 4u);
    const double f = std::sqrt(365.25);
    EXPECT_NEAR(e.factor, f, 1e-12);
    const double s1 = std::sqrt(1e-5) * f, s2 = std::sqrt(0.0001175) * f,
                 s3 = std::sqrt(0.00007125) * f, s4 = std::sqrt(0.000148125) * f;
    EXPECT_NEAR(e.sigma_short, s4, 1e-12);
    EXPECT_NEAR(e.sigma_long, (s1 + s2 + s3 + s4) / 4.0, 1e-12);
    EXPECT_NEAR(e.sigma, 0.7 * s4 + 0.3 * (s1 + s2 + s3 + s4) / 4.0, 1e-12);
    // The forecast's volatility: the same variances at the fixed 16.
    const double g1 = std::sqrt(1e-5) * 16, g2 = std::sqrt(0.0001175) * 16,
                 g3 = std::sqrt(0.00007125) * 16, g4 = std::sqrt(0.000148125) * 16;
    EXPECT_NEAR(e.forecast_sigma, 0.7 * g4 + 0.3 * (g1 + g2 + g3 + g4) / 4.0, 1e-12);
    EXPECT_EQ(e.attenuation, 1.0) << "fewer than 252 values: no attenuation yet";
}

TEST(TrendEstimator, TheVarianceIsFlooredAndTheVolatilityClamped) {
    // No move at all: the variance sits on its floor 1e-6, a daily sd of 0.001.
    const Estimate flat = trend_estimator::estimate(window_of(std::vector<double>(40, 0.0)), 32,
                                                    {{2, 8}}, 1.0);
    EXPECT_NEAR(flat.sigma_short, 0.001 * std::sqrt(365.25), 1e-12);
    EXPECT_NEAR(flat.forecast_sigma, 0.001 * 16.0, 1e-12);
    // Enormous moves: capped at 5.0.
    std::vector<double> wild(40, 0.0);
    for (size_t k = 1; k < wild.size(); ++k) wild[k] = (k % 2 == 0) ? 0.9 : -0.45;
    const Estimate capped = trend_estimator::estimate(window_of(wild), 32, {{2, 8}}, 1.0);
    EXPECT_EQ(capped.sigma_short, 5.0);
    // A single bar has no return: not valid.
    EXPECT_FALSE(trend_estimator::estimate(window_of({0.0}), 32, {{2, 8}}, 1.0).valid);
}

// 3,000 bars against the plain definition: every value of the short-run volatility annualised by
// its own bar's factor, the long-run mean over the trailing 2,520 values including the signal bar,
// the quantile by a direct count, its ten-bar smoothing from the first value (bar 252).
TEST(TrendEstimator, ThreeThousandBarsAgainstThePlainDefinition) {
    const size_t n = 3000;
    const auto r = noisy_returns(n, 0.03);
    const Estimate e = trend_estimator::estimate(window_of(r), 32, kTrendPairs, 1.26);
    ASSERT_TRUE(e.valid);
    const auto sizing = short_vol(r, 32, daily_factor);
    const auto forecast = short_vol(r, 32, [](size_t) { return 16.0; });
    const size_t last = n - 1;
    const size_t lo = last + 1 - 2520;
    double sum = 0.0, fsum = 0.0;
    for (size_t k = lo; k <= last; ++k) {
        sum += sizing[k];
        fsum += forecast[k];
    }
    EXPECT_EQ(e.long_values, 2520u);
    EXPECT_NEAR(e.sigma_short, sizing[last], 1e-13);
    EXPECT_NEAR(e.sigma_long, sum / 2520.0, 1e-13);
    EXPECT_NEAR(e.sigma, 0.7 * sizing[last] + 0.3 * sum / 2520.0, 1e-13);
    EXPECT_NEAR(e.forecast_sigma, 0.7 * forecast[last] + 0.3 * fsum / 2520.0, 1e-13);

    // The attenuation, by a direct count at every bar from the 252nd value on.
    double smoothed = 0.0;
    bool seeded = false;
    for (size_t c = 252; c <= last; ++c) {
        const size_t from = std::max<size_t>(c + 1 > 2520 ? c + 1 - 2520 : 0, 1);
        size_t at_or_below = 0;
        for (size_t k = from; k <= c; ++k) at_or_below += sizing[k] <= sizing[c] ? 1 : 0;
        const double q = static_cast<double>(at_or_below) / static_cast<double>(c - from + 1);
        smoothed = seeded ? (2.0 / 11.0) * q + (9.0 / 11.0) * smoothed : q;
        seeded = true;
    }
    EXPECT_NEAR(e.smoothed_quantile, smoothed, 1e-13);
    EXPECT_NEAR(e.attenuation, 2.0 - 1.5 * smoothed, 1e-13);
    EXPECT_GT(e.attenuation, 0.5);
    EXPECT_LT(e.attenuation, 2.0);

    // Each pair: EMAs seeded at the first level, over the raw close times the forecast's
    // volatility over 16, times the attenuation and the pair's scalar; the combined forecast.
    const Window w = window_of(r);
    double total = 0.0;
    for (size_t p = 0; p < kTrendPairs.size(); ++p) {
        const double lf = 2.0 / (kTrendPairs[p].first + 1.0), ls = 2.0 / (kTrendPairs[p].second + 1.0);
        double ef = w.level[0], es = w.level[0];
        for (size_t k = 1; k < n; ++k) {
            ef = lf * w.level[k] + (1.0 - lf) * ef;
            es = ls * w.level[k] + (1.0 - ls) * es;
        }
        const double scaled = std::clamp(
            (ef - es) / (w.close[last] * e.forecast_sigma / 16.0) * e.attenuation *
                trend_estimator::forecast_scalar(kTrendPairs[p].first, kTrendPairs[p].second),
            -20.0, 20.0);
        EXPECT_NEAR(e.scaled[p], scaled, 1e-10 * std::max(1.0, std::abs(scaled))) << "pair " << p;
        total += e.scaled[p];
    }
    EXPECT_NEAR(e.mean_scaled, total / 6.0, 1e-12);
    EXPECT_NEAR(e.combined, std::clamp(1.26 * total / 6.0, -20.0, 20.0), 1e-12);
}

TEST(TrendEstimator, WithoutTheAttenuationTheMultiplierIsOne) {
    const auto r = noisy_returns(1200, 0.03);
    const Estimate on = trend_estimator::estimate(window_of(r), 32, kTrendPairs, 1.26, true);
    const Estimate off = trend_estimator::estimate(window_of(r), 32, kTrendPairs, 1.26, false);
    EXPECT_EQ(off.attenuation, 1.0);
    EXPECT_NE(on.attenuation, 1.0);
    EXPECT_EQ(on.sigma, off.sigma);
    for (size_t p = 0; p < kTrendPairs.size(); ++p) {
        if (std::abs(on.scaled[p]) < 20.0 && std::abs(off.scaled[p]) < 20.0) {
            EXPECT_NEAR(on.scaled[p], off.scaled[p] * on.attenuation, 1e-12);
        }
    }
}

// A strong one-way trend at low volatility drives every scaled forecast, and the combined one, to
// the cap.
TEST(TrendEstimator, TheForecastsAreCappedAtTwenty) {
    std::vector<double> up(400, 0.004);
    up[0] = 0.0;
    const Estimate e = trend_estimator::estimate(window_of(up), 32, kTrendPairs, 1.26);
    for (double scaled : e.scaled) EXPECT_LE(scaled, 20.0);
    EXPECT_EQ(e.scaled[0], 20.0);
    EXPECT_EQ(e.combined, 20.0);
    std::vector<double> down(400, -0.004);
    down[0] = 0.0;
    EXPECT_EQ(trend_estimator::estimate(window_of(down), 32, kTrendPairs, 1.26).combined, -20.0);
}

// The window rule: the estimate of a long history is the estimate of its last W bars, bit for bit,
// whatever came before them; and a bar inside the window does matter.
TEST(TrendEstimator, OnlyTheLastWBarsAreRead) {
    const size_t n = trend_estimator::kWindowBars + 700;
    const auto r = noisy_returns(n, 0.03);
    const Window whole = window_of(r);
    Window tail;
    const size_t first = n - trend_estimator::kWindowBars;
    tail.close.assign(whole.close.begin() + first, whole.close.end());
    tail.level.assign(whole.level.begin() + first, whole.level.end());
    tail.returns.assign(whole.returns.begin() + first, whole.returns.end());
    tail.day.assign(whole.day.begin() + first, whole.day.end());
    const Estimate a = trend_estimator::estimate(whole, 32, kTrendPairs, 1.26);
    const Estimate b = trend_estimator::estimate(tail, 32, kTrendPairs, 1.26);
    EXPECT_EQ(a.window_bars, trend_estimator::kWindowBars);
    EXPECT_EQ(a.sigma, b.sigma);
    EXPECT_EQ(a.forecast_sigma, b.forecast_sigma);
    EXPECT_EQ(a.attenuation, b.attenuation);
    EXPECT_EQ(a.combined, b.combined);
    EXPECT_EQ(a.scaled, b.scaled);

    // Different bars before the window: nothing moves.
    Window other = whole;
    for (size_t k = 0; k < first; ++k) {
        other.close[k] *= 3.0;
        other.level[k] *= 3.0;
        other.returns[k] *= -2.0;
    }
    const Estimate c = trend_estimator::estimate(other, 32, kTrendPairs, 1.26);
    EXPECT_EQ(a.sigma, c.sigma);
    EXPECT_EQ(a.combined, c.combined);

    // A different level inside the window, 2,000 bars back: the slow EMAs still carry it.
    Window inside = whole;
    for (size_t k = first; k < n - 2000; ++k) inside.level[k] += 5.0;
    const Estimate d = trend_estimator::estimate(inside, 32, kTrendPairs, 1.26);
    EXPECT_EQ(a.sigma, d.sigma) << "the returns are untouched";
    EXPECT_NE(a.ema_slow[5], d.ema_slow[5]);
}

// The annualisation factor is counted per bar over its trailing 256 bars: a six-bar week from the
// 300th bar on gives the last bar the six-bar factor, and gives the bars of the five-bar stretch
// theirs inside the long-run mean.
TEST(TrendEstimator, EachBarIsAnnualisedByItsOwnFactor) {
    const size_t n = 700;
    std::vector<double> days(n, 0.0);
    for (size_t k = 1; k < n; ++k) {
        const bool six = k >= 300;
        // five bars a week: a two-day gap after every fifth bar; six bars: a one-day gap after every sixth
        const size_t in_week = six ? (k - 300) % 6 : k % 5;
        days[k] = days[k - 1] + 1.0 + (in_week == 0 ? (six ? 1.0 : 2.0) : 0.0);
    }
    std::vector<double> r(n, 0.0);
    for (size_t k = 1; k < n; ++k) r[k] = (k % 2 == 0) ? 0.01 : -0.01;
    const Estimate e = trend_estimator::estimate(window_of(r, 100.0, days), 32, {{2, 8}}, 1.0);
    const double span = days[n - 1] - days[n - 256];
    EXPECT_NEAR(e.factor, std::sqrt(255.0 / (span / 365.25)), 1e-12);
    auto factor_of = [&](size_t k) {
        const size_t s = k + 1 > 256 ? k + 1 - 256 : 0;
        const double d = days[k] - days[s];
        return d > 0.0 ? std::sqrt(static_cast<double>(k - s) / (d / 365.25)) : 16.0;
    };
    const auto sizing = short_vol(r, 32, factor_of);
    double sum = 0.0;
    for (size_t k = 1; k < n; ++k) sum += sizing[k];
    EXPECT_NEAR(e.sigma_long, sum / static_cast<double>(n - 1), 1e-13);
    EXPECT_NE(factor_of(250), factor_of(n - 1));
}

// The equity slow rule on one bar: a negative combined forecast stands only when every rule pair's
// scaled forecast is negative.
TEST(TrendEstimator, TheEquitySlowRule) {
    using trade_ngin::trend_estimator::equity_slow_ruled;
    const std::vector<std::pair<int, int>> pairs = {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}};
    const std::vector<std::pair<int, int>> slow = {{32, 128}, {64, 256}};
    EXPECT_EQ(equity_slow_ruled(-7.0, {-9, -9, -9, -9, -3, -1}, pairs, slow), -7.0);
    EXPECT_EQ(equity_slow_ruled(-7.0, {-9, -9, -9, -9, -3, 0.5}, pairs, slow), 0.0);
    EXPECT_EQ(equity_slow_ruled(-7.0, {-9, -9, -9, -9, 0.5, -3}, pairs, slow), 0.0);
    EXPECT_EQ(equity_slow_ruled(-7.0, {-9, -9, -9, -9, -3, 0.0}, pairs, slow), 0.0)
        << "a slow speed at exactly 0 is not negative";
    EXPECT_EQ(equity_slow_ruled(4.0, {9, 9, 9, 9, -3, -1}, pairs, slow), 4.0);
    EXPECT_EQ(equity_slow_ruled(0.0, {0, 0, 0, 0, 1, 1}, pairs, slow), 0.0);
    // a rule pair the sleeve does not carry counts as not negative
    EXPECT_EQ(equity_slow_ruled(-7.0, {-9, -9, -9, -9}, {{2, 8}, {4, 16}, {8, 32}, {16, 64}}, slow), 0.0);
}

// Trading rules removed from a contract by cost (strategy nine, "Removing expensive trading
// rules"): the rules removed are the contract's fastest, and what is left is the sleeve's slowest.
TEST(TrendEstimator, PairsAfterRemovalLeavesTheSlowestPairs) {
    using trade_ngin::trend_estimator::pairs_after_removal;
    using Pairs = std::vector<std::pair<int, int>>;
    std::string why = "unset";
    EXPECT_EQ(pairs_after_removal(kTrendPairs, {{2, 8}}, &why),
              (Pairs{{4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}}));
    EXPECT_EQ(why, "unset") << "a usable removal writes no refusal";
    EXPECT_EQ(pairs_after_removal(kTrendPairs, {{2, 8}, {4, 16}}),
              (Pairs{{8, 32}, {16, 64}, {32, 128}, {64, 256}}));
    // the order the list names them in does not matter
    EXPECT_EQ(pairs_after_removal(kTrendPairs, {{8, 32}, {2, 8}, {4, 16}}),
              (Pairs{{16, 64}, {32, 128}, {64, 256}}));
    EXPECT_EQ(pairs_after_removal(kTrendPairs, {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}}),
              (Pairs{{64, 256}}));
    EXPECT_EQ(pairs_after_removal(kTrendPairs, {}), kTrendPairs);
}

TEST(TrendEstimator, PairsAfterRemovalRefusesWhatTheMultiplierTableDoesNotName) {
    using trade_ngin::trend_estimator::pairs_after_removal;
    auto refused = [](const std::vector<std::pair<int, int>>& pairs,
                      const std::vector<std::pair<int, int>>& removed, const std::string& named) {
        std::string why;
        EXPECT_TRUE(pairs_after_removal(pairs, removed, &why).empty());
        EXPECT_NE(why.find(named), std::string::npos) << why;
    };
    // a slower rule removed while a faster one is kept: the table has no multiplier for that set
    refused(kTrendPairs, {{4, 16}}, "(2, 8) is kept while a slower pair is removed");
    refused(kTrendPairs, {{2, 8}, {8, 32}}, "(4, 16) is kept while a slower pair is removed");
    refused(kTrendPairs, {{64, 256}}, "(2, 8) is kept while a slower pair is removed");
    // every rule removed: the contract leaves the universe, it is not listed
    refused(kTrendPairs, kTrendPairs, "no pair is left");
    refused(kTrendPairs, {{128, 512}}, "(128, 512) is not one of the sleeve's pairs");
    refused(kTrendPairs, {{2, 8}, {2, 8}}, "(2, 8) is named twice");
    // a sleeve whose pairs are not written fastest first cannot say which are its fastest
    refused({{4, 16}, {2, 8}, {8, 32}}, {{2, 8}}, "not in order of speed");
}

// The estimator on the pairs left: each is scaled and capped exactly as among all six, they weigh
// equally, and the multiplier passed is applied once.
TEST(TrendEstimator, TheCombinedForecastOfThePairsLeftIsTheirEqualWeightMeanTimesTheMultiplier) {
    std::vector<double> returns(900, 0.0);
    unsigned state = 2463534242u;
    for (auto& r : returns) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        r = 0.012 * (static_cast<double>(state % 100000u) / 100000.0 - 0.5) + 0.0006;
    }
    const Window w = window_of(returns);
    const Estimate all = trend_estimator::estimate(w, 32, kTrendPairs, 1.26);
    ASSERT_EQ(all.scaled.size(), 6u);
    const double table[] = {0.0, 1.0, 1.03, 1.08, 1.13, 1.19, 1.26};
    for (std::size_t removed = 1; removed <= 5; ++removed) {
        const std::vector<std::pair<int, int>> fastest(kTrendPairs.begin(),
                                                       kTrendPairs.begin() + static_cast<long>(removed));
        const auto left = trend_estimator::pairs_after_removal(kTrendPairs, fastest);
        ASSERT_EQ(left.size(), 6 - removed);
        const double fdm = table[left.size()];
        const Estimate e = trend_estimator::estimate(w, 32, left, fdm);
        ASSERT_EQ(e.scaled.size(), left.size());
        double sum = 0.0;
        for (std::size_t k = 0; k < left.size(); ++k) {
            EXPECT_EQ(e.scaled[k], all.scaled[k + removed]) << "pair " << k + removed;
            sum += all.scaled[k + removed];
        }
        const double mean = sum / static_cast<double>(left.size());
        EXPECT_EQ(e.mean_scaled, mean);
        EXPECT_EQ(e.combined, std::clamp(fdm * mean, -20.0, 20.0));
        EXPECT_NE(e.combined, all.combined) << "the fixture does not tell the sets apart";
        // nothing but the forecast combination moves
        EXPECT_EQ(e.sigma, all.sigma);
        EXPECT_EQ(e.forecast_sigma, all.forecast_sigma);
        EXPECT_EQ(e.attenuation, all.attenuation);
    }
}
