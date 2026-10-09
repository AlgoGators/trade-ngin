// src/strategy/trend_estimator.cpp
#include "trade_ngin/strategy/trend_estimator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace trade_ngin {
namespace trend_estimator {

namespace {

constexpr double kVarianceSeedShare = 0.1;
constexpr double kVarianceFloor = 1e-6;
constexpr double kSigmaFloor = 0.005;
constexpr double kSigmaCap = 5.0;
constexpr double kShortWeight = 0.7;
constexpr double kDaysPerYear = 365.25;
constexpr double kAttenuationSmoothSpan = 10.0;
constexpr double kJumpPercentile = 99.0;

double clamp_sigma(double daily_sd, double factor) {
    return std::clamp(daily_sd * factor, kSigmaFloor, kSigmaCap);
}

// The mean of x over [lo, hi], both inside the defined range.
double mean_of(const std::vector<double>& x, std::size_t lo, std::size_t hi) {
    double sum = 0.0;
    for (std::size_t k = lo; k <= hi; ++k) sum += x[k];
    return sum / static_cast<double>(hi - lo + 1);
}

// The percentile of the values by linear interpolation between order statistics.
double percentile(std::vector<double> values, double pct) {
    if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(values.begin(), values.end());
    const double pos = (pct / 100.0) * static_cast<double>(values.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
    const std::size_t hi = std::min(lo + 1, values.size() - 1);
    const double frac = pos - static_cast<double>(lo);
    return values[lo] + frac * (values[hi] - values[lo]);
}

}  // namespace

double forecast_scalar(int fast, int slow) {
    if (slow != 4 * fast) return 0.0;
    switch (fast) {
        case 2:
            return 12.1;
        case 4:
            return 8.53;
        case 8:
            return 5.95;
        case 16:
            return 4.10;
        case 32:
            return 2.79;
        case 64:
            return 1.91;
        default:
            return 0.0;
    }
}

double equity_slow_ruled(double combined, const std::vector<double>& scaled,
                         const std::vector<std::pair<int, int>>& pairs,
                         const std::vector<std::pair<int, int>>& rule_pairs) {
    if (!(combined < 0.0)) return combined;
    for (const auto& rule_pair : rule_pairs) {
        bool negative = false;
        for (std::size_t k = 0; k < pairs.size() && k < scaled.size(); ++k) {
            if (pairs[k] == rule_pair) {
                negative = scaled[k] < 0.0;
                break;
            }
        }
        if (!negative) return 0.0;
    }
    return combined;
}

bool pairs_supported(const std::vector<std::pair<int, int>>& pairs, std::string* unsupported) {
    for (const auto& [fast, slow] : pairs) {
        if (forecast_scalar(fast, slow) > 0.0) continue;
        if (unsupported != nullptr) {
            *unsupported = "(" + std::to_string(fast) + ", " + std::to_string(slow) + ")";
        }
        return false;
    }
    return true;
}

std::vector<std::pair<int, int>> pairs_after_removal(const std::vector<std::pair<int, int>>& pairs,
                                                     const std::vector<std::pair<int, int>>& removed,
                                                     std::string* refusal) {
    auto refuse = [&](const std::string& why) {
        if (refusal != nullptr) *refusal = why;
        return std::vector<std::pair<int, int>>{};
    };
    auto name = [](const std::pair<int, int>& pair) {
        return "(" + std::to_string(pair.first) + ", " + std::to_string(pair.second) + ")";
    };
    for (std::size_t k = 0; k < pairs.size(); ++k) {
        if (k > 0 && pairs[k].first <= pairs[k - 1].first) {
            return refuse("the sleeve's pairs are not in order of speed, fastest first");
        }
    }
    for (std::size_t k = 0; k < removed.size(); ++k) {
        if (std::find(pairs.begin(), pairs.end(), removed[k]) == pairs.end()) {
            return refuse("the pair " + name(removed[k]) + " is not one of the sleeve's pairs");
        }
        if (std::find(removed.begin(), removed.begin() + static_cast<long>(k), removed[k]) !=
            removed.begin() + static_cast<long>(k)) {
            return refuse("the pair " + name(removed[k]) + " is named twice");
        }
    }
    if (removed.size() >= pairs.size()) {
        return refuse("no pair is left (a contract with no rule left is taken out of the "
                      "universe, not listed here)");
    }
    for (std::size_t k = 0; k < removed.size(); ++k) {
        if (std::find(removed.begin(), removed.end(), pairs[k]) == removed.end()) {
            return refuse("the pairs removed are not the fastest " + std::to_string(removed.size()) +
                          ": " + name(pairs[k]) + " is kept while a slower pair is removed");
        }
    }
    return std::vector<std::pair<int, int>>(pairs.begin() + static_cast<long>(removed.size()),
                                            pairs.end());
}

Estimate estimate(const Window& window, int vol_span, const std::vector<std::pair<int, int>>& pairs,
                  double fdm, bool attenuate) {
    Estimate out;
    const std::size_t total = window.close.size();
    if (total == 0 || window.day.size() != total || window.level.size() != total ||
        window.returns.size() != total || vol_span <= 0) {
        return out;
    }
    // The window: the last kWindowBars bars. `first` is the window's first bar in the caller's
    // vectors; every index below is a window index (0 = the window's first bar).
    const std::size_t first = total > kWindowBars ? total - kWindowBars : 0;
    const std::size_t n = total - first;
    const std::size_t last = n - 1;
    out.window_bars = n;
    out.ema_fast.assign(pairs.size(), 0.0);
    out.ema_slow.assign(pairs.size(), 0.0);
    out.scaled.assign(pairs.size(), 0.0);
    auto day = [&](std::size_t k) { return window.day[first + k]; };
    auto close = [&](std::size_t k) { return window.close[first + k]; };
    auto level = [&](std::size_t k) { return window.level[first + k]; };
    auto ret = [&](std::size_t k) { return window.returns[first + k]; };

    out.valid = n >= 2;

    // The annualisation factor of every window bar: bars a year over its trailing 256 window bars.
    std::vector<double> factor(n, kFixedAnnualisation);
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t s = k + 1 > kAnnualisationBars ? k + 1 - kAnnualisationBars : 0;
        const double days = day(k) - day(s);
        if (days > 0.0) {
            factor[k] = std::sqrt(static_cast<double>(k - s) / (days / kDaysPerYear));
        }
    }
    out.factor = factor[last];

    // The mean-centred EWMA variance from the window's first return, and the two annualisations of
    // its square root. Index 0 has no return: every series below is defined from index 1.
    const double lambda = 2.0 / (static_cast<double>(vol_span) + 1.0);
    std::vector<double> sizing_short(n, 0.0);    // sigma_short, annualised by the counted factor
    std::vector<double> forecast_short(n, 0.0);  // the same daily sd, annualised by 16
    if (n >= 2) {
        double mean = ret(1);
        double variance = std::max(kVarianceSeedShare * mean * mean, kVarianceFloor);
        sizing_short[1] = clamp_sigma(std::sqrt(variance), factor[1]);
        forecast_short[1] = clamp_sigma(std::sqrt(variance), kFixedAnnualisation);
        for (std::size_t k = 2; k < n; ++k) {
            const double r = ret(k);
            mean = lambda * r + (1.0 - lambda) * mean;
            const double deviation = r - mean;
            variance =
                std::max(lambda * deviation * deviation + (1.0 - lambda) * variance, kVarianceFloor);
            const double daily_sd = std::sqrt(variance);
            sizing_short[k] = clamp_sigma(daily_sd, factor[k]);
            forecast_short[k] = clamp_sigma(daily_sd, kFixedAnnualisation);
        }
    }

    // The long-run means and the two blends at every bar: the trailing kLongRunValues window
    // positions including the bar, counting the defined values (index 0 is not one).
    auto long_lo = [](std::size_t k) {
        const std::size_t lo = k + 1 > kLongRunValues ? k + 1 - kLongRunValues : 0;
        return std::max<std::size_t>(lo, 1);
    };
    // Running sums give every bar's mean in one pass; the signal bar's own two means, the ones a
    // position is sized on, are summed directly over their values.
    std::vector<double> sizing_sigma(n, 0.0);
    std::vector<double> forecast_sigma(n, 0.0);
    {
        std::vector<double> sizing_sum(n + 1, 0.0);
        std::vector<double> forecast_sum(n + 1, 0.0);
        for (std::size_t k = 1; k < n; ++k) {
            sizing_sum[k + 1] = sizing_sum[k] + sizing_short[k];
            forecast_sum[k + 1] = forecast_sum[k] + forecast_short[k];
        }
        for (std::size_t k = 1; k < n; ++k) {
            const std::size_t lo = long_lo(k);
            const double count = static_cast<double>(k - lo + 1);
            const double sizing_long =
                k == last ? mean_of(sizing_short, lo, k) : (sizing_sum[k + 1] - sizing_sum[lo]) / count;
            const double forecast_long = k == last ? mean_of(forecast_short, lo, k)
                                                   : (forecast_sum[k + 1] - forecast_sum[lo]) / count;
            sizing_sigma[k] = kShortWeight * sizing_short[k] + (1.0 - kShortWeight) * sizing_long;
            forecast_sigma[k] = kShortWeight * forecast_short[k] + (1.0 - kShortWeight) * forecast_long;
        }
    }

    // The attenuation at every bar: q_c = the share of the trailing sigma_short values at or below
    // bar c's, defined once kAttenuationMinValues values exist, smoothed by a 10-bar EWMA seeded at
    // its first value. The counts come from a rank tree over the window's values.
    std::vector<double> attenuation(n, 1.0);
    double smoothed = 2.0 / 3.0;
    if (attenuate && n >= 2) {
        std::vector<double> sorted(sizing_short.begin() + 1, sizing_short.end());
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        auto rank_of = [&](double v) {
            return static_cast<std::size_t>(std::lower_bound(sorted.begin(), sorted.end(), v) -
                                            sorted.begin()) + 1;
        };
        std::vector<long> tree(sorted.size() + 1, 0);
        auto add = [&](std::size_t rank, long delta) {
            for (; rank < tree.size(); rank += rank & (~rank + 1)) tree[rank] += delta;
        };
        auto at_or_below = [&](std::size_t rank) {
            long total_count = 0;
            for (; rank > 0; rank -= rank & (~rank + 1)) total_count += tree[rank];
            return total_count;
        };
        const double q_lambda = 2.0 / (kAttenuationSmoothSpan + 1.0);
        bool seeded = false;
        for (std::size_t c = 1; c < n; ++c) {
            const std::size_t rank = rank_of(sizing_short[c]);
            add(rank, 1);
            const std::size_t lo = long_lo(c);
            // the value that left the trailing window when bar c entered it
            if (c > kLongRunValues) add(rank_of(sizing_short[c - kLongRunValues]), -1);
            if (c < kAttenuationMinValues) continue;
            const double q =
                static_cast<double>(at_or_below(rank)) / static_cast<double>(c - lo + 1);
            smoothed = seeded ? q_lambda * q + (1.0 - q_lambda) * smoothed : q;
            seeded = true;
            attenuation[c] = 2.0 - 1.5 * smoothed;
        }
    }
    out.attenuation = attenuation[last];
    out.smoothed_quantile = smoothed;

    // Each pair's EMAs of the adjusted level (seeded at the window's first level), its scaled
    // forecast at every bar, and the equal-weight mean of the scaled forecasts.
    std::vector<double> mean_scaled(n, 0.0);
    for (std::size_t p = 0; p < pairs.size(); ++p) {
        const double lf = 2.0 / (static_cast<double>(pairs[p].first) + 1.0);
        const double ls = 2.0 / (static_cast<double>(pairs[p].second) + 1.0);
        const double scalar = forecast_scalar(pairs[p].first, pairs[p].second);
        double ef = level(0);
        double es = level(0);
        for (std::size_t k = 1; k < n; ++k) {
            const double pk = level(k);
            ef = lf * pk + (1.0 - lf) * ef;
            es = ls * pk + (1.0 - ls) * es;
            const double raw = (ef - es) / (close(k) * forecast_sigma[k] / kFixedAnnualisation);
            double scaled = raw * attenuation[k] * scalar;
            if (!std::isfinite(scaled)) scaled = 0.0;
            scaled = std::clamp(scaled, -kForecastCap, kForecastCap);
            mean_scaled[k] += scaled;
            if (k == last) out.scaled[p] = scaled;
        }
        out.ema_fast[p] = ef;
        out.ema_slow[p] = es;
    }
    if (n < 2) {
        return out;  // one bar: no return, no volatility, no forecast
    }
    if (!pairs.empty()) {
        for (std::size_t k = 1; k < n; ++k) mean_scaled[k] /= static_cast<double>(pairs.size());
    }

    const std::size_t lo_last = long_lo(last);
    out.long_values = last - lo_last + 1;
    out.sigma_short = sizing_short[last];
    out.sigma_long = mean_of(sizing_short, lo_last, last);
    out.sigma = sizing_sigma[last];
    out.forecast_sigma = forecast_sigma[last];
    out.mean_scaled = mean_scaled[last];
    out.combined = std::clamp(fdm * mean_scaled[last], -kForecastCap, kForecastCap);
    out.sigma_min = *std::min_element(sizing_sigma.begin() + 1, sizing_sigma.end());
    out.sigma_max = *std::max_element(sizing_sigma.begin() + 1, sizing_sigma.end());
    out.mean_scaled_min = *std::min_element(mean_scaled.begin() + 1, mean_scaled.end());
    out.mean_scaled_max = *std::max_element(mean_scaled.begin() + 1, mean_scaled.end());

    // The jump volatility's input: the 99th percentile of the trailing sigma_short values, each
    // taken back to a daily standard deviation by its own factor.
    {
        std::vector<double> daily;
        daily.reserve(out.long_values);
        for (std::size_t k = lo_last; k <= last; ++k) daily.push_back(sizing_short[k] / factor[k]);
        out.jump_sigma_daily = percentile(std::move(daily), kJumpPercentile);
    }
    return out;
}

}  // namespace trend_estimator
}  // namespace trade_ngin
