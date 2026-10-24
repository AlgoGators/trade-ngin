// include/trade_ngin/core/portability_win.hpp
//
// POSIX time functions the engine uses, for MSVC. Force-included into every Windows translation
// unit (CMakeLists.txt, /FI), so the engine's sources keep calling gmtime_r, localtime_r and timegm
// as they do on Linux. Nothing here is compiled on any other platform.
#pragma once

#ifdef _WIN32
#include <BaseTsd.h>
#include <time.h>

#ifndef _SSIZE_T_DEFINED
using ssize_t = SSIZE_T;
#define _SSIZE_T_DEFINED
#endif

inline struct tm* gmtime_r(const time_t* time, struct tm* result) {
    return gmtime_s(result, time) == 0 ? result : nullptr;
}

inline struct tm* localtime_r(const time_t* time, struct tm* result) {
    return localtime_s(result, time) == 0 ? result : nullptr;
}

/// UTC broken-down time to an instant, as glibc's timegm.
inline time_t timegm(struct tm* tm) {
    return _mkgmtime(tm);
}
#endif  // _WIN32
