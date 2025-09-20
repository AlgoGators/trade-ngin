// src/core/config_manager.cpp
#include "trade_ngin/core/config_manager.hpp"
#include <fstream>
#include <sstream>
#include "trade_ngin/core/config_version.hpp"
#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

// StrategyValidator Implementation
std::vector<ConfigValidationError> StrategyValidator::validate(const nlohmann::json& config) const {
    std::vector<ConfigValidationError> errors;

    // Check required fields
    if (!config.contains("capital_allocation") || !config["capital_allocation"].is_number()) {
        errors.push_back({"capital_allocation", "Must be a number"});
    } else if (config["capital_allocation"].get<double>() <= 0.0) {
        errors.push_back({"capital_allocation", "Must be positive"});
    }

    if (!config.contains("max_leverage") || !config["max_leverage"].is_number()) {
        errors.push_back({"max_leverage", "Must be a number"});
    } else if (config["max_leverage"].get<double>() <= 0.0) {
        errors.push_back({"max_leverage", "Must be positive"});
    }

    // Validate position limits if present
    if (config.contains("position_limits") && !config["position_limits"].is_object()) {
        errors.push_back({"position_limits", "Must be an object mapping symbols to limits"});
    }

    // Validate risk parameters
    if (config.contains("max_drawdown")) {
        if (!config["max_drawdown"].is_number()) {
            errors.push_back({"max_drawdown", "Must be a number"});
        } else if (config["max_drawdown"].get<double>() <= 0.0 ||
                   config["max_drawdown"].get<double>() > 1.0) {
            errors.push_back({"max_drawdown", "Must be between 0 and 1"});
        }
    }

    // Validate EMA windows if present
    if (config.contains("ema_windows")) {
        validate_ema_windows(config["ema_windows"], errors);
    }

    return errors;
}

bool StrategyValidator::validate_ema_windows(const nlohmann::json& windows,
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
            errors.push_back({"ema_windows", "Short window must be less than long window"});
            return false;
        }

        if (short_window < 1 || long_window > 512) {
            errors.push_back({"ema_windows", "Windows must be between 1 and 512"});
            return false;
        }
    }

    return true;
}

bool StrategyValidator::validate_numeric_range(const nlohmann::json& value,
                                               const std::string& field, double min_val,
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
std::vector<ConfigValidationError> RiskValidator::validate(const nlohmann::json& config) const {
    std::vector<ConfigValidationError> errors;

    // Validate risk limits
    validate_risk_limits(config, errors);

    // Validate confidence level
    if (!config.contains("confidence_level") || !config["confidence_level"].is_number()) {
        errors.push_back({"confidence_level", "Must be a number"});
    } else {
        double confidence = config["confidence_level"].get<double>();
        if (confidence <= 0.0 || confidence >= 1.0) {
            errors.push_back({"confidence_level", "Must be between 0 and 1"});
        }
    }

    // Validate lookback period
    if (!config.contains("lookback_period") || !config["lookback_period"].is_number()) {
        errors.push_back({"lookback_period", "Must be a number"});
    } else {
        int lookback = config["lookback_period"].get<int>();
        if (lookback <= 0) {
            errors.push_back({"lookback_period", "Must be positive"});
        }
    }

    // Validate capital
    if (!config.contains("capital") || !config["capital"].is_number()) {
        errors.push_back({"capital", "Must be a number"});
    } else {
        double capital = config["capital"].get<double>();
        if (capital <= 0.0) {
            errors.push_back({"capital", "Must be positive"});
        }
    }

    return errors;
}

bool RiskValidator::validate_risk_limits(const nlohmann::json& config,
                                         std::vector<ConfigValidationError>& errors) const {
    std::vector<std::pair<std::string, std::pair<double, double>>> limits = {
        {"portfolio_var_limit", {0.0, 0.5}},
        {"max_drawdown", {0.0, 0.5}},
        {"max_correlation", {0.0, 1.0}},
        {"max_gross_leverage", {1.0, 20.0}},
        {"max_net_leverage", {1.0, 10.0}}};

    for (const auto& [field, range] : limits) {
        if (config.contains(field)) {
            if (!config[field].is_number() || config[field].get<double>() < range.first ||
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

bool ExecutionValidator::validate_slippage_model(const nlohmann::json& model,
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
            errors.push_back({"price_impact_coefficient", "Must be a positive number"});
        }

        if (!model.contains("min_volume_ratio") || !model["min_volume_ratio"].is_number() ||
            model["min_volume_ratio"].get<double>() < 0.0 ||
            model["min_volume_ratio"].get<double>() > 1.0) {
            errors.push_back({"min_volume_ratio", "Must be between 0 and 1"});
        }
    }

    return errors.empty();
}

bool ExecutionValidator::validate_commission_model(
    const nlohmann::json& model, std::vector<ConfigValidationError>& errors) const {
    if (!model.contains("base_rate") || !model["base_rate"].is_number() ||
        model["base_rate"].get<double>() < 0.0) {
        errors.push_back({"base_rate", "Must be a non-negative number"});
    }

    if (model.contains("min_commission")) {
        if (!model["min_commission"].is_number() || model["min_commission"].get<double>() < 0.0) {
            errors.push_back({"min_commission", "Must be a non-negative number"});
        }
    }

    if (model.contains("clearing_fee")) {
        if (!model["clearing_fee"].is_number() || model["clearing_fee"].get<double>() < 0.0) {
            errors.push_back({"clearing_fee", "Must be a non-negative number"});
        }
    }

    return errors.empty();
}

// DatabaseValidator Implementation
std::vector<ConfigValidationError> DatabaseValidator::validate(const nlohmann::json& config) const {
    std::vector<ConfigValidationError> errors;

    // Required fields
    std::vector<std::string> required = {"host", "port", "database", "user"};
    for (const auto& field : required) {
        if (!config.contains(field)) {
            errors.push_back({field, "Required field missing"});
            continue;
        }

        if (field == "port") {
            if (!config[field].is_number() || config[field].get<int>() <= 0 ||
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
        if (!config["cache_size"].is_number() || config["cache_size"].get<int>() <= 0) {
            errors.push_back({"cache_size", "Must be a positive integer"});
        }
    }

    if (config.contains("prefetch_days")) {
        if (!config["prefetch_days"].is_number() || config["prefetch_days"].get<int>() <= 0 ||
            config["prefetch_days"].get<int>() > 30) {
            errors.push_back({"prefetch_days", "Must be between 1 and 30"});
        }
    }

    return errors;
}

void ConfigManager::initialize_validators() {
    validators_[ConfigType::STRATEGY] = std::make_unique<StrategyValidator>();
    validators_[ConfigType::RISK] = std::make_unique<RiskValidator>();
    validators_[ConfigType::EXECUTION] = std::make_unique<ExecutionValidator>();
    validators_[ConfigType::DATABASE] = std::make_unique<DatabaseValidator>();
}

Result<void> ConfigManager::initialize(const std::filesystem::path& base_path, Environment env) {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        config_path_ = base_path;
        current_env_ = env;

        // Initialize validators
        initialize_validators();

        // Load base configuration
        auto result = load_config_files();
        if (result.is_error()) {
            return result;
        }

        return apply_environment_overrides();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::NOT_INITIALIZED,
                                std::string("Failed to initialize config: ") + e.what(),
                                "ConfigManager");
    }
}

Result<void> ConfigManager::load_config_files() {
    try {
        // Check if directory exists
        if (!std::filesystem::exists(config_path_)) {
            // Create directory and default configs
            std::filesystem::create_directories(config_path_);

            // Create default configs
            for (int i = static_cast<int>(ConfigType::STRATEGY);
                 i <= static_cast<int>(ConfigType::LOGGING); ++i) {
                auto type = static_cast<ConfigType>(i);
                std::string component = get_component_name(type);
                config_[component] = create_default_config(type);
            }

            // Save default configs
            return save_configs();
        }

        // Load each config file
        for (int i = static_cast<int>(ConfigType::STRATEGY);
             i <= static_cast<int>(ConfigType::LOGGING); ++i) {
            auto type = static_cast<ConfigType>(i);
            std::string component = get_component_name(type);
            std::filesystem::path config_file = config_path_ / (component + ".json");

            if (std::filesystem::exists(config_file)) {
                std::ifstream file(config_file);
                if (!file.is_open()) {
                    return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                            "Failed to open config file: " + config_file.string(),
                                            "ConfigManager");
                }

                try {
                    file >> config_[component];
                } catch (const nlohmann::json::exception& e) {
                    return make_error<void>(
                        ErrorCode::INVALID_ARGUMENT,
                        "Invalid JSON in config file: " + config_file.string() + " - " + e.what(),
                        "ConfigManager");
                }

                // Migrate config if needed
                auto migration_result =
                    ConfigVersionManager::instance().auto_migrate(config_[component], type);

                if (migration_result.is_error()) {
                    return make_error<void>(
                        migration_result.error()->code(),
                        "Config migration failed: " + std::string(migration_result.error()->what()),
                        "ConfigManager");
                }

                // Validate loaded config
                auto validation = validate_config(type, config_[component]);
                if (validation.is_error()) {
                    return validation;
                }
            } else {
                // Create default config
                config_[component] = create_default_config(type);

                // Save default config
                std::ofstream file(config_file);
                if (!file.is_open()) {
                    return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                            "Failed to create config file: " + config_file.string(),
                                            "ConfigManager");
                }

                file << std::setw(4) << config_[component] << std::endl;
            }
        }

        // Load credentials separately
        std::filesystem::path credentials_file = get_credentials_path();
        if (std::filesystem::exists(credentials_file)) {
            std::ifstream file(credentials_file);
            if (file.is_open()) {
                try {
                    file >> config_["credentials"];
                } catch (const nlohmann::json::exception& e) {
                    return make_error<void>(
                        ErrorCode::INVALID_ARGUMENT,
                        "Invalid JSON in credentials file - " + std::string(e.what()),
                        "ConfigManager");
                }
            }
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error loading config files: ") + e.what(),
                                "ConfigManager");
    }
}

Result<void> ConfigManager::apply_environment_overrides() {
    try {
        std::string env_str = environment_to_string(current_env_);
        std::filesystem::path env_dir = config_path_ / env_str;

        if (!std::filesystem::exists(env_dir)) {
            return Result<void>();  // No overrides, not an error
        }

        // Load each override file
        for (const auto& entry : std::filesystem::directory_iterator(env_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                std::string component = entry.path().stem().string();

                std::ifstream file(entry.path());
                if (!file.is_open()) {
                    return make_error<void>(
                        ErrorCode::INVALID_ARGUMENT,
                        "Failed to open override file: " + entry.path().string(), "ConfigManager");
                }

                nlohmann::json override_config;
                try {
                    file >> override_config;
                } catch (const nlohmann::json::exception& e) {
                    return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                            "Invalid JSON in override file: " +
                                                entry.path().string() + " - " + e.what(),
                                            "ConfigManager");
                }

                // Apply override
                if (config_.contains(component)) {
                    config_[component].merge_patch(override_config);
                } else {
                    config_[component] = override_config;
                }
            }
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error applying environment overrides: ") + e.what(),
                                "ConfigManager");
    }
}

Result<void> ConfigManager::update_config(ConfigType component_type, const nlohmann::json& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        std::string component = get_component_name(component_type);

        // Validate new configuration
        auto validation = validate_config(component_type, config);
        if (validation.is_error()) {
            return validation;
        }

        // Apply update
        config_[component] = config;

        // Save updated config to file
        std::filesystem::path config_file = config_path_ / (component + ".json");
        std::ofstream file(config_file);
        if (!file.is_open()) {
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "Failed to open config file for writing: " + config_file.string(), "ConfigManager");
        }
        file << std::setw(4) << config << std::endl;
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error updating config: ") + e.what(), "ConfigManager");
    }
}

Result<void> ConfigManager::validate_config(ConfigType component_type,
                                            const nlohmann::json& config) const {
    auto it = validators_.find(component_type);
    if (it == validators_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "No validator found for component: " + get_component_name(component_type),
            "ConfigManager");
    }

    auto errors = it->second->validate(config);
    if (!errors.empty()) {
        std::stringstream ss;
        ss << "Configuration validation failed for " << get_component_name(component_type) << ":";

        for (const auto& error : errors) {
            ss << "\n - " << error.field << ": " << error.message;
        }

        return make_error<void>(ErrorCode::INVALID_ARGUMENT, ss.str(), "ConfigManager");
    }

    return Result<void>();
}

Result<void> ConfigManager::save_configs() {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        for (const auto& item : config_.items()) {
            const auto& component = item.key();
            const auto& config = item.value();
            std::filesystem::path config_file = config_path_ / (component + ".json");
            std::ofstream file(config_file);
            if (!file.is_open()) {
                return make_error<void>(
                    ErrorCode::INVALID_ARGUMENT,
                    "Failed to open config file for writing: " + config_file.string(),
                    "ConfigManager");
            }

            file << std::setw(4) << config << std::endl;
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error saving configs: ") + e.what(), "ConfigManager");
    }
}

nlohmann::json ConfigManager::create_default_config(ConfigType component_type) const {
    switch (component_type) {
        case ConfigType::STRATEGY:
            return create_default_strategy_config();
        case ConfigType::RISK:
            return create_default_risk_config();
        case ConfigType::EXECUTION:
            return create_default_execution_config();
        case ConfigType::DATABASE:
            return create_default_database_config();
        case ConfigType::LOGGING:
            return create_default_logging_config();
        default:
            return nlohmann::json::object();
    }
}

nlohmann::json ConfigManager::create_default_strategy_config() const {
    nlohmann::json config;
    config["capital_allocation"] = 1000000.0;  // $1M
    config["max_leverage"] = 3.0;              // 3x leverage
    config["max_drawdown"] = 0.3;              // 30% max drawdown
    config["var_limit"] = 0.1;                 // 10% VaR limit
    config["correlation_limit"] = 0.7;         // 70% correlation limit
    config["save_executions"] = true;
    config["save_signals"] = true;
    config["save_positions"] = true;
    config["signals_table"] = "trading.signals";
    config["positions_table"] = "trading.positions";
    config["version"] = "1.0.0";

    return config;
}

nlohmann::json ConfigManager::create_default_risk_config() const {
    nlohmann::json config;
    config["var_limit"] = 0.15;          // 15% VaR limit
    config["jump_risk_limit"] = 0.10;    // 10% jump risk limit
    config["max_correlation"] = 0.7;     // 70% correlation limit
    config["max_gross_leverage"] = 4.0;  // 4x gross leverage
    config["max_net_leverage"] = 2.0;    // 2x net leverage
    config["confidence_level"] = 0.99;   // 99% confidence
    config["lookback_period"] = 252;     // 1 year lookback
    config["capital"] = 1000000.0;       // $1M capital
    config["version"] = "1.0.0";

    return config;
}

nlohmann::json ConfigManager::create_default_execution_config() const {
    nlohmann::json config;
    config["max_orders_per_second"] = 100;
    config["max_pending_orders"] = 1000;
    config["max_order_size"] = 100000.0;
    config["max_notional_value"] = 1000000.0;
    config["retry_attempts"] = 3;
    config["retry_delay_ms"] = 100.0;
    config["simulate_fills"] = false;
    config["version"] = "1.0.0";

    return config;
}

nlohmann::json ConfigManager::create_default_database_config() const {
    nlohmann::json config;
    config["connection_string"] = "postgresql://localhost:5432/tradingdb";
    config["max_connections"] = 10;
    config["timeout_seconds"] = 30;
    config["version"] = "1.0.0";

    return config;
}

nlohmann::json ConfigManager::create_default_logging_config() const {
    nlohmann::json config;
    config["min_level"] = "INFO";
    config["destination"] = "CONSOLE";
    config["log_directory"] = "logs";
    config["filename_prefix"] = "trade_ngin";
    config["include_timestamp"] = true;
    config["include_level"] = true;
    config["max_file_size"] = 52428800;  // 50 MB
    config["max_files"] = 10;
    config["version"] = "1.0.0";

    return config;
}

std::string ConfigManager::get_component_name(ConfigType type) {
    switch (type) {
        case ConfigType::STRATEGY:
            return "strategy";
        case ConfigType::RISK:
            return "risk";
        case ConfigType::EXECUTION:
            return "execution";
        case ConfigType::DATABASE:
            return "data";
        case ConfigType::LOGGING:
            return "logging";
        default:
            return "unknown";
    }
}

std::string ConfigManager::environment_to_string(Environment env) {
    switch (env) {
        case Environment::DEVELOPMENT:
            return "development";
        case Environment::STAGING:
            return "staging";
        case Environment::PRODUCTION:
            return "production";
        case Environment::BACKTEST:
            return "backtest";
        default:
            return "unknown";
    }
}

Environment ConfigManager::string_to_environment(const std::string& env_str) {
    if (env_str == "development")
        return Environment::DEVELOPMENT;
    if (env_str == "staging")
        return Environment::STAGING;
    if (env_str == "production")
        return Environment::PRODUCTION;
    if (env_str == "backtest")
        return Environment::BACKTEST;
    return Environment::DEVELOPMENT;
}

}  // namespace trade_ngin
