// include/trade_ngin/core/report_price_text.hpp
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace trade_ngin {

/**
 * @brief A price as the daily report prints it: every decimal the price has, to at most eight,
 *        and never fewer than two.
 *
 * Two fixed decimals printed a Japanese yen future (0.0062995) as 0.01 and a Treasury note
 * (103.73828125) as 103.74, so a row's quantity, price and notional did not multiply out. The
 * prices the book holds are tick multiples with a short decimal expansion (a 1/256 tick has
 * eight decimals), so eight is exact for them; trailing zeros beyond the second decimal are not
 * printed (677.00, 7217.25, 0.1998, 0.0062995, 103.73828125).
 *
 * @param price     the price
 * @param thousands group the integer part in threes with commas (79,070.00)
 * @return the text, without a currency sign; "n/a" for a value that is not finite
 */
inline std::string report_price_text(double price, bool thousands = false) {
    if (!std::isfinite(price))
        return "n/a";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.8f", price);
    std::string text = buf;
    const size_t point = text.find('.');
    size_t end = text.size();
    while (end > point + 3 && text[end - 1] == '0')
        --end;
    text.erase(end);
    if (thousands) {
        const size_t first = text[0] == '-' ? 1 : 0;
        for (int at = static_cast<int>(point) - 3; at > static_cast<int>(first); at -= 3) {
            text.insert(static_cast<size_t>(at), ",");
        }
    }
    return text;
}

}  // namespace trade_ngin
