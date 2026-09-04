// src/core/logger.cpp

#include "trade_ngin/core/logger.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <vector>
#include <iomanip>
#include <iostream>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/time_utils.hpp"

namespace trade_ngin {

// Thread-local variable to store the current component name
thread_local std::string Logger::current_component_;

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

namespace {

// drift-F: this session's own directory -- `log_directory`, plus `log_subdirectory` when the
// caller set one (a replay sets the date). Empty subdirectory reproduces the old path exactly.
std::filesystem::path session_log_dir(const trade_ngin::LoggerConfig& cfg) {
    std::filesystem::path dir = std::filesystem::absolute(cfg.log_directory);
    if (!cfg.log_subdirectory.empty()) dir /= cfg.log_subdirectory;
    return dir;
}

// drift-F: does `name` match the file THIS logger writes -- `<prefix>_YYYYMMDD_HHMMSS_partN.log`?
//
// A bare `starts_with(prefix)` is not ownership, because the prefixes nest. `live_trend` is a
// prefix of `live_trend_conservative`, and `bt_portfolio` of `bt_portfolio_conservative`, so
// the conservative runners' logs counted against the base runners' budgets and were deleted by
// them -- the same defect drift-F set out to fix, one level down and between the two futures
// books rather than between the two asset classes.
//
// The whole filename is checked rather than just a separator: retention then deletes only
// files this logger actually wrote. A hand-made `live_trend_notes.txt` sitting in `logs/`
// survives, and so does anything written under an older naming scheme -- it stops being
// pruned, which is the safe direction for a function whose only action is `remove()`.
bool is_own_log_file(const std::string& name, const std::string& prefix) {
    size_t i = 0;
    auto literal = [&](const char* text) {
        const size_t n = std::strlen(text);
        if (i > name.size() || name.size() - i < n) return false;
        if (name.compare(i, n, text) != 0) return false;
        i += n;
        return true;
    };
    auto digits = [&](size_t n) {
        if (i > name.size() || name.size() - i < n) return false;
        for (size_t k = 0; k < n; ++k) {
            if (!std::isdigit(static_cast<unsigned char>(name[i + k]))) return false;
        }
        i += n;
        return true;
    };

    if (!literal(prefix.c_str())) return false;
    if (!literal("_")) return false;
    if (!digits(8)) return false;              // YYYYMMDD
    if (!literal("_")) return false;
    if (!digits(6)) return false;              // HHMMSS
    if (!literal("_part")) return false;
    const size_t part_number_start = i;        // at least one digit
    while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) ++i;
    if (i == part_number_start) return false;
    return literal(".log") && i == name.size();
}

// drift-F: the files this logger OWNS in that directory, oldest first. Rotation used to take
// every regular file in `log_directory` regardless of name, so one runner's retention budget
// deleted another runner's logs.
std::vector<std::filesystem::path> owned_log_files(const std::filesystem::path& dir,
                                                   const std::string& prefix) {
    std::vector<std::filesystem::path> files;
    if (!std::filesystem::exists(dir)) return files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!std::filesystem::is_regular_file(entry.path())) continue;
        if (!is_own_log_file(entry.path().filename().string(), prefix)) continue;  // not ours
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return std::filesystem::last_write_time(a) < std::filesystem::last_write_time(b);
    });
    return files;
}

}  // namespace

void Logger::initialize(const LoggerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;

    // Close any file this logger already holds before opening another. `ofstream::open` on an
    // already-open stream FAILS and leaves the old file attached, and `is_open()` then returns
    // true, so the guard below would not notice: a second initialize() silently kept writing
    // to the first destination. Every runner initializes exactly once, so nothing in
    // production changed -- but rotate_log_files() has always closed first, and the asymmetry
    // is what made retention untestable in a single process (drift-F).
    if (log_file_.is_open()) {
        log_file_.close();
    }

    if (config_.destination == LogDestination::FILE ||
        config_.destination == LogDestination::BOTH) {
        // Create full path if it doesn't exist
        std::filesystem::path log_dir = session_log_dir(config_);
        std::filesystem::create_directories(log_dir);

        // Enforce retention before creating a new file so total never exceeds max_files.
        // Scoped to this logger's own files in this session's own directory (drift-F).
        {
            auto log_files = owned_log_files(log_dir, config_.filename_prefix);
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

    std::filesystem::path log_dir = session_log_dir(config_);

    auto log_files = owned_log_files(log_dir, config_.filename_prefix);

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