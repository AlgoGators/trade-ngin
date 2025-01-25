// tests/core/test_logger.cpp
#include <gtest/gtest.h>
#include "trade_ngin/core/logger.hpp"
#include <fstream>
#include <string>
#include <filesystem>
#include <regex>
#include <sstream>

using namespace trade_ngin;

class LoggerTest : public ::testing::Test {
protected:
    std::string test_log_dir = "test_logs";
    std::string test_file = "test_logs/test_log.txt";

    void SetUp() override {
        // Create test directory
        std::filesystem::create_directories(test_log_dir);
    }

    void TearDown() override {
        // Clean up test files
        std::filesystem::remove_all(test_log_dir);
    }

    // Helper to read log file contents
    std::string read_log_file(const std::string& filename) {
        std::ifstream file(filename);
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
};

TEST_F(LoggerTest, Initialization) {
    auto& logger = Logger::instance();
    
    LoggerConfig config;
    config.min_level = LogLevel::INFO;
    config.destination = LogDestination::FILE;
    config.log_directory = test_log_dir;
    config.filename_prefix = "test_log";
    
    EXPECT_NO_THROW(logger.initialize(config));

    // Try to initialize again - should throw
    EXPECT_THROW(logger.initialize(config), TradeError);
}

TEST_F(LoggerTest, LogLevels) {
    auto& logger = Logger::instance();
    
    LoggerConfig config;
    config.min_level = LogLevel::INFO;
    config.destination = LogDestination::FILE;
    config.log_directory = test_log_dir;
    config.filename_prefix = "test_log";
    
    logger.initialize(config);

    // These should be logged
    logger.log(LogLevel::INFO, "Info message");
    logger.log(LogLevel::WARNING, "Warning message");
    logger.log(LogLevel::ERROR, "Error message");
    
    // These should be filtered out
    logger.log(LogLevel::DEBUG, "Debug message");
    logger.log(LogLevel::TRACE, "Trace message");

    // Read log file and verify
    auto log_files = std::filesystem::directory_iterator(test_log_dir);
    ASSERT_NE(log_files.begin(), log_files.end()) << "No log file created";
    
    std::string log_content = read_log_file(log_files.begin()->path().string());
    
    EXPECT_TRUE(log_content.find("Info message") != std::string::npos);
    EXPECT_TRUE(log_content.find("Warning message") != std::string::npos);
    EXPECT_TRUE(log_content.find("Error message") != std::string::npos);
    EXPECT_TRUE(log_content.find("Debug message") == std::string::npos);
    EXPECT_TRUE(log_content.find("Trace message") == std::string::npos);
}

TEST_F(LoggerTest, LogRotation) {
    auto& logger = Logger::instance();
    
    LoggerConfig config;
    config.min_level = LogLevel::INFO;
    config.destination = LogDestination::FILE;
    config.log_directory = test_log_dir;
    config.filename_prefix = "test_log";
    config.max_file_size = 100;  // Small size to trigger rotation
    config.max_files = 3;
    
    logger.initialize(config);

    // Write enough logs to trigger rotation
    std::string long_message(50, 'x');  // 50-character message
    for (int i = 0; i < 10; i++) {
        logger.log(LogLevel::INFO, long_message);
    }

    // Check number of log files
    size_t file_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(test_log_dir)) {
        file_count++;
    }
    
    EXPECT_LE(file_count, config.max_files) 
        << "More log files than maximum allowed";
}

TEST_F(LoggerTest, LogFormatting) {
    auto& logger = Logger::instance();
    
    LoggerConfig config;
    config.min_level = LogLevel::INFO;
    config.destination = LogDestination::FILE;
    config.log_directory = test_log_dir;
    config.filename_prefix = "test_log";
    config.include_timestamp = true;
    config.include_level = true;
    
    logger.initialize(config);

    logger.log(LogLevel::INFO, "Test message");

    // Read log file and verify format
    auto log_files = std::filesystem::directory_iterator(test_log_dir);
    std::string log_content = read_log_file(log_files.begin()->path().string());
    
    // Check for timestamp format: YYYY-MM-DD HH:MM:SS
    std::regex timestamp_regex("\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}");
    EXPECT_TRUE(std::regex_search(log_content, timestamp_regex))
        << "Timestamp format incorrect";
    
    // Check for log level
    EXPECT_TRUE(log_content.find("[INFO]") != std::string::npos)
        << "Log level not found in message";
    
    // Check for actual message
    EXPECT_TRUE(log_content.find("Test message") != std::string::npos)
        << "Log message not found";
}

TEST_F(LoggerTest, MacroUsage) {
    auto& logger = Logger::instance();
    
    LoggerConfig config;
    config.min_level = LogLevel::DEBUG;
    config.destination = LogDestination::FILE;
    config.log_directory = test_log_dir;
    config.filename_prefix = "test_log";
    
    logger.initialize(config);

    // Test all logging macros
    TRACE("Trace message");
    DEBUG("Debug message");
    INFO("Info message");
    WARN("Warning message");
    ERROR("Error message");
    FATAL("Fatal message");

    // Read log file and verify
    auto log_files = std::filesystem::directory_iterator(test_log_dir);
    std::string log_content = read_log_file(log_files.begin()->path().string());
    
    EXPECT_TRUE(log_content.find("Debug message") != std::string::npos);
    EXPECT_TRUE(log_content.find("Info message") != std::string::npos);
    EXPECT_TRUE(log_content.find("Warning message") != std::string::npos);
    EXPECT_TRUE(log_content.find("Error message") != std::string::npos);
    EXPECT_TRUE(log_content.find("Fatal message") != std::string::npos);
    // Trace should be filtered out even at DEBUG level
    EXPECT_TRUE(log_content.find("Trace message") == std::string::npos);
}

TEST_F(LoggerTest, ConsoleOutput) {
    auto& logger = Logger::instance();
    
    LoggerConfig config;
    config.min_level = LogLevel::INFO;
    config.destination = LogDestination::CONSOLE;
    
    logger.initialize(config);

    // Redirect cout to stringstream for testing
    std::stringstream buffer;
    std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

    logger.log(LogLevel::INFO, "Console test message");

    // Restore cout
    std::cout.rdbuf(old);

    // Verify output
    std::string output = buffer.str();
    EXPECT_TRUE(output.find("Console test message") != std::string::npos);
}

TEST_F(LoggerTest, DualOutput) {
    auto& logger = Logger::instance();
    
    LoggerConfig config;
    config.min_level = LogLevel::INFO;
    config.destination = LogDestination::BOTH;
    config.log_directory = test_log_dir;
    config.filename_prefix = "test_log";
    
    logger.initialize(config);

    // Capture console output
    std::stringstream buffer;
    std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

    logger.log(LogLevel::INFO, "Dual output test");

    // Restore cout
    std::cout.rdbuf(old);

    // Verify console output
    std::string console_output = buffer.str();
    EXPECT_TRUE(console_output.find("Dual output test") != std::string::npos);

    // Verify file output
    auto log_files = std::filesystem::directory_iterator(test_log_dir);
    std::string file_output = read_log_file(log_files.begin()->path().string());
    EXPECT_TRUE(file_output.find("Dual output test") != std::string::npos);
}

TEST_F(LoggerTest, MinLevelChange) {
    auto& logger = Logger::instance();
    
    LoggerConfig config;
    config.min_level = LogLevel::INFO;
    config.destination = LogDestination::FILE;
    config.log_directory = test_log_dir;
    config.filename_prefix = "test_log";
    
    logger.initialize(config);

    // Initial level is INFO
    logger.log(LogLevel::DEBUG, "Initial debug message");
    logger.log(LogLevel::INFO, "Initial info message");

    // Change level to DEBUG
    logger.set_level(LogLevel::DEBUG);
    EXPECT_EQ(logger.get_min_level(), LogLevel::DEBUG);

    logger.log(LogLevel::DEBUG, "Second debug message");
    logger.log(LogLevel::INFO, "Second info message");

    // Read log file and verify
    auto log_files = std::filesystem::directory_iterator(test_log_dir);
    std::string log_content = read_log_file(log_files.begin()->path().string());
    
    EXPECT_TRUE(log_content.find("Initial debug message") == std::string::npos);
    EXPECT_TRUE(log_content.find("Initial info message") != std::string::npos);
    EXPECT_TRUE(log_content.find("Second debug message") != std::string::npos);
    EXPECT_TRUE(log_content.find("Second info message") != std::string::npos);
}