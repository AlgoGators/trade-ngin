// src/core/logger.cpp

#include "trade_ngin/core/logger.hpp"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/time_utils.hpp"

namespace trade_ngin {

// Helper function to generate formatted timestamp
std::string generate_session_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);

    std::tm time_info;
    core::safe_localtime(&now_c, &time_info);

    char time_str[32];
    std::strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &time_info);
    return std::string(time_str);
}

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

void Logger::initialize(const LoggerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;

    if (config_.destination == LogDestination::FILE ||
        config_.destination == LogDestination::BOTH) {
        // Create full path if it doesn't exist
        std::filesystem::path log_dir = std::filesystem::absolute(config_.log_directory);
        
        // Try to create directories and check if it succeeded
        std::error_code ec;
        std::filesystem::create_directories(log_dir, ec);
        if (ec) {
            throw std::runtime_error("Failed to create log directory: " + log_dir.string() + " - " + ec.message());
        }

        // Enforce retention before creating a new file so total never exceeds max_files
        {
            std::vector<std::filesystem::path> log_files;
            for (const auto& entry : std::filesystem::directory_iterator(log_dir)) {
                if (std::filesystem::is_regular_file(entry.path())) {
                    log_files.push_back(entry.path());
                }
            }
            std::sort(log_files.begin(), log_files.end(), [](const auto& a, const auto& b) {
                return std::filesystem::last_write_time(a) < std::filesystem::last_write_time(b);
            });
            while (log_files.size() >= config_.max_files) {
                std::filesystem::remove(log_files.front());
                log_files.erase(log_files.begin());
            }
        }

        // Generate session timestamp and reset part number
        current_session_timestamp_ = generate_session_timestamp();
        current_part_number_ = 1;

        // Build log filename with new format: prefix_YYYYMMDD_HHMMSS_partN.log
        std::filesystem::path log_path =
            log_dir / (config_.filename_prefix + "_" + current_session_timestamp_ + "_part" +
                       std::to_string(current_part_number_) + ".log");

        // Open log file
        log_file_.open(log_path, std::ios::app);

        if (!log_file_.is_open()) {
            throw std::runtime_error("Failed to open log file: " + log_path.string());
        }
    }

    initialized_.store(true, std::memory_order_release);
}

Logger::~Logger() {
    if (log_file_.is_open()) {
        log_file_.close();
    }
}

void Logger::log(LogLevel level, const std::string& message) {
    // Check if logger is initialized
    if (!initialized_.load(std::memory_order_acquire)) {
        std::cerr << "WARNING: Logger not initialized. Message: " << message << std::endl;
        return;
    }

    // Acquire lock early to ensure thread-safe access to config_
    std::lock_guard<std::mutex> lock(mutex_);

    if (level < config_.min_level) {
        return;
    }

    std::string formatted_message = format_message(level, message);

    if (config_.destination == LogDestination::CONSOLE ||
        config_.destination == LogDestination::BOTH) {
        write_to_console_unsafe(formatted_message);
    }

    if (config_.destination == LogDestination::FILE ||
        config_.destination == LogDestination::BOTH) {
        write_to_file_unsafe(formatted_message);
        if (log_file_.is_open()) {
            log_file_.flush();
        }
    }
}

std::string Logger::format_message(LogLevel level, const std::string& message) {
    std::ostringstream ss;

    // Add timestamp if configured
    if (config_.include_timestamp) {
        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);

        // Use thread-safe time utilities
        std::tm time_info;
        core::safe_localtime(&now_c, &time_info);

        char time_str[32];
        std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &time_info);
        ss << time_str << " ";
    }

    // Add log level if configured
    if (config_.include_level) {
        ss << "[" << level_to_string(level) << "] ";
    }

    // Add component name if available
    if (!current_component_.empty()) {
        ss << "[" << current_component_ << "] ";
    }

    // Add the actual message
    ss << message;

    return ss.str();
}

void Logger::write_to_console(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    write_to_console_unsafe(message);
}

void Logger::write_to_console_unsafe(const std::string& message) {
    // Assumes mutex is already held
    std::cout << message << std::endl;
    std::cout.flush();
}

void Logger::write_to_file(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    write_to_file_unsafe(message);
}

void Logger::write_to_file_unsafe(const std::string& message) {
    // Assumes mutex is already held
    if (log_file_.is_open()) {
        log_file_ << message << std::endl;
        log_file_.flush();

        // Check file size and rotate if necessary
        if (log_file_.tellp() >= static_cast<std::streampos>(config_.max_file_size)) {
            rotate_log_files();
        }
    }
}

void Logger::rotate_log_files() {
    log_file_.close();  // Explicitly close before handling files

    std::filesystem::path log_dir = std::filesystem::absolute(config_.log_directory);

    std::vector<std::filesystem::path> log_files;
    for (const auto& entry : std::filesystem::directory_iterator(log_dir)) {
        if (std::filesystem::is_regular_file(entry.path())) {
            log_files.push_back(entry.path());
        }
    }

    std::sort(log_files.begin(), log_files.end(), [](const auto& a, const auto& b) {
        return std::filesystem::last_write_time(a) < std::filesystem::last_write_time(b);
    });

    while (log_files.size() >= config_.max_files) {
        std::filesystem::remove(log_files.front());
        log_files.erase(log_files.begin());
    }

    // Increment part number for the new file
    current_part_number_++;

    // Use the new naming format: prefix_YYYYMMDD_HHMMSS_partN.log
    std::filesystem::path new_filename =
        log_dir / (config_.filename_prefix + "_" + current_session_timestamp_ + "_part" +
                   std::to_string(current_part_number_) + ".log");

    log_file_.open(new_filename, std::ios::app);
}

}  // namespace trade_ngin