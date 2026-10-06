// include/trade_ngin/risk/overlay_record.hpp
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

#include "trade_ngin/risk/overlay.hpp"

namespace trade_ngin {
namespace overlay {

/// A date as YYYY-MM-DD from a whole day number counted from 1970-01-01.
inline std::string ordinal_date(double ordinal) {
    long z = static_cast<long>(ordinal) + 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned long doe = static_cast<unsigned long>(z - era * 146097);
    const unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = static_cast<long>(yoe) + era * 400;
    const unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned long mp = (5 * doy + 2) / 153;
    const unsigned long d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned long m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04ld-%02lu-%02lu", y, m, d);
    return buf;
}

}  // namespace overlay
}  // namespace trade_ngin
