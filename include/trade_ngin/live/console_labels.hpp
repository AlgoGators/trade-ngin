// include/trade_ngin/live/console_labels.hpp
#pragma once

#include <cstddef>
#include <cstdio>
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

}  // namespace trade_ngin
