// include/trade_ngin/strategy/trend_estimator.hpp
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace trade_ngin {
namespace trend_estimator {

/**
 * The trend sleeve's estimators at one signal bar, as a function of a FIXED trailing window of the
 * symbol's consumed bars and of nothing else: the volatility the position is sized on, the
 * volatility the forecast divides by, the attenuation, each EWMAC pair's scaled forecast and the
 * combined forecast.
 *
 * The window rule. Every value is computed from the last kWindowBars consumed bars ending at the
 * signal bar, and every recursion (the EWMA variance and mean, each EMA of the level, the
 * attenuation's smoothing) STARTS at the window's first bar. A symbol with fewer bars starts at its
 * first bar and counts what it has. Nothing is carried from one call to the next, so two engines
 * holding the same window compute the same numbers, whatever came before the window.
 *
 * The series. `returns[k]` is the return of window bar k against bar k-1, the adjusted change over
 * the raw previous close (0 on a contract-switch bar); `returns[0]` is not read. `level` is the
 * back-adjusted level, read only through the EMAs' differences. `close` is the raw close, the
 * price the forecast divides by. `day[k]` is the bar's date in days (any epoch), for the
 * annualisation count.
 *
 * The volatility. A mean-centred EWMA standard deviation of the returns: mean_1 = r_1 and
 * var_1 = max(0.1 r_1^2, 1e-6); then mean_k = lambda r_k + (1 - lambda) mean_k-1 and
 * var_k = max(lambda (r_k - mean_k)^2 + (1 - lambda) var_k-1, 1e-6), lambda = 2 / (span + 1).
 * sigma_short_k = sqrt(var_k) x factor_k clamped to [0.005, 5.0], with factor_k the square root of
 * the bars a year counted over the trailing 256 window bars ending at k (16 where it cannot be
 * counted). sigma_long is the mean of sigma_short over the trailing 2,520 values including the
 * signal bar, and sigma = 0.7 sigma_short + 0.3 sigma_long. The forecast's volatility is the same
 * variance with the fixed factor 16 and its own 2,520-value mean and blend: one recursion, two
 * annualisations.
 *
 * The forecast. For each pair (fast, slow): raw = (EMA_fast - EMA_slow) / (close x
 * forecast_sigma / 16), the EMAs on the adjusted level and seeded at the window's first level;
 * scaled = raw x attenuation x the pair's fixed scalar, capped at +/-20. The attenuation is
 * 2 - 1.5 q, q the share of the trailing 2,520 sigma_short values at or below the bar's own,
 * defined once 252 values exist and smoothed by a 10-bar EWMA seeded at its first value; 1 before
 * that, and 1 throughout when `attenuate` is off. The combined forecast is the equal-weight mean
 * of the scaled forecasts times the diversification multiplier, capped at +/-20.
 *
 * Every series is computed for every window bar (the counts behind q through a rank tree, so the
 * whole window costs about n log n), which also gives the extremes of the window's sizing
 * volatility and of its combined forecast before the multiplier, for the trace lines.
 */
inline constexpr std::size_t kWindowBars = 3200;
inline constexpr std::size_t kLongRunValues = 2520;
inline constexpr std::size_t kAnnualisationBars = 256;
inline constexpr std::size_t kAttenuationMinValues = 252;
// The calendar days an engine loads before the first bar it feeds, so that a symbol's window holds
// kWindowBars consumed bars at the first sized day: 3,200 bars of a five-bar week are about 12.7
// years; 13.2 years are loaded. A symbol with a shorter history has what it has.
inline constexpr int kHistoryCalendarDays = 4820;
inline constexpr double kFixedAnnualisation = 16.0;
inline constexpr double kForecastCap = 20.0;

/// One symbol's window of consumed bars, oldest first, all four vectors the same length.
struct Window {
    std::vector<double> day;      // bar date in days
    std::vector<double> close;    // raw close
    std::vector<double> level;    // back-adjusted level
    std::vector<double> returns;  // returns[k] against bar k-1; returns[0] unused
};

struct Estimate {
    std::size_t window_bars = 0;   // bars the window held (below kWindowBars: a short history)
    std::size_t long_values = 0;   // sigma_short values behind sigma_long (at most kLongRunValues)
    bool valid = false;            // false when the window has fewer than two bars
    double factor = kFixedAnnualisation;  // sqrt(bars a year) at the signal bar
    double sigma_short = 0.0;
    double sigma_long = 0.0;
    double sigma = 0.0;            // the volatility the position is sized on
    double forecast_sigma = 0.0;   // the volatility the forecast divides by
    double attenuation = 1.0;
    std::vector<double> ema_fast;  // per pair, at the signal bar
    std::vector<double> ema_slow;
    std::vector<double> scaled;    // per pair: after the scalar, the attenuation and the cap
    double mean_scaled = 0.0;      // the equal-weight mean of the scaled forecasts (before the multiplier)
    double combined = 0.0;         // the combined forecast before any rule on it
    double smoothed_quantile = 0.0;  // the smoothed q behind the attenuation (2/3 before it is defined)
    // Over the window's bars that have a return: the extremes of the sizing volatility and of the
    // mean scaled forecast.
    double sigma_min = 0.0;
    double sigma_max = 0.0;
    double mean_scaled_min = 0.0;
    double mean_scaled_max = 0.0;
    double jump_sigma_daily = 0.0; // 99th percentile of the trailing sigma_short values, each as a daily sd
};

/// The fixed forecast scalar of an EWMAC pair (fast, 4 x fast), fast in {2, 4, 8, 16, 32, 64}.
/// Returns 0 for any other pair: such a pair has no scalar and a configuration naming it is refused.
double forecast_scalar(int fast, int slow);

/// True when every pair has a fixed scalar.
bool pairs_supported(const std::vector<std::pair<int, int>>& pairs, std::string* unsupported = nullptr);

/**
 * @brief The estimators at the window's last bar.
 * @param window the trailing consumed bars (at most kWindowBars are read: the last ones)
 * @param vol_span the EWMA span of the volatility (32 for the trend sleeve, 16 for the fast one)
 * @param pairs the EWMAC pairs
 * @param fdm the forecast diversification multiplier for this number of pairs
 * @param attenuate whether the attenuation acts
 */
Estimate estimate(const Window& window, int vol_span, const std::vector<std::pair<int, int>>& pairs,
                  double fdm, bool attenuate = true);

}  // namespace trend_estimator
}  // namespace trade_ngin
