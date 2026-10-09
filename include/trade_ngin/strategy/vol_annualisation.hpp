// include/trade_ngin/strategy/vol_annualisation.hpp
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <string>
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

/**
 * @brief The annualisation of a per-bar volatility estimate, counted from the bars' dates.
 *
 * A per-bar standard deviation becomes an annual one by sqrt(bars a year). Carver's 16 is
 * sqrt(256), right for a five-bar week; a futures series that also prints a Sunday session row
 * has about 5.9 bars a week (about 307 a year), and 16 understates its annual vol by about
 * 8.6 percent. The rule counts the bars the series actually has:
 *
 *   window        the last `window` timestamps (the strategies pass the trailing
 *                 kVolAnnualisationWindowBars bars; see trailing_vol_annualisation below)
 *   span_days     (last - first) in days, fractional
 *   bars_per_year (bars - 1) / (span_days / 365.25), the returns per calendar year
 *   factor        sqrt(bars_per_year)
 *
 * Fallback to Carver's 16 when the count cannot be made: fewer than two timestamps in the
 * window, or a span that is not positive.
 *
 * `first` and `last` are the window's first and last bar timestamps (left at the epoch when the
 * window is empty); they only feed the VOL_ANNUALISATION log line below.
 */
struct VolAnnualisation {
    size_t bars = 0;
    Timestamp first{};
    Timestamp last{};
    double span_days = 0.0;
    double bars_per_year = 256.0;
    double factor = 16.0;
    bool fallback = true;
};

inline constexpr double kCarverAnnualisation = 16.0;  // sqrt(256)
inline constexpr double kDaysPerYear = 365.25;

inline VolAnnualisation vol_annualisation(const std::deque<Timestamp>& timestamps,
                                          size_t window) {
    VolAnnualisation out;
    const size_t n = std::min(window, timestamps.size());
    out.bars = n;
    if (n == 0) {
        return out;
    }
    out.first = timestamps[timestamps.size() - n];
    out.last = timestamps.back();
    if (n < 2) {
        return out;
    }
    // The span counts DATES: a loaded bar's timestamp sits some hours into its date, and not the
    // same hours on every date, so a span between two instants is not a whole number of days on
    // every window. The estimator (trend_estimator.hpp) counts the same whole dates.
    out.span_days = static_cast<double>((std::chrono::floor<std::chrono::days>(out.last) -
                                         std::chrono::floor<std::chrono::days>(out.first))
                                            .count());
    if (!(out.span_days > 0.0) || !std::isfinite(out.span_days)) {
        return out;
    }
    out.bars_per_year = static_cast<double>(n - 1) / (out.span_days / kDaysPerYear);
    out.factor = std::sqrt(out.bars_per_year);
    out.fallback = false;
    return out;
}

/**
 * @brief The count window of the strategies' annualisation (HD ruling 11, T-7b-3, review R-4).
 *
 * The bars a year are counted over the trailing 256 bars the estimator has for the symbol as of
 * the signal bar, never over the whole loaded history. The whole history differs by engine (live
 * loads 730 calendar days, about 600 bars; the backtest's history grows from its 256-bar warm-up to
 * the 756-bar cap; a 5-year and a 2-year backtest hold different histories on the same date), so a
 * count over it gave each engine its own factor on the same bars. Over the trailing 256 bars every
 * engine holding the same bars counts the same bars and gets the same factor.
 *
 * A series with fewer than 256 bars counts over what it has (all its bars; the VOL_ANNUALISATION
 * line then shows bars < 256), and fewer than two bars fall back to 16 as before.
 *
 * 256 is a literal, not read from a strategy parameter: it is the bar count HD ruled, the year of
 * daily bars behind Carver's 16 = sqrt(256), and it must be the same for every trend class (Fast's
 * longest EMA is 64, Slow's 512; vol_lookback_long is 252), which no per-strategy parameter is.
 */
inline constexpr size_t kVolAnnualisationWindowBars = 256;

/**
 * @brief The strategies' annualisation: vol_annualisation over the trailing
 * min(kVolAnnualisationWindowBars, estimator_bars) timestamps, where `estimator_bars` is the
 * number of bars the estimator reads (`prices.size()`).
 */
inline VolAnnualisation trailing_vol_annualisation(const std::deque<Timestamp>& timestamps,
                                                   size_t estimator_bars) {
    return vol_annualisation(timestamps, std::min(kVolAnnualisationWindowBars, estimator_bars));
}

/**
 * @brief The VOL_ANNUALISATION log line: one per symbol per signal computation, written where
 * the factor is computed, carrying the numbers the estimator was scaled by.
 *
 *   VOL_ANNUALISATION class=<C> strategy=<id> symbol=<S> signal_bar=<YYYY-MM-DD> bars=<n>
 *     first=<YYYY-MM-DD> last=<YYYY-MM-DD> span_days=<d> bars_per_year=<b> factor=<f>
 *     fallback=<0|1>
 *
 * Dates are UTC calendar dates (format_utc_date); "-" for an empty window. span_days,
 * bars_per_year and factor are printed with nine decimals. `signal_bar` is the last bar this
 * call delivered for the symbol, `last` the last bar of the counted window; they are the same
 * bar on both the live bulk load and the backtest's daily feed.
 */
inline std::string vol_annualisation_log_line(const std::string& strategy_class,
                                              const std::string& strategy_id,
                                              const std::string& symbol, Timestamp signal_bar,
                                              const VolAnnualisation& a) {
    char nums[160];
    std::snprintf(nums, sizeof(nums), " span_days=%.9f bars_per_year=%.9f factor=%.9f fallback=%d",
                  a.span_days, a.bars_per_year, a.factor, a.fallback ? 1 : 0);
    const bool empty = a.bars == 0;
    return "VOL_ANNUALISATION class=" + strategy_class + " strategy=" + strategy_id +
           " symbol=" + symbol + " signal_bar=" + core::format_utc_date(signal_bar) +
           " bars=" + std::to_string(a.bars) +
           " first=" + (empty ? std::string("-") : core::format_utc_date(a.first)) +
           " last=" + (empty ? std::string("-") : core::format_utc_date(a.last)) + nums;
}

}  // namespace trade_ngin
