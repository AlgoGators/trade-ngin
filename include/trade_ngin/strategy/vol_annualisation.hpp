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
 *   window        the last `window` timestamps (the same bars the estimator reads)
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
    const double span_seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(out.last - out.first).count();
    out.span_days = span_seconds / 86400.0;
    if (!(out.span_days > 0.0) || !std::isfinite(out.span_days)) {
        return out;
    }
    out.bars_per_year = static_cast<double>(n - 1) / (out.span_days / kDaysPerYear);
    out.factor = std::sqrt(out.bars_per_year);
    out.fallback = false;
    return out;
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
