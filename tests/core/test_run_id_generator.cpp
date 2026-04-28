#include <gtest/gtest.h>
#include <chrono>
#include <regex>
#include "test_base.hpp"

// combine_strategy_names is a private static helper; reach it via the same
// pattern used elsewhere in this test suite.
#define private public
#include "trade_ngin/core/run_id_generator.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

Timestamp date_at(int year, int month, int day, int hour = 12, int min = 0, int sec = 0) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

}  // namespace

class RunIdGeneratorTest : public TestBase {};

// ===== combine_strategy_names =====

TEST_F(RunIdGeneratorTest, CombineStrategyNamesEmptyReturnsEmpty) {
    EXPECT_EQ(RunIdGenerator::combine_strategy_names({}), "");
}

TEST_F(RunIdGeneratorTest, CombineStrategyNamesSingleStrategyReturnsItself) {
    EXPECT_EQ(RunIdGenerator::combine_strategy_names({"ALPHA"}), "ALPHA");
}

TEST_F(RunIdGeneratorTest, CombineStrategyNamesSortsAlphabetically) {
    EXPECT_EQ(RunIdGenerator::combine_strategy_names({"BETA", "ALPHA"}), "ALPHA&BETA");
}

TEST_F(RunIdGeneratorTest, CombineStrategyNamesUsesAmpersandSeparator) {
    EXPECT_EQ(RunIdGenerator::combine_strategy_names({"A", "B", "C"}), "A&B&C");
}

TEST_F(RunIdGeneratorTest, CombineStrategyNamesIsDeterministicAcrossInputOrder) {
    auto a = RunIdGenerator::combine_strategy_names({"X", "Y", "Z"});
    auto b = RunIdGenerator::combine_strategy_names({"Z", "X", "Y"});
    auto c = RunIdGenerator::combine_strategy_names({"Y", "Z", "X"});
    EXPECT_EQ(a, b);
    EXPECT_EQ(b, c);
}

// ===== generate_timestamp_string =====

TEST_F(RunIdGeneratorTest, TimestampStringHasExpectedFormat) {
    auto ts = date_at(2026, 4, 27, 12, 34, 56);
    std::string s = RunIdGenerator::generate_timestamp_string(ts);
    // Expected shape: YYYYMMDD_HHMMSS_NNN (millis padded to 3 digits)
    std::regex pattern("^[0-9]{8}_[0-9]{6}_[0-9]{3}$");
    EXPECT_TRUE(std::regex_match(s, pattern)) << "Got: " << s;
}

TEST_F(RunIdGeneratorTest, TimestampStringDeterministicForSameTimestamp) {
    auto ts = date_at(2026, 1, 5, 10, 0, 0);
    EXPECT_EQ(RunIdGenerator::generate_timestamp_string(ts),
              RunIdGenerator::generate_timestamp_string(ts));
}

TEST_F(RunIdGeneratorTest, TimestampStringDistinctTimestampsProduceDistinctIds) {
    auto a = RunIdGenerator::generate_timestamp_string(date_at(2026, 1, 5, 10, 0, 0));
    auto b = RunIdGenerator::generate_timestamp_string(date_at(2026, 1, 5, 10, 0, 1));
    EXPECT_NE(a, b);
}

TEST_F(RunIdGeneratorTest, TimestampStringEncodesYearMonthDay) {
    auto s = RunIdGenerator::generate_timestamp_string(date_at(2026, 4, 27, 0, 0, 0));
    EXPECT_NE(s.find("20260427"), std::string::npos);
}

// ===== generate_date_string =====

TEST_F(RunIdGeneratorTest, DateStringHasYYYYMMDDFormat) {
    auto s = RunIdGenerator::generate_date_string(date_at(2026, 4, 27));
    EXPECT_EQ(s, "20260427");
}

TEST_F(RunIdGeneratorTest, DateStringIgnoresIntradayHourMinuteSecond) {
    auto a = RunIdGenerator::generate_date_string(date_at(2026, 4, 27, 0, 0, 0));
    auto b = RunIdGenerator::generate_date_string(date_at(2026, 4, 27, 23, 59, 59));
    EXPECT_EQ(a, b);
}

// ===== generate_portfolio_run_id (timestamp overload) =====

TEST_F(RunIdGeneratorTest, PortfolioRunIdContainsCombinedStrategiesAndTimestamp) {
    auto ts = date_at(2026, 4, 27, 12, 0, 0);
    auto id = RunIdGenerator::generate_portfolio_run_id({"BETA", "ALPHA"}, ts);
    EXPECT_NE(id.find("ALPHA&BETA"), std::string::npos);
    EXPECT_NE(id.find("20260427"), std::string::npos);
}

TEST_F(RunIdGeneratorTest, PortfolioRunIdEmptyStrategyVectorYieldsLeadingUnderscoreTimestamp) {
    auto ts = date_at(2026, 4, 27, 12, 0, 0);
    auto id = RunIdGenerator::generate_portfolio_run_id({}, ts);
    EXPECT_EQ(id[0], '_');  // empty combined → starts with "_"
}

// ===== generate_portfolio_run_id (string overload) =====

TEST_F(RunIdGeneratorTest, PortfolioRunIdStringOverloadConcatenatesDirectly) {
    auto id = RunIdGenerator::generate_portfolio_run_id({"ALPHA"}, std::string{"20260101_000000_000"});
    EXPECT_EQ(id, "ALPHA_20260101_000000_000");
}

// ===== generate_strategy_run_id (timestamp overload) =====

TEST_F(RunIdGeneratorTest, StrategyRunIdContainsNameAndTimestamp) {
    auto ts = date_at(2026, 4, 27, 12, 0, 0);
    auto id = RunIdGenerator::generate_strategy_run_id("TREND", ts);
    EXPECT_NE(id.find("TREND_"), std::string::npos);
    EXPECT_NE(id.find("20260427"), std::string::npos);
}

TEST_F(RunIdGeneratorTest, StrategyRunIdStringOverloadConcatenates) {
    auto id = RunIdGenerator::generate_strategy_run_id("TREND", std::string{"20260101_000000_000"});
    EXPECT_EQ(id, "TREND_20260101_000000_000");
}

// ===== generate_live_portfolio_run_id =====

TEST_F(RunIdGeneratorTest, LivePortfolioRunIdEncodesDateAndZeroPaddedSequence) {
    auto id = RunIdGenerator::generate_live_portfolio_run_id({"A", "B"}, date_at(2026, 4, 27), 5);
    EXPECT_EQ(id, "A&B_20260427_005");
}

TEST_F(RunIdGeneratorTest, LivePortfolioRunIdHandlesThreeDigitSequence) {
    auto id = RunIdGenerator::generate_live_portfolio_run_id({"A"}, date_at(2026, 4, 27), 123);
    EXPECT_EQ(id, "A_20260427_123");
}

TEST_F(RunIdGeneratorTest, LivePortfolioRunIdHandlesSequenceLargerThanThreeDigits) {
    auto id = RunIdGenerator::generate_live_portfolio_run_id({"A"}, date_at(2026, 4, 27), 9999);
    // setw(3) doesn't truncate; will print full 4 digits
    EXPECT_EQ(id, "A_20260427_9999");
}

TEST_F(RunIdGeneratorTest, LivePortfolioRunIdIsDeterministicForSameInputs) {
    auto a = RunIdGenerator::generate_live_portfolio_run_id({"X"}, date_at(2026, 1, 1), 1);
    auto b = RunIdGenerator::generate_live_portfolio_run_id({"X"}, date_at(2026, 1, 1), 1);
    EXPECT_EQ(a, b);
}
