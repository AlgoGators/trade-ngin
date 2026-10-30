// include/trade_ngin/core/report_equity_day.hpp
//
// The banners of an equity report about today and the day before. Email only. The calendar says
// whether a day is closed
// (weekend or market holiday); "the last close" and "the last trading session" are the runner's settled
// date, never a weekday.
//   closed today, open yesterday : "Today is ..." banner, red box, the settled table shown
//   closed today, closed yesterday: "Yesterday was ..." banner, red box, yellow note, table hidden
//   open today, closed yesterday : "Yesterday was ..." banner, yellow note, table hidden; the red box
//                                  only when no trade was made
//   open both days               : nothing
#pragma once

#include <string>

#include "trade_ngin/core/report_day_before.hpp"

namespace trade_ngin {

struct EquityDayBanner {
    std::string banner_html;
    std::string red_box_text;      ///< inside of the red "No Executions" box (empty: none)
    std::string yellow_note;       ///< inside of the yellow note (empty: none)
    bool red_box_with_banner{false};  ///< the red box follows the banner (the table stays shown)
    bool hide_table{false};
};

namespace equity_day_detail {
inline std::string banner(const std::string& title, const std::string& paragraph) {
    return "<div style=\"background: linear-gradient(135deg, #fef3c7 0%, #fde68a 100%); "
           "border: 2px solid #f59e0b; border-radius: 8px; padding: 20px 30px; margin: 20px 0 "
           "30px 0; box-shadow: 0 4px 6px rgba(0,0,0,0.1);\">\n"
           "<h2 style=\"margin: 0 0 10px 0; color: #92400e; font-size: 20px; border-bottom: "
           "2px solid #92400e; padding-bottom: 8px; display: inline-block;\">" + title +
           "</h2>\n<p style=\"margin: 15px 0 5px 0; color: #78350f; font-size: 15px; "
           "line-height: 1.6;\">" + paragraph + "</p>\n<p style=\"margin: 5px 0 0 0; color: #92400e; "
           "font-weight: 600; font-size: 14px;\">Please continue to monitor your positions "
           "closely.</p>\n</div>\n";
}
/// "Saturday", "Sunday" or the holiday's name of a closed day
inline std::string day_name(const std::string& ymd, const std::string& holiday_name) {
    if (!holiday_name.empty()) return holiday_name;
    return report_weekday_of(ymd) == 6 ? "Saturday" : "Sunday";
}
}  // namespace equity_day_detail

/**
 * @param run_date         the report's date, "YYYY-MM-DD" (today)
 * @param today_closed     the calendar closes today
 * @param today_holiday    its holiday name, empty for a weekend
 * @param yesterday_date   the calendar day before the run date
 * @param yesterday_closed the calendar closes it
 * @param yesterday_holiday its holiday name, empty for a weekend
 * @param settled_date     the runner's last trading session
 * @param has_trades       today's trades were made
 */
inline EquityDayBanner equity_day_banner(const std::string& run_date, bool today_closed,
                                         const std::string& today_holiday,
                                         const std::string& yesterday_date, bool yesterday_closed,
                                         const std::string& yesterday_holiday,
                                         const std::string& settled_date, bool has_trades) {
    using equity_day_detail::banner;
    using equity_day_detail::day_name;
    EquityDayBanner out;
    const std::string note = "<strong>Note:</strong> Yesterday's PnL data is not available.\n";
    if (today_closed && !yesterday_closed) {
        const std::string name = day_name(run_date, today_holiday);
        out.banner_html = banner(
            "Today is " + name,
            "Equity markets are closed today (" + run_date + ")" +
                (today_holiday.empty() ? "" : " due to a market holiday") +
                ". No trades were placed and positions are unchanged. Yesterday's results (" +
                settled_date + ") are shown below.");
        out.red_box_text = "<strong>No Executions:</strong> Equity markets are closed today, so no "
                           "executions were generated and portfolio holdings are unchanged. Updates "
                           "will resume with the next trading session\xE2\x80\x99s data.\n";
        out.red_box_with_banner = true;
    } else if (today_closed && yesterday_closed) {
        out.banner_html = banner(
            "Yesterday was " + day_name(yesterday_date, yesterday_holiday),
            "Equity markets were closed yesterday (" + yesterday_date +
                ") and are closed today, so no new prices are available. No trades were placed and "
                "positions are unchanged. The last trading session was " + settled_date + ".");
        out.red_box_text = "<strong>No Executions:</strong> Equity markets were closed yesterday, so "
                           "no executions were generated and portfolio holdings are unchanged. "
                           "Updates will resume with the next trading session\xE2\x80\x99s data.\n";
        out.yellow_note = note;
        out.hide_table = true;
    } else if (!today_closed && yesterday_closed) {
        std::string paragraph = "Equity markets were closed yesterday (" + yesterday_date +
                                "), so no new prices are available.";
        if (has_trades)
            paragraph += " Today's trades are priced at the last close (" + settled_date + ").";
        out.banner_html =
            banner("Yesterday was " + day_name(yesterday_date, yesterday_holiday), paragraph);
        if (!has_trades)
            out.red_box_text = "<strong>No Executions:</strong> Equity markets were closed "
                               "yesterday, so no executions were generated and portfolio holdings "
                               "are unchanged. Updates will resume with the next trading "
                               "session\xE2\x80\x99s data.\n";
        out.yellow_note = note;
        out.hide_table = true;
    }
    return out;
}

}  // namespace trade_ngin
