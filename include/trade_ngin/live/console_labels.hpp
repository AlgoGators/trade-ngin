// include/trade_ngin/live/console_labels.hpp
#pragma once

#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>

namespace trade_ngin {

/**
 * @brief The console lines of the live runners that name a figure (T-8D-2 R68). Each function
 *        returns the finished text, ending with a newline, so a test can call it; the runners
 *        print what it returns and compute nothing here.
 */

/// The two lines under the "Daily Position Report" banner: the run date and the number of
/// symbols in the runner's book map (a symbol held at zero is counted; it is not the number of
/// open positions, which the runner prints as "Active Positions"). Built as text, so the
/// console stream's fill character is never changed: the date used to set it to '0' and every
/// padded column printed after it came out zero-padded.
inline std::string position_report_head_console(int year, int month, int day,
                                                std::size_t symbols_in_book) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Date: %d-%02d-%02d\nSymbols in book: %zu\n", year, month, day,
                  symbols_in_book);
    return buf;
}

/// The snapshot reporter's figures of the stored book (the second RiskManager's result, T-8D
/// R16): measurements, not what sized the book. `ex_ante_vol` is the stored portfolio_var, a
/// fraction (the reporter's annualised ex-ante volatility on price-space weights).
struct ReporterConsoleFigures {
    double ex_ante_vol{0.0};
    double gross_leverage{0.0};
    double net_leverage{0.0};
    double max_correlation{0.0};
    double jump_risk{0.0};
};

/// The reporter's lines of "Strategy Metrics", each named as the reporter's, the two leverage
/// lines with the capital the reporter divided by (`capital_name`: "the sizing capital" on the
/// futures runners, "the configured capital" on the equity runner). `prints_ex_ante_vol` is
/// false on the futures runners, which print the overlay's expected risk instead. No figures
/// (the reporter's evaluation failed): every line reads N/A.
inline std::string reporter_console(const std::optional<ReporterConsoleFigures>& figures,
                                    const std::string& capital_name, bool prints_ex_ante_vol) {
    const auto two = [](double value) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", value);
        return std::string(buf);
    };
    const auto line = [&](const std::string& name, const std::optional<std::string>& value) {
        return name + ": " + (value ? *value : std::string("N/A")) + "\n";
    };
    const auto of = [&](double ReporterConsoleFigures::* field) {
        return figures ? std::optional<std::string>(two((*figures).*field)) : std::nullopt;
    };
    std::string out;
    if (prints_ex_ante_vol) {
        out += line("Ex-ante vol (reporter)",
                    figures ? std::optional<std::string>(two(figures->ex_ante_vol * 100.0) + "%")
                            : std::nullopt);
    }
    out += line("Gross Leverage (reporter, on " + capital_name + ")",
                of(&ReporterConsoleFigures::gross_leverage));
    out += line("Net Leverage (reporter, on " + capital_name + ")",
                of(&ReporterConsoleFigures::net_leverage));
    out += line("Max Correlation (reporter)", of(&ReporterConsoleFigures::max_correlation));
    out += line("Jump Risk (99th, reporter)", of(&ReporterConsoleFigures::jump_risk));
    return out;
}

/// The day-T "Realized P&L" line: the cumulative realized P&L BEFORE costs (the row's
/// total_realized_pnl; the costs are their own column and come off once, in the total).
inline std::string realized_pnl_console(double total_realized_pnl) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Realized P&L (gross of costs): $%.2f\n", total_realized_pnl);
    return buf;
}

/// The futures runners' day-T "Daily Return" line, a percent: the day-T row is written before
/// the day's close exists, so its return is today's transaction costs over yesterday's value
/// and nothing else; the day's market move is booked when the next run settles the row.
inline std::string daily_return_console_costs_only(double daily_return_pct) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Daily Return (day T: costs only): %.2f%%\n", daily_return_pct);
    return buf;
}

/// The equity runner's day-T "Daily Return" line, a percent. It is NOT costs only: the equity
/// row's day-T P&L is the day's realized P&L less its costs plus the day-over-day change in the
/// mark of the held shares.
inline std::string daily_return_console_as_written(double daily_return_pct) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "Daily Return (day T as written: realized less costs, plus the change in the "
                  "mark): %.2f%%\n",
                  daily_return_pct);
    return buf;
}

}  // namespace trade_ngin
