// include/trade_ngin/core/report_email_context.hpp
#pragma once

#include <cstdio>
#include <string>

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

    /// n, the count of settled grid returns the stored statistics are taken on (the row's
    /// total_days), and K, the sessions a year of the book's statistics grid. While n is below K
    /// the annualised return is an extrapolation and its line says so (T-8D-2 R74). 0: not given.
    int statistics_sessions{0};
    double sessions_per_year{0.0};

    /// A futures book's sizing capital and the portfolio value the sizing read, as the
    /// day's row stores them (risk_detail.sizing_capital, risk_detail.account_value). False when
    /// the day's row carries no risk_detail (no sized rebalance the overlay answered): the report
    /// then prints no figure.
    bool has_sizing_capital{false};
    double sizing_capital{0.0};
    double account_value_at_sizing{0.0};

    /// The book's asset class is equities (the equity runner): no futures text in the footer, no
    /// Symbols Reference and no rollover notice (tables of futures contract metadata), no
    /// holiday banner (its day before is its previous session by construction).
    bool equities{false};

    /// A futures runner's own classification of the day before: it found NO session and carried
    /// its book (its skip of the strategy processing). The report then shows no settled table
    /// and says there was no session; nothing in the report is derived from the weekday.
    bool prior_day_no_session{false};

    /// The book's start date, "YYYY-MM-DD": the live_start_date of its
    /// trading.strategy_trading_days_metadata row, as the runner read it. Empty: the footer
    /// prints no start date.
    std::string live_start_date;
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

/// The label of the annualised return line (T-8D-2 R61, R74): the stored value is printed for
/// every n, and labelled while the book has under one year of sessions.
inline std::string annualized_return_label(const EmailReportContext& context) {
    const bool under_one_year =
        context.statistics_sessions > 0 && context.sessions_per_year > 0.0 &&
        static_cast<double>(context.statistics_sessions) < context.sessions_per_year;
    return under_one_year ? "Total Annualized Return (extrapolated: under one year of sessions)"
                          : "Total Annualized Return";
}

/**
 * @brief The heading of the report's book block: the day T row as the run wrote it, and what of
 *        the day is still to be settled. The two kinds of book write that row differently, so
 *        each has its own words.
 *
 * A futures row is written before the day's close exists: it carries the day's transaction costs
 * and nothing of the day's market move, which the next run settles as the row's P&L.
 *
 * An equity row is written with more than its costs (the equity runner's Day T aggregate): the
 * P&L realized by the day's own fills, which are priced at the close before, is booked on the day
 * and never restated; the day's costs are booked; and the shares held are valued at that same
 * close, the latest one the run has. What the next run settles is the value of the holdings at
 * the day's own close, that is the day's price move, and no realized P&L.
 */
inline std::string book_heading_html(const std::string& report_date, bool equities) {
    if (equities) {
        return "<h2>Book as of " + report_date +
               " (today's trades and costs booked, holdings valued at the latest close; today's "
               "price move settles on the next run)</h2>\n";
    }
    return "<h2>Book as of " + report_date +
           " (today's costs booked; P&amp;L settles tomorrow)</h2>\n";
}

/// The equity runner's metric key for the cumulative dividend income its day's row stores
/// (total_dividend_income): the report prints it as one informational line (T-8D-2 R44 (c)).
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

/**
 * @brief A futures book's two capital lines (LOOP_SPEC section 3.1, section 10): the sizing
 *        capital the day was sized on, and the portfolio value the sizing read, each from its
 *        own stored key of the day's risk_detail (sizing_capital, account_value) and each under
 *        its own words. Nothing is derived from the two: their difference is the account's
 *        high-water mark less the starting capital, a figure that never falls and is positive
 *        with the account under its starting capital, so no line prints it (HD 2026-10-12).
 *        risk_detail.account_value is the portfolio value at the instant the day was SIZED; it
 *        is not the day's current portfolio value, which the leverage lines call the account
 *        value, so the line does not use those words for it. A day whose row carries no
 *        risk_detail has one line that says there is no reading, and no figure. The text a
 *        reader sees is plain words: no symbol of the specification.
 */
inline std::string capital_lines_html(const EmailReportContext& context) {
    if (!context.has_sizing_capital) {
        return "<div class=\"metric\"><strong>Sizing capital and portfolio value when the day "
               "was sized:</strong> no reading (no sized rebalance this run: no risk reading was "
               "stored for this day)</div>\n";
    }
    using report_capital_detail::dollars;
    return "<div class=\"metric\"><strong>Sizing capital:</strong> <span>" +
           dollars(context.sizing_capital) +
           "</span></div>\n"
           "<div class=\"metric\"><strong>Portfolio value when the day was sized:</strong> "
           "<span>" +
           dollars(context.account_value_at_sizing) + "</span></div>\n";
}

/// The equity report's dividend line: the cumulative figure, labelled as outside the P&L totals.
inline std::string dividend_line_html(double cumulative_dividend_income) {
    return "<div class=\"metric\"><strong>Dividend Income (cumulative, not in P&amp;L):</strong> "
           "<span>" +
           report_capital_detail::dollars(cumulative_dividend_income) + "</span></div>\n";
}

}  // namespace trade_ngin
