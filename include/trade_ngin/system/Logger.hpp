#ifndef LOGGER_HPP
#define LOGGER_HPP

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <chrono>
#include <map>
#include <mutex>
#include <iostream>
#include <nlohmann/json.hpp>

// Add custom formatter for nlohmann::json
template<>
struct fmt::formatter<nlohmann::json> : fmt::formatter<std::string> {
    template<typename FormatContext>
    auto format(const nlohmann::json& j, FormatContext& ctx) {
        return fmt::formatter<std::string>::format(j.dump(), ctx);
    }
};

class Logger {
public:
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }
    
    // Initialize logger with rotation
    void initialize(const std::string& logFile = "trade_ngin.log",
                   size_t maxFileSize = 5 * 1024 * 1024,  // 5MB
                   size_t maxFiles = 3) {
        try {
            // Create rotating file sink
            auto rotating_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                logFile, maxFileSize, maxFiles
            );
            
            // Create console sink with color
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            
            // Create logger with multiple sinks
            logger_ = std::make_shared<spdlog::logger>("trade_ngin",
                spdlog::sinks_init_list({console_sink, rotating_sink}));
            
            // Set pattern for structured logging
            logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
            
            // Set level
            logger_->set_level(spdlog::level::debug);
            
            logger_->info("Logger initialized with rotation");
            
        } catch (const spdlog::spdlog_ex& ex) {
            std::cerr << "Logger initialization failed: " << ex.what() << std::endl;
        }
    }
    
    // Performance metrics tracking
    void startOperation(const std::string& operation) {
        std::lock_guard<std::mutex> lock(metricsMutex_);
        operationStart_[operation] = std::chrono::steady_clock::now();
    }
    
    void endOperation(const std::string& operation) {
        std::lock_guard<std::mutex> lock(metricsMutex_);
        auto start = operationStart_[operation];
        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        // Update metrics
        auto& metrics = operationMetrics_[operation];
        metrics.count++;
        metrics.totalDuration += duration.count();
        metrics.avgDuration = metrics.totalDuration / metrics.count;
        
        if (duration.count() > metrics.maxDuration) {
            metrics.maxDuration = duration.count();
        }
        
        // Log if duration exceeds threshold (e.g., 100ms)
        if (duration.count() > 100000) {
            logger_->warn("Operation {} took {}us (avg: {}us, max: {}us)",
                         operation, duration.count(), metrics.avgDuration, metrics.maxDuration);
        }
    }
    
    // Log performance metrics summary
    void logMetricsSummary() {
        std::lock_guard<std::mutex> lock(metricsMutex_);
        logger_->info("Performance Metrics Summary:");
        for (const auto& [op, metrics] : operationMetrics_) {
            logger_->info("{}: count={}, avg={}us, max={}us",
                         op, metrics.count, metrics.avgDuration, metrics.maxDuration);
        }
    }
    
    // Logging methods with structured data
    template<typename... Args>
    void info(const char* fmt, const Args&... args) {
        logger_->info(fmt, args...);
    }
    
    template<typename... Args>
    void error(const char* fmt, const Args&... args) {
        logger_->error(fmt, args...);
    }
    
    template<typename... Args>
    void warning(const char* fmt, const Args&... args) {
        logger_->warn(fmt, args...);
    }
    
    template<typename... Args>
    void debug(const char* fmt, const Args&... args) {
        logger_->debug(fmt, args...);
    }
    
    // Structured logging with additional context
    template<typename... Args>
    void logWithContext(spdlog::level::level_enum level,
                       const std::map<std::string, std::string>& context,
                       const char* fmt,
                       const Args&... args) {
        std::string contextStr;
        for (const auto& [key, value] : context) {
            contextStr += "[" + key + "=" + value + "] ";
        }
        logger_->log(level, "{}{}", contextStr, fmt::format(fmt, args...));
    }

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
    std::shared_ptr<spdlog::logger> logger_;
    
    // Performance metrics tracking
    struct Metrics {
        uint64_t count{0};
        uint64_t totalDuration{0};
        uint64_t avgDuration{0};
        uint64_t maxDuration{0};
    };
    
    std::mutex metricsMutex_;
    std::map<std::string, std::chrono::steady_clock::time_point> operationStart_;
    std::map<std::string, Metrics> operationMetrics_;
};

#endif // LOGGER_HPP 