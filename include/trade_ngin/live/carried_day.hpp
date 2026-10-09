// include/trade_ngin/live/carried_day.hpp
#pragma once

#include <string>
#include <unordered_map>

#include "trade_ngin/data/session_classifier.hpp"

namespace trade_ngin {

/**
 * @brief The values a futures runner CARRIES into its positions file on a day with no session.
 *
 * HD 2026-09-18 (the Sunday positions CSV): with the MarketDataBus off at the load, a day with
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
struct CarriedForecasts {
    std::string session_date;
    std::unordered_map<std::string, double> forecasts;
};

/**
 * @brief Why a whole-book carry had no session, from the day's T-1 classification (T-7b-1 C7b R2,
 *        T-7a_CODE_REVIEW R2). A carry happens when no symbol has a T-1 price, so every verdict
 *        is a no-bar one: when any of them is a feed hole the reason is the FEED HOLE and its
 *        count (the dead Sundays 2026-05-17 and 05-24), otherwise a closure with the first
 *        verdict's reason ("holiday: ...", "not expected: ..."). The runners used to derive it
 *        from the weekday and the calendar, which named nothing on a feed-hole carry.
 */
inline std::string carried_day_reason(const T1Classification& t1) {
    if (t1.feed_hole > 0) {
        return "FEED HOLE: " + std::to_string(t1.feed_hole) +
               " symbol(s) normally print on this weekday";
    }
    if (t1.verdicts.empty()) return "closure";
    return "closure: " + t1.verdicts.front().reason;
}

namespace carried_day_detail {
inline std::string html_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}
}  // namespace carried_day_detail

/// `body` with the carried-day banner (the same note the positions file carries) at the top of
/// the report, inside the container EmailSender::generate_trading_report_body opens, or in front
/// of the body when the container is not found.
inline std::string flag_email_body_for_carried_day(const std::string& body,
                                                   const std::string& note) {
    static const std::string kContainer = "<div class=\"container\">\n";
    const std::string banner =
        "<div class=\"alert-note\" id=\"carried-day\">\n<strong>NO SESSION - BOOK CARRIED:</strong> " +
        carried_day_detail::html_escape(note) +
        ". No orders were generated; every position is the previous day's, marked at its last "
        "close.\n</div>\n";
    const auto at = body.find(kContainer);
    if (at == std::string::npos) return banner + body;
    std::string flagged = body;
    flagged.insert(at + kContainer.size(), banner);
    return flagged;
}

}  // namespace trade_ngin
