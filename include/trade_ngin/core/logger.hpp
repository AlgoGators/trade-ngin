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
#include <nlohmann/json.hpp>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/config_base.hpp"

namespace trade_ngin {

/**
 * @brief Log levels for different types of messages
 */
enum class LogLevel {
    TRACE,    // Detailed debug information
    DEBUG,    // General debug information
    INFO,     // General information
    WARNING,  // Warnings that don't affect operation
    ERR,    // Errors that affect operation but don't stop system
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

inline std::string log_destination_to_string(LogDestination dest) {
    switch (dest) {
        case LogDestination::CONSOLE: return "CONSOLE";
        case LogDestination::FILE: return "FILE";
        case LogDestination::BOTH: return "BOTH";
        default: return "UNKNOWN";
    }
}

/**
 * @brief Configuration for the logger
 */
struct LoggerConfig : public ConfigBase {
    LogLevel min_level{LogLevel::INFO};         // Minimum level to log
    LogDestination destination{LogDestination::CONSOLE};
    std::string log_directory{"logs"};          // Directory for log files
    std::string filename_prefix{"trade_ngin"};  // Prefix for log files
    bool include_timestamp{true};               // Include timestamp in logs
    bool include_level{true};                   // Include log level in logs
    size_t max_file_size{50 * 1024 * 1024};    // Max log file size (50MB)
    size_t max_files{10};                       // Maximum number of log files to keep

    // Configuration metadata
    std::string version{"1.0.0"};               // Configuration version

    // Implement serialization methods
    nlohmann::json to_json() const override {
        nlohmann::json j;
        j["min_level"] = level_to_string(min_level);
        j["destination"] = log_destination_to_string(destination);
        j["log_directory"] = log_directory;
        j["filename_prefix"] = filename_prefix;
        j["include_timestamp"] = include_timestamp;
        j["include_level"] = include_level;
        j["max_file_size"] = max_file_size;
        j["max_files"] = max_files;
        j["version"] = version;
        
        return j;
    }

    void from_json(const nlohmann::json& j) override {
        if (j.contains("min_level")) {
            std::string level_str = j.at("min_level").get<std::string>();
            if (level_str == "TRACE") min_level = LogLevel::TRACE;
            else if (level_str == "DEBUG") min_level = LogLevel::DEBUG;
            else if (level_str == "INFO") min_level = LogLevel::INFO;
            else if (level_str == "WARNING") min_level = LogLevel::WARNING;
            else if (level_str == "ERROR") min_level = LogLevel::ERR;
            else if (level_str == "FATAL") min_level = LogLevel::FATAL;
        }
        if (j.contains("destination")) {
            std::string dest_str = j.at("destination").get<std::string>();
            if (dest_str == "CONSOLE") destination = LogDestination::CONSOLE;
            else if (dest_str == "FILE") destination = LogDestination::FILE;
            else if (dest_str == "BOTH") destination = LogDestination::BOTH;
        }
        if (j.contains("log_directory")) log_directory = j.at("log_directory").get<std::string>();
        if (j.contains("filename_prefix")) filename_prefix = j.at("filename_prefix").get<std::string>();
        if (j.contains("include_timestamp")) include_timestamp = j.at("include_timestamp").get<bool>();
        if (j.contains("include_level")) include_level = j.at("include_level").get<bool>();
        if (j.contains("max_file_size")) max_file_size = j.at("max_file_size").get<size_t>();
        if (j.contains("max_files")) max_files = j.at("max_files").get<size_t>();
        if (j.contains("version")) version = j.at("version").get<std::string>();
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
     */
    void initialize(const LoggerConfig& config);

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
        return config_.min_level;
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
#define ERROR(message) LOG(LogLevel::ERR, message)
#define FATAL(message) LOG(LogLevel::FATAL, message)
} // namespace trade_ngin