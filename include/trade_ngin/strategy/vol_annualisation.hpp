// include/trade_ngin/strategy/vol_annualisation.hpp
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
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
 */
struct VolAnnualisation {
    size_t bars = 0;
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
    if (n < 2) {
        return out;
    }
    const auto first = timestamps[timestamps.size() - n];
    const auto last = timestamps.back();
    const double span_seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(last - first).count();
    out.span_days = span_seconds / 86400.0;
    if (!(out.span_days > 0.0) || !std::isfinite(out.span_days)) {
        return out;
    }
    out.bars_per_year = static_cast<double>(n - 1) / (out.span_days / kDaysPerYear);
    out.factor = std::sqrt(out.bars_per_year);
    out.fallback = false;
    return out;
}

}  // namespace trade_ngin
