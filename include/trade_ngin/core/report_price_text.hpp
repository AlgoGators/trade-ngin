// include/trade_ngin/core/report_price_text.hpp
#pragma once

#include <algorithm>
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

/**
 * @brief The decimals every price of one contract prints with, in every table and file:
 *        four, or the decimals of the contract's tick when four cannot show a tick
 *        exactly (6J 0.0000005 -> 7, ZF 1/64 -> 6), so a row still multiplies out.
 */
inline int report_price_places(double tick_size) {
    if (!(tick_size > 0.0) || !std::isfinite(tick_size))
        return 4;
    for (int places = 4; places < 10; ++places) {
        const double scaled = tick_size * std::pow(10.0, places);
        if (std::fabs(scaled - std::round(scaled)) < 1e-6 * std::max(1.0, scaled))
            return places;
    }
    return 8;
}

/// A price at a fixed number of decimals, optionally with thousands separators; "n/a" if not finite.
inline std::string report_price_fixed(double price, int places, bool thousands = false) {
    if (!std::isfinite(price))
        return "n/a";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", places, std::fabs(price));
    std::string text = buf;
    if (thousands) {
        const size_t point = text.find('.');
        for (int at = static_cast<int>(point == std::string::npos ? text.size() : point) - 3; at > 0;
             at -= 3)
            text.insert(static_cast<size_t>(at), ",");
    }
    const bool zero = text.find_first_not_of("0.,") == std::string::npos;
    return std::string(price < 0 && !zero ? "-" : "") + text;
}

/// "$1,234.5000" / "-$1,234.5000" at `places` decimals.
inline std::string report_price_dollars_places(double price, int places) {
    if (!std::isfinite(price))
        return "n/a";
    const std::string text = report_price_fixed(price, places, true);
    return text[0] == '-' ? "-$" + text.substr(1) : "$" + text;
}

}  // namespace trade_ngin
