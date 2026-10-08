// include/trade_ngin/core/testing_access.hpp

#pragma once

/**
 * @file
 * @brief Access specifier that opens internals to the test build only
 *
 * Several unit tests exercise private helpers directly. On GCC and Clang the
 * tests get away with "#define private public" because member access is not
 * encoded in the mangled symbol name; MSVC does encode it, so the library and
 * the tests must agree on the access specifier or the symbols fail to link.
 *
 * TESTING is defined by CMake for both the library and the test target when
 * BUILD_TESTING is ON, so TESTING_PRIVATE is public in test builds and private
 * everywhere else. Production builds keep the original encapsulation.
 *
 * Usage:
 * @code
 *   class Foo {
 *   public:
 *       void public_api();
 *
 *   TESTING_PRIVATE:
 *       int helper_under_test();
 *   };
 * @endcode
 */

#ifdef TESTING
#define TESTING_PRIVATE public
#else
#define TESTING_PRIVATE private
#endif
