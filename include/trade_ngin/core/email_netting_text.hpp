// How the daily email prints the netting figures (T-NETTING fix rounds 1 and 2).
//
// One rounding for every figure: the stream's (std::fixed, two decimals), the rounding the email's
// other dollar figures have always had. The adjustment is printed AS IT ACTS ON THE COST, and as
// the difference of the two figures printed beside it IN PRINTED CENTS, so on every line: own
// cost, the adjustment applied to it, the cost after netting, add up exactly as printed.

#pragma once

#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>

namespace trade_ngin {
namespace email_netting {

/// The note under the book's footer of a several-sleeve email on a netted day.
inline constexpr const char* kRoundingNote =
    "Each figure is rounded to the cent on its own, so rows and totals can differ by a cent.";

/// `value` with two decimals, as the stream rounds it, without separators: "-1234.50".
inline std::string two_decimals(double value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << value;
    return oss.str();
}

/// A dollar amount with two decimals and thousands separators: "1,234.50", "-1,234.50",
/// "-123.45" (never "-,123.45").
inline std::string netting_money(double value) {
    std::string s = two_decimals(value);
    const size_t first_digit = (!s.empty() && s[0] == '-') ? 1 : 0;
    const size_t dp = s.find('.');
    int pos = static_cast<int>(dp == std::string::npos ? s.size() : dp) - 3;
    while (pos > static_cast<int>(first_digit)) {
        s.insert(static_cast<size_t>(pos), ",");
        pos -= 3;
    }
    return s;
}

/// The cents of `value` exactly as two_decimals prints it (so a figure and its cents can never
/// disagree, whatever the value's last binary digit).
inline long long printed_cents(double value) {
    const std::string s = two_decimals(value);
    long long cents = 0;
    for (const char c : s) {
        if (c >= '0' && c <= '9') cents = cents * 10 + (c - '0');
    }
    return (!s.empty() && s[0] == '-') ? -cents : cents;
}

/// The netting adjustment as it acts on the cost, from the two figures printed beside it: a saving
/// is a reduction ("-$2.40"), an extra cost an addition ("+$1.82"), none "$0.00". `own_cost` and
/// `cost_after_netting` are the very doubles the neighbouring cells print.
inline std::string netting_effect_text(double own_cost, double cost_after_netting) {
    const long long cents = printed_cents(cost_after_netting) - printed_cents(own_cost);
    if (cents == 0) return "$0.00";
    return std::string(cents < 0 ? "-$" : "+$") +
           netting_money(static_cast<double>(std::llabs(cents)) / 100.0);
}

}  // namespace email_netting
}  // namespace trade_ngin
