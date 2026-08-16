//===== test_portability.hpp =====
// POSIX functions used by the test suite that MSVC does not provide.
// The tests were originally written against the Linux CI image; these shims
// let the same sources build on Windows without touching each test file.
#pragma once

#ifdef _MSC_VER

#include <cstdlib>
#include <ctime>

// timegm() interprets a struct tm as UTC. MSVC spells it _mkgmtime().
inline time_t timegm(std::tm* tm) {
    return _mkgmtime(tm);
}

// setenv()/unsetenv() have no direct MSVC equivalent; _putenv_s() sets a
// variable, and setting it to an empty string removes it.
inline int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value);
}

inline int unsetenv(const char* name) {
    return _putenv_s(name, "");
}

#endif  // _MSC_VER
