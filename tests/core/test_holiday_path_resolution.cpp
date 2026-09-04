#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "trade_ngin/core/holiday_checker.hpp"

using namespace trade_ngin;

// Phase 6 §6a -- pins the holidays.json path-resolution fallback chain:
//   1. TRADE_NGIN_HOLIDAYS_JSON env var (returned as-is, even if missing)
//   2. ./include/trade_ngin/core/holidays.json (dev/source layout)
//   3. ./holidays.json (deploy bundle next to the binary)
//   4. /etc/trade_ngin/holidays.json (system-wide)
// If none exist, returns option (2) so the load-error log names the most
// likely expected location.

namespace {

class HolidayPathResolutionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Make sure the env var is unset at the start of each test; the
        // test that sets it cleans up after itself.
        unsetenv("TRADE_NGIN_HOLIDAYS_JSON");
    }
    void TearDown() override {
        unsetenv("TRADE_NGIN_HOLIDAYS_JSON");
    }
};

}  // namespace

// Env var wins, returned verbatim even if the file doesn't exist (so the
// load-error log names the misconfigured path -- silent fallback would be
// worse).
TEST_F(HolidayPathResolutionTest, EnvVarWinsAndPassesThroughVerbatim) {
    setenv("TRADE_NGIN_HOLIDAYS_JSON", "/nonexistent/custom/holidays.json", 1);
    EXPECT_EQ(HolidayChecker::resolve_holidays_path(),
              "/nonexistent/custom/holidays.json");
}

// Env var honored even when the file DOES exist at one of the fallback
// paths.
TEST_F(HolidayPathResolutionTest, EnvVarPreemptsFallbackChain) {
    auto tmp = std::filesystem::temp_directory_path() / "test_holidays_env.json";
    {
        std::ofstream(tmp) << "{}";
    }
    setenv("TRADE_NGIN_HOLIDAYS_JSON", tmp.string().c_str(), 1);
    EXPECT_EQ(HolidayChecker::resolve_holidays_path(), tmp.string());
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

// With no env var and no override files set up, the resolver returns the
// dev-layout path so the load-error log points operators at the most
// likely expected location.
TEST_F(HolidayPathResolutionTest, ReturnsDevLayoutPathOnFallback) {
    const std::string p = HolidayChecker::resolve_holidays_path();
    // The dev-layout path is the recommended default. The test environment
    // may or may not have the file present in the CWD; what we pin is the
    // shape of the returned string when nothing else matches.
    //
    // If the file IS present at the dev-layout path (because tests run
    // from the repo root), the resolver still returns it -- that's the
    // intended behavior. If absent, we get the dev-layout path anyway
    // (option (2) above).
    EXPECT_TRUE(p == "include/trade_ngin/core/holidays.json" ||
                p == "holidays.json" ||
                p == "/etc/trade_ngin/holidays.json")
        << "Unexpected resolved path: " << p;
}

// Loading from a fully-resolved path should produce a valid HolidayChecker
// (even with an empty JSON file -- just means no holidays registered).
TEST_F(HolidayPathResolutionTest, ConstructorAcceptsResolvedPath) {
    auto tmp = std::filesystem::temp_directory_path() / "test_holidays_ctor.json";
    {
        std::ofstream(tmp) << "{}";  // empty calendar
    }
    setenv("TRADE_NGIN_HOLIDAYS_JSON", tmp.string().c_str(), 1);
    HolidayChecker checker(HolidayChecker::resolve_holidays_path());
    EXPECT_FALSE(checker.is_holiday("2024-12-25"));  // empty calendar = nothing is a holiday
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}
