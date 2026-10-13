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


/**
 * @brief A price with its dollar sign and thousands separators, as every price of the daily report
 *        prints (T-EMAIL, HD ruling Q9): "$79,070.00", "$0.0062995", "-$1.25"; "n/a" for a value
 *        that is not finite.
 */
inline std::string report_price_dollars(double price) {
    if (!std::isfinite(price))
        return "n/a";
    const bool negative = price < 0 && report_price_text(-price) != "0.00";
    return std::string(negative ? "-$" : "$") + report_price_text(std::fabs(price), true);
}

/**
 * @brief An amount with its dollar sign, thousands separators and `decimals` decimals, as every
 *        amount of the daily report prints: "$1,234.50", and a negative as "-$1,234.50", never
 *        "$-1,234.50". An amount that rounds to zero prints "$0.00" with no sign.
 */
inline std::string report_dollars(double value, int decimals = 2) {
    if (!std::isfinite(value))
        return "n/a";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, std::fabs(value));
    std::string text = buf;
    const bool zero = text.find_first_not_of("0.") == std::string::npos;
    const size_t point = text.find('.');
    const int integer_end = static_cast<int>(point == std::string::npos ? text.size() : point);
    for (int at = integer_end - 3; at > 0; at -= 3)
        text.insert(static_cast<size_t>(at), ",");
    return std::string(value < 0 && !zero ? "-$" : "$") + text;
}

}  // namespace trade_ngin
