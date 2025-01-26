#include <gtest/gtest.h>
#include "trade_ngin/core/logger.hpp"
#include <sstream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <algorithm>

using namespace trade_ngin;

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset logger first to close any existing file handles
        Logger::reset_for_tests();

        // Redirect cout to capture console output
        original_cout = std::cout.rdbuf();
        std::cout.rdbuf(cout_buffer.rdbuf());

        // Ensure test directory is clean
        std::error_code ec;
        std::filesystem::remove_all(test_log_dir, ec);
        std::filesystem::create_directories(test_log_dir);
    }

    void TearDown() override {
        // Restore cout
        std::cout.rdbuf(original_cout);

        // Reset logger BEFORE directory cleanup
        Logger::reset_for_tests();

        // Safe directory removal
        std::error_code ec;
        std::filesystem::remove_all(test_log_dir, ec);
    }

    // Helper to get log files in a directory
    std::vector<std::filesystem::path> get_log_files(const std::string& dir) {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".log") {
                files.push_back(entry.path());
            }
        }
        // Sort by modification time (oldest first)
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return std::filesystem::last_write_time(a) < std::filesystem::last_write_time(b);
        });
        return files;
    }

    // Helper to read file content
    std::string read_file(const std::filesystem::path& path) {
        std::ifstream file(path);
        if (!file.is_open()) return "";
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // Helper to debug directory contents
    std::string list_files(const std::string& dir) {
        std::stringstream ss;
        try {
            if (!std::filesystem::exists(dir)) {
                return "Directory does not exist";
            }
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                ss << entry.path().filename().string() << " ";
            }
            if (ss.str().empty()) {
                return "Directory is empty";
            }
            return ss.str();
        } catch (const std::filesystem::filesystem_error& e) {
            return std::string("Error listing directory: ") + e.what();
        }
    }
    
    std::streambuf* original_cout;
    std::stringstream cout_buffer;
    const std::string test_log_dir = "test_logs";
};

// Add this test case to verify proper handle closure
TEST_F(LoggerTest, FileHandlesClosedAfterReset) {
    LoggerConfig config;
    config.destination = LogDestination::FILE;
    config.log_directory = test_log_dir;
    Logger::instance().initialize(config);
    
    // Explicit reset
    Logger::reset_for_tests();
    
    // Verify directory can be deleted
    std::error_code ec;
    std::filesystem::remove_all(test_log_dir, ec);
    EXPECT_FALSE(ec) << "Failed to delete directory: " << ec.message();
}

TEST_F(LoggerTest, InitializationCreatesLogDirectory) {
    LoggerConfig config;
    config.destination = LogDestination::FILE;
    config.log_directory = test_log_dir + "/subdir";
    ASSERT_NO_THROW(Logger::instance().initialize(config));
    EXPECT_TRUE(std::filesystem::exists(config.log_directory));
}

TEST_F(LoggerTest, LogsToConsoleWhenConfigured) {
    LoggerConfig config;
    config.destination = LogDestination::CONSOLE;
    config.include_timestamp = false;
    config.include_level = false;
    Logger::instance().initialize(config);

    std::string message = "Console message";
    Logger::instance().log(LogLevel::INFO, message);

    EXPECT_EQ(cout_buffer.str(), message + "\n");
}

TEST_F(LoggerTest, LogsToFileWhenConfigured) {
    LoggerConfig config;
    config.destination = LogDestination::FILE;
    config.log_directory = test_log_dir;
    config.include_timestamp = false;
    config.include_level = false;
    Logger::instance().initialize(config);

    Logger::instance().log(LogLevel::INFO, "File message");

    auto files = get_log_files(test_log_dir);
    ASSERT_EQ(files.size(), 1);
    EXPECT_EQ(read_file(files[0]), "File message\n");
}

TEST_F(LoggerTest, LogsToBothDestinations) {
    LoggerConfig config;
    config.destination = LogDestination::BOTH;
    config.log_directory = test_log_dir;
    config.include_timestamp = false;
    config.include_level = false;
    Logger::instance().initialize(config);

    Logger::instance().log(LogLevel::INFO, "Both message");

    // Check console
    EXPECT_EQ(cout_buffer.str(), "Both message\n");

    // Check file
    auto files = get_log_files(test_log_dir);
    ASSERT_EQ(files.size(), 1);
    EXPECT_EQ(read_file(files[0]), "Both message\n");
}

TEST_F(LoggerTest, LogLevelFiltering) {
    LoggerConfig config;
    config.destination = LogDestination::FILE;
    config.log_directory = test_log_dir;
    config.min_level = LogLevel::WARNING;
    config.include_timestamp = false;
    config.include_level = false;
    Logger::instance().initialize(config);

    Logger::instance().log(LogLevel::DEBUG, "Debug");
    Logger::instance().log(LogLevel::INFO, "Info");
    Logger::instance().log(LogLevel::WARNING, "Warning");
    Logger::instance().log(LogLevel::ERR, "Error");

    auto content = read_file(get_log_files(test_log_dir)[0]);
    EXPECT_TRUE(content.find("Debug") == std::string::npos);
    EXPECT_TRUE(content.find("Info") == std::string::npos);
    EXPECT_TRUE(content.find("Warning\nError\n") != std::string::npos);
}

TEST_F(LoggerTest, MessageFormatting) {
    LoggerConfig config;
    config.destination = LogDestination::FILE;
    config.log_directory = test_log_dir;
    config.include_timestamp = true;
    config.include_level = true;
    Logger::instance().initialize(config);

    Logger::instance().log(LogLevel::INFO, "Formatted");

    auto content = read_file(get_log_files(test_log_dir)[0]);
    EXPECT_TRUE(content.find("[INFO]") != std::string::npos);
    EXPECT_TRUE(content.find("Formatted") != std::string::npos);
    EXPECT_GE(content.size(), 20); // Basic timestamp check
}

TEST_F(LoggerTest, FileRotation) {
    LoggerConfig config;
    config.destination = LogDestination::FILE;
    config.log_directory = test_log_dir;
    config.max_file_size = 10; // 10 bytes
    config.max_files = 2;
    config.include_timestamp = false;
    config.include_level = false;
    Logger::instance().initialize(config);

    // Each message is 9 bytes ("12345678\n")
    Logger::instance().log(LogLevel::INFO, "12345678"); // 9 bytes
    Logger::instance().log(LogLevel::INFO, "12345678"); // Triggers rotation

    auto files = get_log_files(test_log_dir);
    ASSERT_EQ(files.size(), 2);
}

TEST_F(LoggerTest, MaxFilesEnforced) {
    LoggerConfig config;
    config.destination = LogDestination::FILE;
    config.log_directory = test_log_dir;
    config.max_file_size = 1; // Rotate every message
    config.max_files = 2;
    config.include_timestamp = false;
    config.include_level = false;
    Logger::instance().initialize(config);

    for (int i = 0; i < 3; ++i) {
        Logger::instance().log(LogLevel::INFO, std::to_string(i));
    }

    auto files = get_log_files(test_log_dir);
    EXPECT_EQ(files.size(), 2); // Oldest file should be deleted
}

TEST_F(LoggerTest, LogBeforeInitializationSilent) {
    Logger::reset_for_tests(); // Ensure uninitialized

    Logger::instance().log(LogLevel::INFO, "Test");
    
    EXPECT_TRUE(cout_buffer.str().empty());
    EXPECT_TRUE(get_log_files(test_log_dir).empty());
}

TEST_F(LoggerTest, ReinitializationSwitchesFile) {
    // Create absolute paths for both directories
    std::filesystem::path dir1 = std::filesystem::absolute(test_log_dir) / "dir1";
    std::filesystem::path dir2 = std::filesystem::absolute(test_log_dir) / "dir2";

    // Ensure both directories don't exist at start
    std::filesystem::remove_all(dir1);
    std::filesystem::remove_all(dir2);

    // First initialization
    LoggerConfig config1;
    config1.destination = LogDestination::FILE;
    config1.log_directory = dir1.string();
    config1.filename_prefix = "test1";
    
    std::cout << "Initializing logger with dir1: " << dir1.string() << std::endl;
    Logger::instance().initialize(config1);
    
    // Write to first log
    Logger::instance().log(LogLevel::INFO, "Dir1");
    
    // Verify first directory
    auto dir1_files = get_log_files(dir1.string());
    std::cout << "Files in dir1: " << list_files(dir1.string()) << std::endl;
    ASSERT_EQ(dir1_files.size(), 1) << "Dir1 should have 1 file";

    // Reset logger before reinitializing
    Logger::reset_for_tests();

    // Create second directory if it doesn't exist
    std::filesystem::create_directories(dir2);

    // Reinitialize with new directory
    LoggerConfig config2;
    config2.destination = LogDestination::FILE;
    config2.log_directory = dir2.string();
    config2.filename_prefix = "test2";
    
    std::cout << "Reinitializing logger with dir2: " << dir2.string() << std::endl;
    Logger::instance().initialize(config2);

    // Verify directory exists
    ASSERT_TRUE(std::filesystem::exists(dir2)) 
        << "Directory not created: " << dir2.string();

    // Write to second log and flush
    Logger::instance().log(LogLevel::INFO, "Dir2");
    
    // Explicitly reset to ensure file is closed
    Logger::reset_for_tests();

    // Debug output
    std::cout << "Checking files in dir2: " << dir2.string() << std::endl;
    std::cout << "Directory exists: " << std::filesystem::exists(dir2) << std::endl;
    std::cout << "Directory listing: " << list_files(dir2.string()) << std::endl;

    // Verify files in dir2
    auto dir2_files = get_log_files(dir2.string());
    ASSERT_EQ(dir2_files.size(), 1) 
        << "Files in dir2: " << list_files(dir2.string()) 
        << "\nDirectory exists: " << std::filesystem::exists(dir2);

    // Verify dir1 still has its file
    EXPECT_EQ(get_log_files(dir1.string()).size(), 1);
}
