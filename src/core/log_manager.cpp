#include "trade_ngin/core/log_manager.hpp"
#include <iostream>

namespace trade_ngin {

void LogManager::initialize(const LoggerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    global_config_ = config;
    
    // Allow future reinitializations for components
    global_config_.allow_reinitialize = true;
    
    initialized_ = true;
    
    // Initialize the root logger with a simple configuration
    LoggerConfig root_config = global_config_;
    root_config.filename_prefix = "trade_ngin_core";
    
    bool success = Logger::instance().initialize(root_config);
    if (success) {
        INFO("LogManager initialized with level=" + level_to_string(global_config_.min_level));
    } else {
        std::cerr << "LogManager initialization failed" << std::endl;
    }
}

LoggerConfig LogManager::get_component_config(const std::string& component_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!initialized_) {
        // If not initialized, create a default configuration
        global_config_.min_level = LogLevel::INFO;
        global_config_.destination = LogDestination::CONSOLE;
        global_config_.log_directory = "logs";
        global_config_.allow_reinitialize = true;
        initialized_ = true;
    }
    
    // Create a copy of the global config
    LoggerConfig component_config = global_config_;
    
    // Set component-specific settings
    component_config.filename_prefix = component_name;
    
    return component_config;
}

bool LogManager::configure_component_logger(const std::string& component_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if this component has already been configured
    auto it = configured_components_.find(component_name);
    if (it != configured_components_.end() && it->second) {
        return true; // Already configured
    }
    
    // Get component config
    LoggerConfig component_config = get_component_config(component_name);
    
    // Initialize the logger
    bool success = Logger::instance().initialize(component_config);
    
    // Record the configuration state
    configured_components_[component_name] = success;
    
    return success;
}

void LogManager::set_global_log_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Update the global configuration
    global_config_.min_level = level;
    
    // Update the existing logger
    Logger::instance().set_level(level);
    
    INFO("Global log level set to " + level_to_string(level));
}

LoggerConfig LogManager::get_global_config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return global_config_;
}

} // namespace trade_ngin 