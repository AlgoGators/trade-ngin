// A live run always names its date (T-EMAIL P2-5, HD ruling Q5).
//
// A run with no date used to read the host's clock and mail the report by default. Production
// always passes a date and --send-email (scripts/run_live_portfolio.sh), so only a mistake could
// reach that path: now it prints the usage, exits kDatelessRunExitCode and does nothing else.

#pragma once

#include <string>

namespace trade_ngin {
namespace live {

inline constexpr int kDatelessRunExitCode = 2;

inline std::string dateless_run_message(const std::string& program) {
    return "No run date was given, so nothing was run and nothing was sent.\n"
           "Usage: " +
           program +
           " YYYY-MM-DD [--send-email]\n"
           "Example: " +
           program + " 2025-01-01 --send-email\n";
}

// A run names one date (the rejoin, 2026-10-22). main's --date and the positional date name the same
// run; two different dates are refused with exit 1 before anything is read or written, never the
// later one taken silently.
inline std::string two_run_dates_message(const std::string& first, const std::string& second) {
    return "Two run dates were given (" + first + " and " + second +
           "): give one, positionally or as --date";
}

}  // namespace live
}  // namespace trade_ngin
