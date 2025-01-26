// src/core/config_manager.cpp
#include "trade_ngin/core/config_manager.hpp"
#include "trade_ngin/core/logger.hpp"
#include <fstream>
#include <sstream>

namespace trade_ngin {

// StrategyValidator Implementation
std::vector<ConfigValidationError> StrategyValidator::validate(
    const nlohmann::json& config) const {
    
    std::vector<ConfigValidationError> errors;

    // Validate trend following strategy configuration
    if (config.contains("trend_following")) {
        const auto& trend = config["trend_following"];

        // Validate risk target
        validate_numeric_range(trend["risk_target"], "risk_target", 0.0, 1.0, errors);

        // Validate IDM
        validate_numeric_range(trend["idm"], "idm", 0.1, 10.0, errors);

        // Validate EMA windows
        if (trend.contains("ema_windows")) {
            validate_ema_windows(trend["ema_windows"], errors);
        }

        // Validate lookback periods
        validate_numeric_range(trend["vol_lookback_short"], "vol_lookback_short", 
                             1, 504, errors);
        validate_numeric_range(trend["vol_lookback_long"], "vol_lookback_long", 
                             22, 2520, errors);
    }

    // Add validation for other strategy types here

    return errors;
}

bool StrategyValidator::validate_ema_windows(
    const nlohmann::json& windows,
    std::vector<ConfigValidationError>& errors) const {
    
    if (!windows.is_array()) {
        errors.push_back({"ema_windows", "Must be an array of window pairs"});
        return false;
    }

    for (const auto& window_pair : windows) {
        if (!window_pair.is_array() || window_pair.size() != 2) {
            errors.push_back({"ema_windows", "Each window must be a pair of integers"});
            return false;
        }

        int short_window = window_pair[0];
        int long_window = window_pair[1];

        if (short_window >= long_window) {
            errors.push_back({"ema_windows", 
                "Short window must be less than long window"});
            return false;
        }

        if (short_window < 1 || long_window > 512) {
            errors.push_back({"ema_windows", 
                "Windows must be between 1 and 512"});
            return false;
        }
    }

    return true;
}

bool StrategyValidator::validate_numeric_range(
    const nlohmann::json& value,
    const std::string& field,
    double min_val,
    double max_val,
    std::vector<ConfigValidationError>& errors) const {
    
    if (!value.is_number()) {
        errors.push_back({field, "Must be a number"});
        return false;
    }

    double val = value.get<double>();
    if (val < min_val || val > max_val) {
        std::stringstream ss;
        ss << "Must be between " << min_val << " and " << max_val;
        errors.push_back({field, ss.str()});
        return false;
    }

    return true;
}

// RiskValidator Implementation
std::vector<ConfigValidationError> RiskValidator::validate(
    const nlohmann::json& config) const {
    
    std::vector<ConfigValidationError> errors;
    
    // Validate risk limits
    validate_risk_limits(config, errors);

    // Validate confidence level
    if (config.contains("confidence_level")) {
        if (!config["confidence_level"].is_number() || 
            config["confidence_level"].get<double>() <= 0.9 ||
            config["confidence_level"].get<double>() >= 1.0) {
            errors.push_back({"confidence_level", 
                "Must be between 0.9 and 1.0"});
        }
    }

    // Validate lookback periods
    if (config.contains("var_lookback")) {
        if (!config["var_lookback"].is_number() ||
            config["var_lookback"].get<int>() < 22 ||
            config["var_lookback"].get<int>() > 2520) {
            errors.push_back({"var_lookback", 
                "Must be between 22 and 2520 days"});
        }
    }

    return errors;
}

bool RiskValidator::validate_risk_limits(
    const nlohmann::json& config,
    std::vector<ConfigValidationError>& errors) const {
    
    std::vector<std::pair<std::string, std::pair<double, double>>> limits = {
        {"portfolio_var_limit", {0.0, 0.5}},
        {"max_drawdown", {0.0, 0.5}},
        {"max_correlation", {0.0, 1.0}},
        {"max_gross_leverage", {1.0, 20.0}},
        {"max_net_leverage", {1.0, 10.0}}
    };

    for (const auto& [field, range] : limits) {
        if (config.contains(field)) {
            if (!config[field].is_number() ||
                config[field].get<double>() < range.first ||
                config[field].get<double>() > range.second) {
                std::stringstream ss;
                ss << "Must be between " << range.first << " and " << range.second;
                errors.push_back({field, ss.str()});
            }
        }
    }

    return errors.empty();
}

// ExecutionValidator Implementation
std::vector<ConfigValidationError> ExecutionValidator::validate(
    const nlohmann::json& config) const {
    
    std::vector<ConfigValidationError> errors;

    // Validate slippage model
    if (config.contains("slippage_model")) {
        validate_slippage_model(config["slippage_model"], errors);
    }

    // Validate commission model
    if (config.contains("commission_model")) {
        validate_commission_model(config["commission_model"], errors);
    }

    return errors;
}

bool ExecutionValidator::validate_slippage_model(
    const nlohmann::json& model,
    std::vector<ConfigValidationError>& errors) const {
    
    if (!model.contains("type")) {
        errors.push_back({"slippage_model", "Must specify type"});
        return false;
    }

    std::string type = model["type"];
    if (type == "volume_based") {
        if (!model.contains("price_impact_coefficient") ||
            !model["price_impact_coefficient"].is_number() ||
            model["price_impact_coefficient"].get<double>() <= 0.0) {
            errors.push_back({"price_impact_coefficient", 
                "Must be a positive number"});
        }

        if (!model.contains("min_volume_ratio") ||
            !model["min_volume_ratio"].is_number() ||
            model["min_volume_ratio"].get<double>() < 0.0 ||
            model["min_volume_ratio"].get<double>() > 1.0) {
            errors.push_back({"min_volume_ratio", 
                "Must be between 0 and 1"});
        }
    }

    return errors.empty();
}

bool ExecutionValidator::validate_commission_model(
    const nlohmann::json& model,
    std::vector<ConfigValidationError>& errors) const {
    
    if (!model.contains("base_rate") ||
        !model["base_rate"].is_number() ||
        model["base_rate"].get<double>() < 0.0) {
        errors.push_back({"base_rate", "Must be a non-negative number"});
    }

    if (model.contains("min_commission")) {
        if (!model["min_commission"].is_number() ||
            model["min_commission"].get<double>() < 0.0) {
            errors.push_back({"min_commission", "Must be a non-negative number"});
        }
    }

    if (model.contains("clearing_fee")) {
        if (!model["clearing_fee"].is_number() ||
            model["clearing_fee"].get<double>() < 0.0) {
            errors.push_back({"clearing_fee", "Must be a non-negative number"});
        }
    }

    return errors.empty();
}

// DatabaseValidator Implementation
std::vector<ConfigValidationError> DatabaseValidator::validate(
    const nlohmann::json& config) const {
    
    std::vector<ConfigValidationError> errors;

    // Required fields
    std::vector<std::string> required = {"host", "port", "database", "user"};
    for (const auto& field : required) {
        if (!config.contains(field)) {
            errors.push_back({field, "Required field missing"});
            continue;
        }

        if (field == "port") {
            if (!config[field].is_number() ||
                config[field].get<int>() <= 0 ||
                config[field].get<int>() > 65535) {
                errors.push_back({field, "Must be a valid port number (1-65535)"});
            }
        } else {
            if (!config[field].is_string() || config[field].get<std::string>().empty()) {
                errors.push_back({field, "Must be a non-empty string"});
            }
        }
    }

    // Cache settings
    if (config.contains("cache_size")) {
        if (!config["cache_size"].is_number() ||
            config["cache_size"].get<int>() <= 0) {
            errors.push_back({"cache_size", "Must be a positive integer"});
        }
    }

    if (config.contains("prefetch_days")) {
        if (!config["prefetch_days"].is_number() ||
            config["prefetch_days"].get<int>() <= 0 ||
            config["prefetch_days"].get<int>() > 30) {
            errors.push_back({"prefetch_days", "Must be between 1 and 30"});
        }
    }

    return errors;
}

// ConfigManager Implementation
ConfigManager::ConfigManager()
    : current_env_(Environment::DEVELOPMENT) {
    initialize_validators();
}

void ConfigManager::initialize_validators() {
    validators_[ConfigType::STRATEGY] = std::make_unique<StrategyValidator>();
    validators_[ConfigType::RISK] = std::make_unique<RiskValidator>();
    validators_[ConfigType::EXECUTION] = std::make_unique<ExecutionValidator>();
    validators_[ConfigType::DATABASE] = std::make_unique<DatabaseValidator>();
}

Result<void> ConfigManager::initialize(
    const std::filesystem::path& base_path,
    Environment env) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        config_path_ = base_path;
        current_env_ = env;

        // Load base configuration
        auto load_result = load_config_files();
        if (load_result.is_error()) {
            return load_result;
        }

        // Apply environment-specific overrides
        auto override_result = apply_environment_overrides();
        if (override_result.is_error()) {
            return override_result;
        }

        // Validate all configurations
        for (const auto& [type, validator] : validators_) {
            std::string component = get_component_name(type);
            if (config_.contains(component)) {
                auto validation = validate_config(type, config_[component]);
                if (validation.is_error()) {
                    return validation;
                }
            }
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::NOT_INITIALIZED,
            std::string("Failed to initialize config: ") + e.what(),
            "ConfigManager"
        );
    }
}

Result<void> ConfigManager::load_config_files() {
    try {
        // Load base config
        std::filesystem::path base_config = config_path_ / "base.json";
        if (!std::filesystem::exists(base_config)) {
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "Base config file not found: " + base_config.string(),
                "ConfigManager"
            );
        }

        std::ifstream base_file(base_config);
        config_ = nlohmann::json::parse(base_file);

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            std::string("Error loading config files: ") + e.what(),
            "ConfigManager"
        );
    }
}

Result<void> ConfigManager::apply_environment_overrides() {
    try {
        // Skip for development environment (uses base config)
        if (current_env_ == Environment::DEVELOPMENT) {
            return Result<void>();
        }

        // Load environment-specific config
        std::string env_name = environment_to_string(current_env_);
        std::filesystem::path env_config = config_path_ / (env_name + ".json");

        if (std::filesystem::exists(env_config)) {
            std::ifstream env_file(env_config);
            nlohmann::json env_overrides = nlohmann::json::parse(env_file);

            // Apply overrides recursively
            for (auto& [key, value] : env_overrides.items()) {
                if (config_.contains(key)) {
                    if (value.is_object()) {
                        config_[key].merge_patch(value);
                    } else {
                        config_[key] = value;
                    }
                }
            }
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error applying environment overrides: ") + e.what(),
            "ConfigManager"
        );
    }
}

Result<void> ConfigManager::update_config(
    ConfigType component_type,
    const nlohmann::json& config) {
    
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        // Validate new config
        auto validation = validate_config(component_type, config);
        if (validation.is_error()) {
            return validation;
        }

        // Update configuration
        std::string component = get_component_name(component_type);
        config_[component] = config;

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error updating config: ") + e.what(),
            "ConfigManager"
        );
    }
}

Result<void> ConfigManager::validate_config(
    ConfigType component_type,
    const nlohmann::json& config) const {
    
    auto it = validators_.find(component_type);
    if (it == validators_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "No validator found for component: " + 
            get_component_name(component_type),
            "ConfigManager"
        );
    }

    auto errors = it->second->validate(config);
    if (!errors.empty()) {
        std::stringstream ss;
        ss << "Configuration validation failed:\n";
        for (const auto& error : errors) {
            ss << "  " << error.field << ": " << error.message << "\n";
        }
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            ss.str(),
            "ConfigManager"
        );
    }

    return Result<void>();
}

std::string ConfigManager::get_component_name(ConfigType type) {
    switch (type) {
        case ConfigType::STRATEGY:   return "strategy";
        case ConfigType::RISK:       return "risk";
        case ConfigType::EXECUTION:  return "execution";
        case ConfigType::DATABASE:   return "data";
        case ConfigType::LOGGING:    return "logging";
        default:
            throw std::runtime_error("Unknown component type");
    }
}

std::string ConfigManager::environment_to_string(Environment env) {
    switch (env) {
        case Environment::DEVELOPMENT: return "development";
        case Environment::STAGING:     return "staging";
        case Environment::PRODUCTION:  return "production";
        case Environment::BACKTEST:    return "backtest";
        default:
            throw std::runtime_error("Unknown environment");
    }
}

Environment ConfigManager::string_to_environment(const std::string& env_str) {
    if (env_str == "development") return Environment::DEVELOPMENT;
    if (env_str == "staging")     return Environment::STAGING;
    if (env_str == "production")  return Environment::PRODUCTION;
    if (env_str == "backtest")    return Environment::BACKTEST;
    throw std::runtime_error("Invalid environment string: " + env_str);
}

} // namespace trade_ngin