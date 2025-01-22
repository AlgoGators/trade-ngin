// include/trade_ngin/core/logger.hpp

#pragma once

#include <string>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <chrono>
#include <filesystem>
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

/**
 * @brief Log levels for different types of messages
 */
enum class LogLevel {
    TRACE,    // Detailed debug information
    DEBUG,    // General debug information
    INFO,     // General information
    WARNING,  // Warnings that don't affect operation
    ERROR,    // Errors that affect operation but don't stop system
    FATAL     // Critical errors that require system shutdown
};

/**
 * @brief Log destination type
 */
enum class LogDestination {
    CONSOLE,  // Standard output
    FILE,     // File output
    BOTH      // Both console and file
};

/**
 * @brief Configuration for the logger
 */
struct LoggerConfig {
    LogLevel min_level{LogLevel::INFO};         // Minimum level to log
    LogDestination destination{LogDestination::CONSOLE};
    std::string log_directory{"logs"};          // Directory for log files
    std::string filename_prefix{"trade_ngin"};  // Prefix for log files
    bool include_timestamp{true};               // Include timestamp in logs
    bool include_level{true};                   // Include log level in logs
    size_t max_file_size{50 * 1024 * 1024};    // Max log file size (50MB)
    size_t max_files{10};                       // Maximum number of log files to keep
};

/**
 * @brief Thread-safe logging class
 */
class Logger {
public:
    /**
     * @brief Get the singleton instance
     * @return Reference to the logger instance
     */
    static Logger& instance() {
        static Logger instance;
        return instance;
    }

    /**
     * @brief Initialize the logger with configuration
     * @param config Logger configuration
     */
    void initialize(const LoggerConfig& config);

    /**
     * @brief Log a message with specified level
     * @param level Log level
     * @param message Message to log
     */
    void log(LogLevel level, const std::string& message);

    /**
     * @brief Set the minimum log level
     * @param level Minimum level to log
     */
    void set_level(LogLevel level) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_.min_level = level;
    }
    
    /**
     * @brief Get the minimum log level
     * @return Minimum log level
     */
    LogLevel get_min_level() const {
        return config_.min_level;
    }

    /**
     * @brief Convert LogLevel to string
     * @param level Log level to convert
     * @return String representation of log level
     */
    static std::string level_to_string(LogLevel level);

private:
    Logger() = default;
    ~Logger();

    // Delete copy and move to ensure singleton
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    void rotate_log_files();
    void write_to_file(const std::string& message);
    void write_to_console(const std::string& message);
    std::string format_message(LogLevel level, const std::string& message);

    std::mutex mutex_;
    LoggerConfig config_;
    std::ofstream log_file_;
    bool initialized_{false};
};

/**
 * @brief Convenience macro for logging
 * Usage: LOG(LogLevel::INFO, "Message: " << variable)
 */
#define LOG(level, message) \
    do { \
        if (level >= Logger::instance().get_min_level()) { \
            std::ostringstream os; \
            os << message; \
            Logger::instance().log(level, os.str()); \
        } \
    } while (0)

// Convenience functions for different log levels
#define TRACE(message) LOG(LogLevel::TRACE, message)
#define DEBUG(message) LOG(LogLevel::DEBUG, message)
#define INFO(message)  LOG(LogLevel::INFO, message)
#define WARN(message)  LOG(LogLevel::WARNING, message)
#define ERROR(message) LOG(LogLevel::ERROR, message)
#define FATAL(message) LOG(LogLevel::FATAL, message)

} // namespace trade_ngin