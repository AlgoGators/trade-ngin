#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <memory>
#include <filesystem>
#include <unordered_map>
#include <mutex>
#include <iostream>


namespace trade_ngin {

/**
 * @brief Environment types for configuration
 */
enum class Environment {
    DEVELOPMENT,
    STAGING,
    PRODUCTION,
    BACKTEST
};

/**
 * @brief Configuration validation error
 */
struct ConfigValidationError {
    std::string field;
    std::string message;
};

/**
 * @brief Configuration type enumeration
 */
enum class ConfigType {
    STRATEGY,
    RISK,
    EXECUTION,
    DATABASE,
    LOGGING
};

/**
 * @brief Base configuration validator interface
 */
class ConfigValidator {
public:
    virtual ~ConfigValidator() = default;
    virtual std::vector<ConfigValidationError> validate(
        const nlohmann::json& config) const = 0;
    virtual ConfigType get_type() const = 0;
};

/**
 * @brief Validator for strategy configuration
 */
class StrategyValidator : public ConfigValidator {
public:
    std::vector<ConfigValidationError> validate(
        const nlohmann::json& config) const override;
    ConfigType get_type() const override { return ConfigType::STRATEGY; }

private:
    bool validate_ema_windows(const nlohmann::json& windows,
                            std::vector<ConfigValidationError>& errors) const;
    bool validate_numeric_range(const nlohmann::json& value,
                              const std::string& field,
                              double min_val,
                              double max_val,
                              std::vector<ConfigValidationError>& errors) const;
};

/**
 * @brief Validator for risk management configuration
 */
class RiskValidator : public ConfigValidator {
public:
    std::vector<ConfigValidationError> validate(
        const nlohmann::json& config) const override;
    ConfigType get_type() const override { return ConfigType::RISK; }

private:
    bool validate_risk_limits(const nlohmann::json& config,
                            std::vector<ConfigValidationError>& errors) const;
};

/**
 * @brief Validator for execution configuration
 */
class ExecutionValidator : public ConfigValidator {
public:
    std::vector<ConfigValidationError> validate(
        const nlohmann::json& config) const override;
    ConfigType get_type() const override { return ConfigType::EXECUTION; }

private:
    bool validate_slippage_model(const nlohmann::json& model,
                               std::vector<ConfigValidationError>& errors) const;
    bool validate_commission_model(const nlohmann::json& model,
                                 std::vector<ConfigValidationError>& errors) const;
};

/**
 * @brief Validator for database configuration
 */
class DatabaseValidator : public ConfigValidator {
public:
    std::vector<ConfigValidationError> validate(
        const nlohmann::json& config) const override;
    ConfigType get_type() const override { return ConfigType::DATABASE; }
};

/**
 * @brief Configuration manager for system-wide settings
 */
class ConfigManager {
public:
    /**
     * @brief Get singleton instance
     */
    static ConfigManager& instance() {
        static ConfigManager instance;
        return instance;
    }

    /**
     * @brief Initialize configuration from files
     * @param base_path Base path to config files
     * @param env Environment to load
     * @return Result indicating success or failure
     */
    Result<void> initialize(
        const std::filesystem::path& base_path,
        Environment env = Environment::DEVELOPMENT);

    /**
     * @brief Get configuration for a component
     * @param component_type Type of component
     * @return Configuration object
     */
    template<typename T>
    Result<T> get_config(ConfigType component_type) const;

    /**
     * @brief Update configuration at runtime
     * @param component_type Component to update
     * @param config New configuration
     * @return Result indicating success or failure
     */
    Result<void> update_config(
        ConfigType component_type,
        const nlohmann::json& config);

    /**
     * @brief Save entire configuration to files
     * @return Result indicating success or failure
     */
    Result<void> save_configs();

    /**
     * @brief Create a default configuration for a component
     * @param component_type Component type
     * @return Default configuration as JSON
     */
    nlohmann::json create_default_config(ConfigType component_type) const;

    /**
     * @brief Get current environment
     */
    Environment get_environment() const { return current_env_; }

    /**
     * @brief Check if in production environment
     */
    bool is_production() const { 
        return current_env_ == Environment::PRODUCTION; 
    }

    /**
     * @brief Convert environment to string
     */
    static std::string environment_to_string(Environment env);

    /**
     * @brief Convert string to environment
     */
    static Environment string_to_environment(const std::string& env_str);

private:
    ConfigManager() = default;
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    Environment current_env_{Environment::DEVELOPMENT};
    std::filesystem::path config_path_;
    nlohmann::json config_;
    std::unordered_map<ConfigType, std::unique_ptr<ConfigValidator>> validators_;
    mutable std::mutex mutex_;

    /**
     * @brief Path to credentials file
     */
    std::filesystem::path get_credentials_path() const {
        return config_path_ / "credentials.json";
    }

    /**
     * @brief Initialize validators
     */
    void initialize_validators();

    /**
     * @brief Load configuration files
     */
    Result<void> load_config_files();

    /**
     * @brief Validate configuration
     * @param component_type Component to validate
     * @param config Configuration to validate
     * @return Result indicating success or failure
     */
    Result<void> validate_config(
        ConfigType component_type,
        const nlohmann::json& config) const;

    /**
     * @brief Apply environment overrides
     */
    Result<void> apply_environment_overrides();

    /**
     * @brief Get component name from type
     */
    static std::string get_component_name(ConfigType type);

    /**
     * @brief Create default strategy configuration
     */
    nlohmann::json create_default_strategy_config() const;

    /**
     * @brief Create default risk configuration
     */
    nlohmann::json create_default_risk_config() const;

    /**
     * @brief Create default execution configuration
     */
    nlohmann::json create_default_execution_config() const;

    /**
     * @brief Create default database configuration
     */
    nlohmann::json create_default_database_config() const;

    /**
     * @brief Create default logging configuration
     */
    nlohmann::json create_default_logging_config() const;
};


// Template implementation
template<typename T>
Result<T> ConfigManager::get_config(ConfigType component_type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        std::string component = get_component_name(component_type);
        
        // Check if component exists
        if (!config_.contains(component)) {
            return make_error<T>(
                ErrorCode::INVALID_ARGUMENT,
                "Component not found: " + component,
                "ConfigManager"
            );
        }

        // Get config JSON
        const auto& component_config = config_[component];

        // Validate config
        auto validation = validate_config(component_type, component_config);
        if (validation.is_error()) {
            return make_error<T>(
                validation.error()->code(),
                validation.error()->what(),
                "ConfigManager"
            );
        }

        T config;

        // Special handling for ConfigBase derived classes
        if constexpr (std::is_base_of<ConfigBase, T>::value) {
            config.from_json(component_config);
        } else {
            // Direct conversion from JSON
            config = component_config.get<T>();
        }

        // Convert to requested type
        return Result<T>(config);

    } catch (const std::exception& e) {
        return make_error<T>(
            ErrorCode::CONVERSION_ERROR,
            std::string("Error getting config: ") + e.what(),
            "ConfigManager"
        );
    }
}

} // namespace trade_ngin