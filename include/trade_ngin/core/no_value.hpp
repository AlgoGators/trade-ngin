// A statistic that has no value (T-8D R12).
//
// A ratio whose denominator is zero is undefined: a Sharpe ratio with no volatility, a Sortino
// ratio with no downside, a Calmar ratio with no drawdown, a profit factor with no loss. The
// calculators used to return a number for it (0, 999 or 999.99), which reads as a measured 0 or
// as a very good figure. They return kNoRatio, a NaN, and the stores keep NULL for it: the
// results writers never format a number that is not finite into a statement
// (data/finite_statement.hpp).

#pragma once

#include <cmath>
#include <limits>
#include <ostream>

namespace trade_ngin {

/// The value of a ratio that has none.
inline constexpr double kNoRatio = std::numeric_limits<double>::quiet_NaN();

/// Whether a statistic carries a value (it is finite).
inline bool has_value(double statistic) {
    return std::isfinite(statistic);
}

/// A statistic for a console line: the number, formatted as the stream is set, or "n/a" when it
/// has no value. `std::cout << ratio_or_na(results.sortino_ratio)`.
struct RatioOrNa {
    double value;
};
inline RatioOrNa ratio_or_na(double value) {
    return RatioOrNa{value};
}
inline std::ostream& operator<<(std::ostream& os, const RatioOrNa& ratio) {
    if (has_value(ratio.value)) {
        return os << ratio.value;
    }
    return os << "n/a";
}

}  // namespace trade_ngin
