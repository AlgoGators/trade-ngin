#pragma once

#include "trade_ngin/core/logger.hpp"
#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>

namespace trade_ngin {

/**
 * @brief Centralized manager for logging configuration
 * 
 * This class provides a single point for configuring logging across
 * the entire system, allowing components to get appropriate logging
 * configuration while maintaining consistent settings.
 */
class LogManager {
public:
    /**
     * @brief Get the singleton instance
     * @return Reference to the log manager instance
     */
    static LogManager& instance() {
        static LogManager instance;
        return instance;
    }

    /**
     * @brief Initialize the global logging configuration
     * @param config Base configuration for all loggers
     */
    void initialize(const LoggerConfig& config);

    /**
     * @brief Get logger configuration for a specific component
     * 
     * This returns a configuration based on the global settings
     * but with a component-specific filename prefix.
     * 
     * @param component_name Name of the component
     * @return Configured LoggerConfig for the component
     */
    LoggerConfig get_component_config(const std::string& component_name);

    /**
     * @brief Configure a component's logger directly
     * 
     * This is a convenience method that gets the appropriate config
     * and initializes the logger with it.
     * 
     * @param component_name Name of the component
     * @return true if initialization was successful
     */
    bool configure_component_logger(const std::string& component_name);

    /**
     * @brief Set the global log level
     * 
     * Changes the log level for all future logger initializations
     * and attempts to update existing loggers if possible.
     * 
     * @param level New log level
     */
    void set_global_log_level(LogLevel level);

    /**
     * @brief Get the global logger configuration
     * @return Current global logger configuration
     */
    LoggerConfig get_global_config() const;

private:
    LogManager() = default;
    ~LogManager() = default;

    // Delete copy and move to ensure singleton
    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;
    LogManager(LogManager&&) = delete;
    LogManager& operator=(LogManager&&) = delete;

    bool initialized_{false};
    mutable std::mutex mutex_;
    LoggerConfig global_config_;
    std::unordered_map<std::string, bool> configured_components_;
};

} // namespace trade_ngin 