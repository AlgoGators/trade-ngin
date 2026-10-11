// include/trade_ngin/core/report_email_context.hpp
#pragma once

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
};

/// The label of the annualised return line (T-8D-2 R61, R74): the stored value is printed for
/// every n, and labelled while the book has under one year of sessions.
inline std::string annualized_return_label(const EmailReportContext& context) {
    const bool under_one_year =
        context.statistics_sessions > 0 && context.sessions_per_year > 0.0 &&
        static_cast<double>(context.statistics_sessions) < context.sessions_per_year;
    return under_one_year ? "Total Annualized Return (extrapolated: under one year of sessions)"
                          : "Total Annualized Return";
}

}  // namespace trade_ngin
