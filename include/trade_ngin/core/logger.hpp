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
#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/core/json_wrapper.hpp"

namespace trade_ngin {

/**
 * @brief Log levels for different types of messages
 */
enum class LogLevel {
    TRACE,    // Detailed debug information
    DEBUG,    // General debug information
    INFO,     // General information
    WARNING,  // Warnings that don't affect operation
    ERR,      // Errors that affect operation but don't stop system
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

inline std::string level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO: return "INFO";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::ERR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

inline LogLevel string_to_level(const std::string& level_str) {
    if (level_str == "TRACE") return LogLevel::TRACE;
    if (level_str == "DEBUG") return LogLevel::DEBUG;
    if (level_str == "INFO") return LogLevel::INFO;
    if (level_str == "WARNING") return LogLevel::WARNING;
    if (level_str == "ERROR") return LogLevel::ERR;
    if (level_str == "FATAL") return LogLevel::FATAL;
    return LogLevel::INFO; // Default
}

inline std::string log_destination_to_string(LogDestination dest) {
    switch (dest) {
        case LogDestination::CONSOLE: return "CONSOLE";
        case LogDestination::FILE: return "FILE";
        case LogDestination::BOTH: return "BOTH";
        default: return "UNKNOWN";
    }
}

inline LogDestination string_to_log_destination(const std::string& dest_str) {
    if (dest_str == "CONSOLE") return LogDestination::CONSOLE;
    if (dest_str == "FILE") return LogDestination::FILE;
    if (dest_str == "BOTH") return LogDestination::BOTH;
    return LogDestination::CONSOLE; // Default
}

/**
 * @brief Configuration for the logger
 */
struct LoggerConfig : public JsonSerializable {
    LogLevel min_level{LogLevel::INFO};         // Minimum level to log
    LogDestination destination{LogDestination::CONSOLE};
    std::string log_directory{"logs"};          // Directory for log files
    std::string filename_prefix{"trade_ngin"};  // Prefix for log files
    bool include_timestamp{true};               // Include timestamp in logs
    bool include_level{true};                   // Include log level in logs
    size_t max_file_size{50 * 1024 * 1024};    // Max log file size (50MB)
    size_t max_files{10};                       // Maximum number of log files to keep
    bool allow_reinitialize{false};             // Allow reinitializing the logger

    // Configuration metadata
    std::string version{"1.0.0"};               // Configuration version

    // Implement serialization methods
    JsonWrapper to_json() const override {
        JsonWrapper j;
        j.set_string("min_level", level_to_string(min_level));
        j.set_string("destination", log_destination_to_string(destination));
        j.set_string("log_directory", log_directory);
        j.set_string("filename_prefix", filename_prefix);
        j.set_bool("include_timestamp", include_timestamp);
        j.set_bool("include_level", include_level);
        j.set_int("max_file_size", static_cast<int>(max_file_size));
        j.set_int("max_files", static_cast<int>(max_files));
        j.set_bool("allow_reinitialize", allow_reinitialize);
        j.set_string("version", version);
        
        return j;
    }

    void from_json(const JsonWrapper& j) override {
        min_level = string_to_level(j.get_string("min_level", level_to_string(min_level)));
        destination = string_to_log_destination(j.get_string("destination", log_destination_to_string(destination)));
        log_directory = j.get_string("log_directory", log_directory);
        filename_prefix = j.get_string("filename_prefix", filename_prefix);
        include_timestamp = j.get_bool("include_timestamp", include_timestamp);
        include_level = j.get_bool("include_level", include_level);
        max_file_size = static_cast<size_t>(j.get_int("max_file_size", static_cast<int>(max_file_size)));
        max_files = static_cast<size_t>(j.get_int("max_files", static_cast<int>(max_files)));
        allow_reinitialize = j.get_bool("allow_reinitialize", allow_reinitialize);
        version = j.get_string("version", version);
    }
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
     * @return true if initialization was successful, false if already initialized and not allowed to reinitialize
     */
    bool initialize(const LoggerConfig& config);

    /**
     * @brief Check if logger has been initialized
     * @return true if initialized, false otherwise
     */
    bool is_initialized() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return initialized_;
    }

    /**
     * @brief Reset the logger for testing
     */
    static void reset_for_tests() {
        std::lock_guard<std::mutex> lock(instance().mutex_);
        instance().initialized_ = false;
        if (instance().log_file_.is_open()) {
            instance().log_file_.close();
        }
    }

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
        std::lock_guard<std::mutex> lock(mutex_);
        return config_.min_level;
    }

    /**
     * @brief Get the current logger configuration
     * @return Current logger configuration
     */
    LoggerConfig get_config() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_;
    }

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

    mutable std::mutex mutex_;
    LoggerConfig config_;
    std::ofstream log_file_;
    bool initialized_{false};
    std::string component_name_;
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
#define ERROR(message) LOG(LogLevel::ERR, message)
#define FATAL(message) LOG(LogLevel::FATAL, message)

} // namespace trade_ngin