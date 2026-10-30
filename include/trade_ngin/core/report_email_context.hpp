// include/trade_ngin/core/report_email_context.hpp
#pragma once

#include <cstdio>
#include <string>

#include "trade_ngin/core/report_rolls.hpp"
#include "trade_ngin/core/report_settled_marks.hpp"

namespace trade_ngin {

/**
 * @brief What a runner tells the daily report about the day that the metric maps (name to
 *        number) cannot carry. Every field has a default that prints nothing.
 */
struct EmailReportContext {
    /// The settled day, "YYYY-MM-DD": the day the stored statistics run through and the settled
    /// table and its block are for. It is the runner's own T-1: the calendar day before the run
    /// date on a futures book, the previous session on the equity book. Empty: the report takes
    /// the calendar day before its date.
    std::string settled_date;

    /// The book's asset class is equities (the equity runner): no futures text in the footer, no
    /// Symbols Reference and no rollover notice (tables of futures contract metadata), no
    /// holiday banner (its day before is its previous session by construction).
    bool equities{false};

    /// A futures runner's symbols whose settled bar booked no move, by kind: the settled
    /// table marks their $0.00 rows.
    SettledZeroKinds settled_zero_kinds;

    /// A futures runner's own classification of the day before: it found NO session and carried
    /// its book (its skip of the strategy processing). The report then shows no settled table
    /// and says there was no session; nothing in the report is derived from the weekday.
    bool prior_day_no_session{false};

    /// The equity runner's calendar closes the run date (weekend or market holiday): the equity
    /// report's day banners (report_equity_day.hpp).
    bool equity_closed_today{false};

    /// The book's start date, "YYYY-MM-DD": the live_start_date of its
    /// trading.strategy_trading_days_metadata row, as the runner read it. Empty: the footer
    /// prints no start date.
    std::string live_start_date;

    /// With per_contract_rolls on, the roll outlook of the held roots and the
    /// words of each roll's line (report_rolls.hpp): the roll line, the Rollover Warning box, the
    /// Symbols Reference's Front Month and Contract Months columns, the footer's roll sentence. Empty
    /// with the switch off: none of them is printed.
    ReportRolls rolls;
};

/// "2025-10-05" as the footer prints a date: "October 5th, 2025". A text that is not a date of
/// that form is returned as it is.
inline std::string report_long_date(const std::string& iso_date) {
    static const char* const kMonths[] = {"January",   "February", "March",    "April",
                                          "May",       "June",     "July",     "August",
                                          "September", "October",  "November", "December"};
    int year = 0, month = 0, day = 0;
    char tail = 0;
    if (iso_date.size() != 10 ||
        std::sscanf(iso_date.c_str(), "%4d-%2d-%2d%c", &year, &month, &day, &tail) != 3 ||
        month < 1 || month > 12 || day < 1 || day > 31) {
        return iso_date;
    }
    const char* suffix = "th";
    if (day % 100 < 11 || day % 100 > 13) {
        if (day % 10 == 1)
            suffix = "st";
        if (day % 10 == 2)
            suffix = "nd";
        if (day % 10 == 3)
            suffix = "rd";
    }
    return std::string(kMonths[month - 1]) + " " + std::to_string(day) + suffix + ", " +
           std::to_string(year);
}

/// The equity runner's metric key for the cumulative dividend income its day's row stores
/// (total_dividend_income): the report prints it as one informational line.
inline constexpr const char* kReportDividendIncome = "Dividend Income";

namespace report_capital_detail {
/// Dollars to the cent with thousands separators, the sign in front: 2254.731679 -> "$2,254.73".
inline std::string dollars(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", value < 0 ? -value : value);
    std::string text = buf;
    for (int at = static_cast<int>(text.find('.')) - 3; at > 0; at -= 3) {
        text.insert(static_cast<size_t>(at), ",");
    }
    return std::string(value < 0 && text != "0.00" ? "-$" : "$") + text;
}
}  // namespace report_capital_detail

/// The equity report's dividend line: the cumulative figure, labelled as outside the P&L totals.
inline std::string dividend_line_html(double cumulative_dividend_income) {
    return "<div class=\"metric\"><strong>Dividends Received:</strong> "
           "<span>" +
           report_capital_detail::dollars(cumulative_dividend_income) + "</span></div>\n";
}

}  // namespace trade_ngin
