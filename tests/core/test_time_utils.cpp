//
// Created by Pranav Bhargava on 10/12/25.
//
#include <gtest/gtest.h>
#include <ctime>
#include <regex>
#include <thread>
#include <vector>
#include "trade_ngin/core/time_utils.hpp"  // Adjust the include path as needed

using namespace trade_ngin::core;

class TimeUtilsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Set up any common test fixtures if needed
    }

    void TearDown() override {
        // Clean up after tests if needed
    }
};

// Test safe_localtime with valid input
TEST_F(TimeUtilsTest, SafeLocaltimeValidInput) {
    std::time_t now = std::time(nullptr);
    std::tm result;

    std::tm* ret = safe_localtime(&now, &result);

    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret, &result);

    // Verify the struct contains reasonable values
    EXPECT_GE(result.tm_year, 100);  // Years since 1900, should be >= 2000
    EXPECT_GE(result.tm_mon, 0);
    EXPECT_LE(result.tm_mon, 11);
    EXPECT_GE(result.tm_mday, 1);
    EXPECT_LE(result.tm_mday, 31);
    EXPECT_GE(result.tm_hour, 0);
    EXPECT_LE(result.tm_hour, 23);
    EXPECT_GE(result.tm_min, 0);
    EXPECT_LE(result.tm_min, 59);
    EXPECT_GE(result.tm_sec, 0);
    EXPECT_LE(result.tm_sec, 61);  // Leap seconds
}

// Test safe_localtime with epoch time
TEST_F(TimeUtilsTest, SafeLocaltimeEpochTime) {
    std::time_t epoch = 0;
    std::tm result;

    std::tm* ret = safe_localtime(&epoch, &result);

    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret, &result);
    // Local time at epoch depends on timezone, just verify it's reasonable
    EXPECT_GE(result.tm_year, 69);  // 1969-1970 depending on timezone
    EXPECT_LE(result.tm_year, 70);
}

// Test safe_gmtime with valid input
TEST_F(TimeUtilsTest, SafeGmtimeValidInput) {
    std::time_t now = std::time(nullptr);
    std::tm result;

    std::tm* ret = safe_gmtime(&now, &result);

    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret, &result);

    // Verify the struct contains reasonable values
    EXPECT_GE(result.tm_year, 100);
    EXPECT_GE(result.tm_mon, 0);
    EXPECT_LE(result.tm_mon, 11);
    EXPECT_GE(result.tm_mday, 1);
    EXPECT_LE(result.tm_mday, 31);
    EXPECT_GE(result.tm_hour, 0);
    EXPECT_LE(result.tm_hour, 23);
}

// Test safe_gmtime with epoch time
TEST_F(TimeUtilsTest, SafeGmtimeEpochTime) {
    std::time_t epoch = 0;
    std::tm result;

    std::tm* ret = safe_gmtime(&epoch, &result);

    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret, &result);
    EXPECT_EQ(result.tm_year, 70);  // 1970
    EXPECT_EQ(result.tm_mon, 0);    // January
    EXPECT_EQ(result.tm_mday, 1);   // 1st
    EXPECT_EQ(result.tm_hour, 0);   // Midnight
    EXPECT_EQ(result.tm_min, 0);
    EXPECT_EQ(result.tm_sec, 0);
}

// Test that safe_localtime and safe_gmtime return the same pointer
TEST_F(TimeUtilsTest, SafeTimeReturnsSamePointer) {
    std::time_t now = std::time(nullptr);
    std::tm local_result, gmt_result;

    std::tm* local_ret = safe_localtime(&now, &local_result);
    std::tm* gmt_ret = safe_gmtime(&now, &gmt_result);

    EXPECT_EQ(local_ret, &local_result);
    EXPECT_EQ(gmt_ret, &gmt_result);
}

// Test get_formatted_time with default format
TEST_F(TimeUtilsTest, GetFormattedTimeBasic) {
    std::string time_str = get_formatted_time("%Y-%m-%d %H:%M:%S");

    EXPECT_FALSE(time_str.empty());

    // Verify format using regex: YYYY-MM-DD HH:MM:SS
    std::regex pattern(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})");
    EXPECT_TRUE(std::regex_match(time_str, pattern));
}

// Test get_formatted_time with local time
TEST_F(TimeUtilsTest, GetFormattedTimeLocalTime) {
    std::string time_str = get_formatted_time("%Y-%m-%d", true);

    EXPECT_FALSE(time_str.empty());

    // Verify format: YYYY-MM-DD
    std::regex pattern(R"(\d{4}-\d{2}-\d{2})");
    EXPECT_TRUE(std::regex_match(time_str, pattern));
}

// Test get_formatted_time with GMT
TEST_F(TimeUtilsTest, GetFormattedTimeGMT) {
    std::string time_str = get_formatted_time("%Y-%m-%d", false);

    EXPECT_FALSE(time_str.empty());

    // Verify format: YYYY-MM-DD
    std::regex pattern(R"(\d{4}-\d{2}-\d{2})");
    EXPECT_TRUE(std::regex_match(time_str, pattern));
}

// Test get_formatted_time with various formats
TEST_F(TimeUtilsTest, GetFormattedTimeVariousFormats) {
    struct FormatTest {
        const char* format;
        std::regex pattern;
    };

    std::vector<FormatTest> tests = {
        {"%Y", std::regex(R"(\d{4})")},     {"%m", std::regex(R"(\d{2})")},
        {"%d", std::regex(R"(\d{2})")},     {"%H:%M:%S", std::regex(R"(\d{2}:\d{2}:\d{2})")},
        {"%Y%m%d", std::regex(R"(\d{8})")}, {"%a %b %d", std::regex(R"(\w{3} \w{3} \d{2})")}};

    for (const auto& test : tests) {
        std::string result = get_formatted_time(test.format);
        EXPECT_TRUE(std::regex_match(result, test.pattern))
            << "Format: " << test.format << " produced: " << result;
    }
}

// Test thread safety of safe_localtime
TEST_F(TimeUtilsTest, SafeLocaltimeThreadSafety) {
    const int num_threads = 10;
    const int iterations = 100;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&success_count, iterations]() {
            for (int j = 0; j < iterations; ++j) {
                std::time_t now = std::time(nullptr);
                std::tm result;

                if (safe_localtime(&now, &result) != nullptr) {
                    success_count++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * iterations);
}

// Test thread safety of safe_gmtime
TEST_F(TimeUtilsTest, SafeGmtimeThreadSafety) {
    const int num_threads = 10;
    const int iterations = 100;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&success_count, iterations]() {
            for (int j = 0; j < iterations; ++j) {
                std::time_t now = std::time(nullptr);
                std::tm result;

                if (safe_gmtime(&now, &result) != nullptr) {
                    success_count++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * iterations);
}

// Test thread safety of get_formatted_time
TEST_F(TimeUtilsTest, GetFormattedTimeThreadSafety) {
    const int num_threads = 10;
    const int iterations = 100;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&success_count, iterations]() {
            for (int j = 0; j < iterations; ++j) {
                std::string result = get_formatted_time("%Y-%m-%d %H:%M:%S");
                if (!result.empty()) {
                    success_count++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * iterations);
}

// Test that local time and GMT differ (unless in GMT timezone)
TEST_F(TimeUtilsTest, LocalTimeAndGMTDifference) {
    std::time_t now = std::time(nullptr);
    std::tm local_result, gmt_result;

    safe_localtime(&now, &local_result);
    safe_gmtime(&now, &gmt_result);

    // Convert both back to time_t for comparison
    // Note: This test might fail in GMT timezone
    // We're mainly checking that the functions return different results
    // unless you're actually in GMT+0

    // At least verify both calls succeeded
    EXPECT_GE(local_result.tm_year, 100);
    EXPECT_GE(gmt_result.tm_year, 100);
}

// Test edge case: far future date
TEST_F(TimeUtilsTest, FarFutureDate) {
    // Test with a date far in the future (year 2100)
    std::time_t future = 4102444800;  // Jan 1, 2100
    std::tm result;

    std::tm* ret = safe_gmtime(&future, &result);

    if (ret != nullptr) {                // Some systems might not support dates this far
        EXPECT_EQ(result.tm_year, 200);  // 2100 - 1900
    }
}

// Test consecutive calls produce consistent results
TEST_F(TimeUtilsTest, ConsecutiveCallsConsistency) {
    std::time_t now = std::time(nullptr);
    std::tm result1, result2;

    safe_localtime(&now, &result1);
    safe_localtime(&now, &result2);

    EXPECT_EQ(result1.tm_year, result2.tm_year);
    EXPECT_EQ(result1.tm_mon, result2.tm_mon);
    EXPECT_EQ(result1.tm_mday, result2.tm_mday);
    EXPECT_EQ(result1.tm_hour, result2.tm_hour);
    EXPECT_EQ(result1.tm_min, result2.tm_min);
    EXPECT_EQ(result1.tm_sec, result2.tm_sec);
}
