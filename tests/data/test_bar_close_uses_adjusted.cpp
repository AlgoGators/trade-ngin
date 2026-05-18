#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// Regression test for audit finding §1.11 (the verified non-bug, kept as a
// guard against quiet breakage).
//
// The equity loader at src/data/postgres_database.cpp rewrites the SELECT for
// AssetClass::EQUITIES so that OHLC are proportionally rescaled by
// (closeadj / close) and the `close` column maps to `closeadj`. This makes
// equity Bars carry a continuous split/dividend-adjusted series, which the
// strategy code depends on for signal correctness.
//
// A proper integration test would load a known-split equity (e.g. AAPL
// 2020-08-31 4-for-1) and assert bar.close straddles the split smoothly. That
// requires a populated test DB; the project doesn't currently ship one, so
// this test substitutes a lighter-weight guard: assert the SQL query string
// in the source file still contains the rescaling expression. Cheap, catches
// the regression the audit cares about, no DB required.
//
// If the source layout changes such that this file lives elsewhere, update
// candidate_paths_. The test deliberately probes multiple plausible roots so
// it works from `ctest --test-dir build` and from the repo root.

namespace {

const char* kCandidatePaths[] = {
    "src/data/postgres_database.cpp",
    "../src/data/postgres_database.cpp",
    "../../src/data/postgres_database.cpp",
    "../../../src/data/postgres_database.cpp",
};

std::string read_file_from_candidates() {
    for (const char* p : kCandidatePaths) {
        std::ifstream f(p);
        if (f.is_open()) {
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
    }
    return {};
}

}  // namespace

TEST(BarCloseAdjustedRegression, EquitySelectRescaleExpressionPresent) {
    std::string source = read_file_from_candidates();
    if (source.empty()) {
        GTEST_SKIP() << "postgres_database.cpp not findable from cwd; "
                        "run from repo root or build dir.";
    }

    // The proportional OHLC rescaling: open/high/low * (closeadj / close).
    EXPECT_NE(source.find("closeadj / close"), std::string::npos)
        << "Equity OHLC rescaling expression `(closeadj / close)` missing from "
           "postgres_database.cpp. Without it, equity bars load raw exchange "
           "prices instead of split/dividend-adjusted prices and strategy "
           "signals silently corrupt across corporate actions.";

    // The close-column remap: `closeadj as close`.
    EXPECT_NE(source.find("closeadj as close"), std::string::npos)
        << "`closeadj as close` mapping missing from equity SELECT. Bars would "
           "carry raw close instead of adjusted.";

    // The asset-class branch that gates the equity SELECT.
    EXPECT_NE(source.find("AssetClass::EQUITIES"), std::string::npos)
        << "Equity SELECT branch missing; loader no longer differentiates "
           "equities from futures.";
}
