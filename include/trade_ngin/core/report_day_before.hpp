// include/trade_ngin/core/report_day_before.hpp
//
// The banners about the day before. The settled day's weekday and the calendar choose the
// TITLE; whether prices arrived chooses the FORM:
//   closed   no prices at all (the book was carried): the banner, the red "No Executions" box and the yellow
//            note; the settled table is hidden;
//   partial  some held rows have no price or an unaccepted one: the banner only, the table shown;
//   none     nothing is missing: no banner, whatever the weekday or the calendar says.
// A weekday that is not a holiday has no title: closed gives the red box and the yellow note only.
#pragma once

#include <cstdio>
#include <string>

namespace trade_ngin {

enum class DayBeforeForm { None, Partial, Closed };

struct DayBeforeBanner {
    DayBeforeForm form{DayBeforeForm::None};
    std::string banner_html;   ///< the yellow banner (empty: none)
    std::string red_box_text;  ///< the inside of the red "No Executions" box (closed form only)
    std::string yellow_note;   ///< the inside of the yellow note (closed form only)
    bool hide_table() const { return form == DayBeforeForm::Closed; }
};

/// 0 = Sunday ... 6 = Saturday, of "YYYY-MM-DD"; -1 if it does not parse.
inline int report_weekday_of(const std::string& ymd) {
    if (ymd.size() < 10) return -1;
    int y = 0, m = 0, d = 0;
    if (std::sscanf(ymd.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return -1;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = static_cast<long>(era) * 146097 + static_cast<long>(doe) - 719468;
    return static_cast<int>(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday
}

namespace day_before_detail {
inline std::string yellow_banner(const std::string& title, const std::string& paragraph,
                                 const std::string& last_line) {
    return "<div style=\"background: linear-gradient(135deg, #fef3c7 0%, #fde68a 100%); "
           "border: 2px solid #f59e0b; border-radius: 8px; padding: 20px 30px; margin: 20px 0 "
           "30px 0; box-shadow: 0 4px 6px rgba(0,0,0,0.1);\">\n"
           "<h2 style=\"margin: 0 0 10px 0; color: #92400e; font-size: 20px; border-bottom: "
           "2px solid #92400e; padding-bottom: 8px; display: inline-block;\">Yesterday was " +
           title + "</h2>\n<p style=\"margin: 15px 0 5px 0; color: #78350f; font-size: 15px; "
           "line-height: 1.6;\">" + paragraph + "</p>\n<p style=\"margin: 5px 0 0 0; color: #92400e; "
           "font-weight: 600; font-size: 14px;\">" + last_line + "</p>\n</div>\n";
}
}  // namespace day_before_detail

/**
 * @param settled_date  the settled day, "YYYY-MM-DD"
 * @param holiday_name  the calendar's name for it, empty when it is not a holiday
 * @param no_session    no prices at all: the book was carried
 * @param some_missing  at least one held row of the settled table has no price or an unaccepted one
 */
inline DayBeforeBanner day_before_banner(const std::string& settled_date,
                                         const std::string& holiday_name, bool no_session,
                                         bool some_missing) {
    using day_before_detail::yellow_banner;
    DayBeforeBanner out;
    if (!no_session && !some_missing) return out;
    const int weekday = report_weekday_of(settled_date);
    const bool holiday = !holiday_name.empty();
    const bool saturday = !holiday && weekday == 6;
    const bool sunday = !holiday && weekday == 0;
    const std::string keep_watch = "Please continue to monitor your positions closely.";
    const std::string watch = "Please monitor these positions closely.";
    const std::string partial_paragraph =
        "Some futures settlement prices for " + settled_date +
        " are not available, as these contracts did not trade. The PnL for these contracts will be "
        "updated in the next report once settlement data is released.";
    const std::string sunday_paragraph =
        "Agricultural futures settlement prices for Sunday (" + settled_date +
        ") are not yet available, as these contracts begin trading Sunday evening. The PnL for "
        "these contracts will be updated in the next report once settlement data is released.";
    if (no_session) {
        out.form = DayBeforeForm::Closed;
        out.yellow_note = "<strong>Note:</strong> Yesterday's PnL data is not available.\n";
        if (holiday) {
            out.banner_html = yellow_banner(
                holiday_name,
                "The latest futures settlement prices are not available, as futures markets were "
                "closed yesterday (" + settled_date + ") due to a federal holiday. The PnL for "
                "these contracts will be updated in the next report once settlement data is "
                "released.",
                keep_watch);
            out.red_box_text = "<strong>No Executions:</strong> Since yesterday (" + settled_date +
                               ") was a market holiday, no new market data is available. "
                               "Positions remain unchanged from the previous trading day, and no "
                               "executions were generated.\n";
        } else if (saturday) {
            out.banner_html = yellow_banner(
                "Saturday",
                "The latest futures settlement prices are not available, as futures markets were "
                "closed yesterday (" + settled_date + ") due to it being a Saturday. The PnL for "
                "these contracts will be updated in the next report once settlement data is "
                "released.",
                keep_watch);
            out.red_box_text = "<strong>No Executions:</strong> Futures markets were closed "
                               "yesterday, so no executions were generated and portfolio holdings "
                               "are unchanged. Updates will resume with the next trading "
                               "session\xE2\x80\x99s data.\n";
        } else {
            if (sunday) {
                out.banner_html = yellow_banner("Sunday", sunday_paragraph, watch);
                out.yellow_note = "<strong>Note:</strong> Yesterday's PnL data is not available "
                                  "for agricultural contracts.\n";
            }
            out.red_box_text = "<strong>No Executions:</strong> No session was found for "
                               "yesterday (" + settled_date + "), so no new market data is "
                               "available. Positions remain unchanged from the previous trading "
                               "day, and no executions were generated.\n";
        }
        return out;
    }
    out.form = DayBeforeForm::Partial;
    if (holiday) {
        out.banner_html = yellow_banner(holiday_name, partial_paragraph, watch);
    } else if (saturday) {
        out.banner_html = yellow_banner("Saturday", partial_paragraph, watch);
    } else if (sunday) {
        out.banner_html = yellow_banner("Sunday", sunday_paragraph, watch);
    } else {
        out.form = DayBeforeForm::None;  // a weekday with a few rows missing: footnote 1 only
    }
    return out;
}

}  // namespace trade_ngin
