// Tests for MacroDataLoader — 90-day fill semantics and NaN handling across
// macro panel construction.
//
// Note: MacroDataLoader DB-backed paths are exercised end-to-end by the
// macro pipeline integration tests; standalone substrate-level guards for
// the loader currently live alongside the BSTS forward-fill tests because
// the same fill semantics are shared.

#include <gtest/gtest.h>
