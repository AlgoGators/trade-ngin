// src/core/logger.cpp

#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>

namespace trade_ngin {

bool Logger::initialize(const LoggerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if already initialized
    if (initialized_) {
        // If reinitialization is not allowed, return false
        if (!config.allow_reinitialize) {
            // Still allow changing log level even if not reinitializing
            config_.min_level = config.min_level;
            
            std::cout << "Logger already initialized. Only updating log level." << std::endl;
            return false;
        }
        
        // Close existing file handles
        if (log_file_.is_open()) {
            log_file_.close();
        }
        initialized_ = false;
    }

    config_ = config;

    if (config_.destination == LogDestination::FILE || 
        config_.destination == LogDestination::BOTH) {
        // Create full path if it doesn't exist
        std::filesystem::path log_dir = std::filesystem::absolute(config_.log_directory);
        std::filesystem::create_directories(log_dir);

        // Build log filename
        auto now = std::chrono::system_clock::now().time_since_epoch();
        std::string timestamp = std::to_string(
            std::chrono::duration_cast<std::chrono::seconds>(now).count()
        );
        
        std::filesystem::path log_path = log_dir / 
            (config_.filename_prefix + "_" + timestamp + ".log");
        
        // Open log file
        log_file_.open(log_path, std::ios::app);
        
        if (!log_file_.is_open()) {
            std::cerr << "Failed to open log file: " << log_path.string() << std::endl;
            return false;
        }
    }

    initialized_ = true;
    
    // Log initialization success to the configured destination
    std::string init_message = "Logger initialized with level=" + 
        level_to_string(config_.min_level) + 
        ", destination=" + log_destination_to_string(config_.destination) +
        ", filename_prefix=" + config_.filename_prefix;
    
    log(LogLevel::INFO, init_message);
    
    return true;
}

Logger::~Logger() {
    if (log_file_.is_open()) {
        log_file_.close();
    }
}

void Logger::log(LogLevel level, const std::string& message) {
    // Fast check without locking
    if (level < config_.min_level) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!initialized_) {
        // Write to console even if not initialized
        std::cout << "[UNINIT][" << level_to_string(level) << "] " << message << std::endl;
        return;
    }

    std::string formatted_message = format_message(level, message);

    if (config_.destination == LogDestination::CONSOLE || 
        config_.destination == LogDestination::BOTH) {
        write_to_console(formatted_message);
    }

    if (config_.destination == LogDestination::FILE || 
        config_.destination == LogDestination::BOTH) {
        write_to_file(formatted_message);
    }
}

std::string Logger::format_message(LogLevel level, const std::string& message) {
    std::ostringstream ss;

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

    if (config_.include_level) {
        ss << "[" << level_to_string(level) << "] ";
    }

    ss << message;
    return ss.str();
}

void Logger::write_to_console(const std::string& message) {
    // No need to lock here as the calling method already has the lock
    std::cout << message << std::endl;
}

void Logger::write_to_file(const std::string& message) {
    // No need to lock here as the calling method already has the lock
    
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
    log_file_.close(); // Explicitly close before handling files

    std::filesystem::path log_dir = std::filesystem::absolute(config_.log_directory);
    
    std::vector<std::filesystem::path> log_files;
    for (const auto& entry : std::filesystem::directory_iterator(log_dir)) {
        if (entry.path().filename().string().find(config_.filename_prefix) != std::string::npos) {
            log_files.push_back(entry.path());
        }
    }

    std::sort(log_files.begin(), log_files.end(), 
              [](const auto& a, const auto& b) {
                  return std::filesystem::last_write_time(a) < 
                         std::filesystem::last_write_time(b);
              });

    while (log_files.size() >= config_.max_files) {
        std::filesystem::remove(log_files.front());
        log_files.erase(log_files.begin());
    }

    // Use absolute path for the new file
    std::filesystem::path new_filename = log_dir / 
        (config_.filename_prefix + "_" + 
         std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + 
         ".log");
    
    log_file_.open(new_filename, std::ios::app);
}

} // namespace trade_ngin