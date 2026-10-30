// include/trade_ngin/live/carried_day.hpp
#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/session_classifier.hpp"

namespace trade_ngin {

/**
 * @brief The values a futures runner CARRIES into its positions file on a day with no session.
 *
 * With the MarketDataBus off at the load, a day with
 * no session (the previous calendar day was a Saturday or a holiday, so no T-1 close exists)
 * feeds the strategies nothing and runs no rebalance, so a strategy's forecast, volatility and
 * EMAs are empty that day. The per-strategy positions file keeps the real details instead of
 * zero rows: the held quantities, each symbol's last mark, and each sleeve's last computed
 * forecasts from the previous session, with a note that no session occurred and the values are
 * carried, not computed.
 */

/// A symbol's last mark: its latest loaded close and that bar's UTC date (YYYY-MM-DD).
struct CarriedMark {
    double close{0.0};
    std::string date;
};

/// One sleeve's last computed forecasts: its trading.signals rows of the latest stored run date
/// before today, and that date (YYYY-MM-DD, UTC). Empty when the sleeve has no stored signals.
/// The date is the date of the RUN that stored the rows, not of the bars they were computed
/// from: a run computes its forecasts from the bars through the day before it.
struct CarriedForecasts {
    std::string session_date;
    std::unordered_map<std::string, double> forecasts;
};

/// How the carried positions file describes a stored forecast of run date `run_date`
/// (YYYY-MM-DD): "computed by the run of 2026-05-02 from bars through 2026-05-01". Empty for
/// an empty date; a text that is not a date is named as given, with no bar date.
inline std::string carried_forecast_label(const std::string& run_date) {
    if (run_date.empty())
        return std::string();
    std::chrono::system_clock::time_point run_day;
    if (!core::parse_utc_date(run_date, run_day))
        return "computed by the run of " + run_date;
    return "computed by the run of " + run_date + " from bars through " +
           core::format_utc_date(run_day - std::chrono::hours(24));
}

/// The futures runners' log line for one sleeve's carried forecasts on a day with no session:
/// how many were read and, in the carried file's own words (carried_forecast_label), which run
/// computed them and the last bar date that run read. The stored date is a RUN date: the line
/// used to end "from <date>", which read as the date of the bars.
inline std::string carried_forecasts_log_line(const std::string& sleeve, std::size_t count,
                                              const std::string& run_date) {
    const std::string label = carried_forecast_label(run_date);
    return "Carried forecasts for " + sleeve + ": " + std::to_string(count) +
           (label.empty() ? std::string(" (no stored session)") : " " + label);
}

/**
 * @brief Why a whole-book carry had no session, from the day's T-1 classification. A carry
 *        happens when no symbol has a T-1 price, so every verdict
 *        is a no-bar one: when any of them is a feed hole the reason is the FEED HOLE and its
 *        count (the dead Sundays 2026-05-17 and 05-24), otherwise a closure with the first
 *        verdict's reason ("holiday: ...", "not expected: ..."). The weekday and the calendar
 *        alone would name nothing on a feed-hole carry.
 */
inline std::string carried_day_reason(const T1Classification& t1) {
    if (t1.feed_hole > 0) {
        return "FEED HOLE: " + std::to_string(t1.feed_hole) +
               " symbol(s) normally print on this weekday";
    }
    if (t1.verdicts.empty()) return "closure";
    return "closure: " + t1.verdicts.front().reason;
}

}  // namespace trade_ngin
