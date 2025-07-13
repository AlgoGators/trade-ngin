#pragma once

#include <time.h>
#include <chrono>

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