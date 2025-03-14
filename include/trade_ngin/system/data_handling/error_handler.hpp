#ifndef ERROR_HANDLER_HPP
#define ERROR_HANDLER_HPP

#include "Logger.hpp"
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <functional>
#include <mutex>
#include <memory>

class ErrorHandler {
public:
    // Error severity levels
    enum class Severity {
        INFO,
        WARNING,
        ERROR,
        CRITICAL
    };
    
    // Error categories
    enum class Category {
        NETWORK,
        API,
        DATA,
        TRADING,
        SYSTEM
    };
    
    // Error record structure
    struct ErrorRecord {
        std::string errorId;
        std::string message;
        Severity severity;
        Category category;
        std::chrono::system_clock::time_point timestamp;
        std::string context;
        int retryCount{0};
        bool resolved{false};
    };
    
    // Retry strategy configuration
    struct RetryConfig {
        int maxRetries;
        std::chrono::milliseconds initialDelay;
        double backoffMultiplier;
        std::chrono::milliseconds maxDelay;

        RetryConfig()
            : maxRetries(3),
              initialDelay(100),
              backoffMultiplier(2.0),
              maxDelay(5000) {}
    };
    
    static ErrorHandler& getInstance() {
        static ErrorHandler instance;
        return instance;
    }
    
    // Record a new error
    std::string recordError(const std::string& message,
                           Severity severity,
                           Category category,
                           const std::string& context = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        
        ErrorRecord record {
            generateErrorId(),
            message,
            severity,
            category,
            std::chrono::system_clock::now(),
            context,
            0,
            false
        };
        
        errors_[record.errorId] = record;
        
        // Log the error
        std::map<std::string, std::string> logContext {
            {"error_id", record.errorId},
            {"category", categoryToString(category)},
            {"severity", severityToString(severity)}
        };
        
        Logger::getInstance().logWithContext(
            severityToLogLevel(severity),
            logContext,
            "Error: {}",
            message
        );
        
        // Handle critical errors immediately
        if (severity == Severity::CRITICAL) {
            handleCriticalError(record);
        }
        
        return record.errorId;
    }
    
    // Execute operation with retry logic
    template<typename F>
    auto executeWithRetry(F&& operation,
                         const std::string& operationName,
                         const RetryConfig& config = RetryConfig()) {
        using ReturnType = std::invoke_result_t<F>;
        
        int retryCount = 0;
        std::chrono::milliseconds delay = config.initialDelay;
        
        while (true) {
            try {
                Logger::getInstance().startOperation(operationName);
                if constexpr (std::is_void_v<ReturnType>) {
                    operation();
                    Logger::getInstance().endOperation(operationName);
                    return;
                } else {
                    auto result = operation();
                    Logger::getInstance().endOperation(operationName);
                    return result;
                }
            } catch (const std::exception& e) {
                Logger::getInstance().endOperation(operationName);
                
                if (retryCount >= config.maxRetries) {
                    recordError(e.what(), Severity::ERROR, Category::SYSTEM,
                              "Max retries exceeded for " + operationName);
                    throw;
                }
                
                recordError(e.what(), Severity::WARNING, Category::SYSTEM,
                          "Retry " + std::to_string(retryCount + 1) + 
                          " for " + operationName);
                
                std::this_thread::sleep_for(delay);
                delay = std::min(
                    std::chrono::milliseconds(
                        static_cast<long>(delay.count() * config.backoffMultiplier)
                    ),
                    config.maxDelay
                );
                retryCount++;
            }
        }
    }
    
    // Get error statistics
    std::map<Category, int> getErrorStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<Category, int> stats;
        
        for (const auto& [_, error] : errors_) {
            stats[error.category]++;
        }
        
        return stats;
    }
    
    // Get unresolved errors
    std::vector<ErrorRecord> getUnresolvedErrors() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ErrorRecord> unresolved;
        
        for (const auto& [_, error] : errors_) {
            if (!error.resolved) {
                unresolved.push_back(error);
            }
        }
        
        return unresolved;
    }
    
    // Mark error as resolved
    void resolveError(const std::string& errorId) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = errors_.find(errorId); it != errors_.end()) {
            it->second.resolved = true;
            Logger::getInstance().info("Error {} marked as resolved", errorId);
        }
    }

private:
    ErrorHandler() = default;
    ~ErrorHandler() = default;
    ErrorHandler(const ErrorHandler&) = delete;
    ErrorHandler& operator=(const ErrorHandler&) = delete;
    
    mutable std::mutex mutex_;
    std::map<std::string, ErrorRecord> errors_;
    
    // Generate unique error ID
    std::string generateErrorId() const {
        static int counter = 0;
        return "ERR-" + std::to_string(++counter) + "-" +
               std::to_string(
                   std::chrono::system_clock::now().time_since_epoch().count()
               );
    }
    
    // Convert severity to string
    static std::string severityToString(Severity severity) {
        switch (severity) {
            case Severity::INFO: return "INFO";
            case Severity::WARNING: return "WARNING";
            case Severity::ERROR: return "ERROR";
            case Severity::CRITICAL: return "CRITICAL";
            default: return "UNKNOWN";
        }
    }
    
    // Convert category to string
    static std::string categoryToString(Category category) {
        switch (category) {
            case Category::NETWORK: return "NETWORK";
            case Category::API: return "API";
            case Category::DATA: return "DATA";
            case Category::TRADING: return "TRADING";
            case Category::SYSTEM: return "SYSTEM";
            default: return "UNKNOWN";
        }
    }
    
    // Convert severity to spdlog level
    static spdlog::level::level_enum severityToLogLevel(Severity severity) {
        switch (severity) {
            case Severity::INFO: return spdlog::level::info;
            case Severity::WARNING: return spdlog::level::warn;
            case Severity::ERROR: return spdlog::level::err;
            case Severity::CRITICAL: return spdlog::level::critical;
            default: return spdlog::level::info;
        }
    }
    
    // Handle critical errors
    void handleCriticalError(const ErrorRecord& error) {
        // Log critical error
        Logger::getInstance().error(
            "CRITICAL ERROR: {} (ID: {})", error.message, error.errorId
        );
        
        // Notify administrators (implement notification system here)
        
        // Take emergency actions based on category
        switch (error.category) {
            case Category::TRADING:
                // Emergency trading shutdown
                break;
            case Category::SYSTEM:
                // System health check
                break;
            default:
                break;
        }
    }
};

#endif // ERROR_HANDLER_HPP 