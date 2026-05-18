#pragma once

#include <time.h>
#include <chrono>
#include <cstdio>
#include <string>

namespace trade_ngin {
namespace core {

/**
 * @brief Thread-safe wrapper for localtime
 *
 * This function provides a platform-independent way to get local time
 * using thread-safe variants of the standard library functions.
 *
 * @param time Pointer to time_t value
 * @param result Pointer to tm struct where result will be stored
 * @return Pointer to the result tm struct on success, nullptr on failure
 */
inline std::tm* safe_localtime(const std::time_t* time, std::tm* result) {
#ifdef _WIN32
    // Windows
    if (localtime_s(result, time) != 0) {
        return nullptr;
    }
    return result;
#else
    // POSIX
    return localtime_r(time, result);
#endif
}

/**
 * @brief Thread-safe wrapper for gmtime
 *
 * This function provides a platform-independent way to get GMT time
 * using thread-safe variants of the standard library functions.
 *
 * @param time Pointer to time_t value
 * @param result Pointer to tm struct where result will be stored
 * @return Pointer to the result tm struct on success, nullptr on failure
 */
inline std::tm* safe_gmtime(const std::time_t* time, std::tm* result) {
#ifdef _WIN32
    // Windows
    if (gmtime_s(result, time) != 0) {
        return nullptr;
    }
    return result;
#else
    // POSIX
    return gmtime_r(time, result);
#endif
}

/**
 * @brief Format a system_clock time_point as a UTC `YYYY-MM-DD` calendar date.
 *
 * Phase 5 §5c -- the data layer's timezone contract is UTC end-to-end. This
 * helper is the one true way to produce a date-string key from a Timestamp
 * for use in date-keyed lookups against provider tables. Callers MUST NOT
 * call `std::gmtime` or `std::put_time` directly -- both are non-thread-safe
 * and silently locale-dependent.
 *
 * Underlying primitive is safe_gmtime (thread-safe on POSIX and Windows).
 *
 * @param tp system_clock time point (typically Timestamp = chrono alias)
 * @return 10-char `YYYY-MM-DD` string in UTC
 */
inline std::string format_utc_date(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    if (!safe_gmtime(&t, &tm)) {
        return std::string("1970-01-01");
    }
    char buf[11];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return std::string(buf);
}

/**
 * @brief Format a time_point as a UTC `YYYY-MM-DD HH:MM:SS` timestamp string.
 *
 * Phase 6 §6c -- companion to format_utc_date for sites that need a full
 * timestamp (e.g. SQL INSERT values with second resolution). Same
 * thread-safety guarantees: routes through safe_gmtime.
 */
inline std::string format_utc_datetime(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    if (!safe_gmtime(&t, &tm)) {
        return std::string("1970-01-01 00:00:00");
    }
    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buf);
}

/**
 * @brief Get current time as a string with specified format
 *
 * @param format Format string compatible with strftime
 * @param use_local_time If true, uses local time, otherwise GMT
 * @return Formatted time string
 */
inline std::string get_formatted_time(const char* format, bool use_local_time = true) {
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::tm result;

    if (use_local_time) {
        safe_localtime(&now_c, &result);
    } else {
        safe_gmtime(&now_c, &result);
    }

    char buffer[128];
    std::strftime(buffer, sizeof(buffer), format, &result);
    return std::string(buffer);
}

}  // namespace core
}  // namespace trade_ngin