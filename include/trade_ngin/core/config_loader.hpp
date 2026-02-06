// include/trade_ngin/core/config_loader.hpp

#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include "trade_ngin/risk/risk_manager.hpp"

namespace trade_ngin {

/**
 * @brief Email configuration
 */
struct EmailConfig {
    std::string smtp_host{"smtp.gmail.com"};
    int smtp_port{587};
    std::string username;
    std::string password;
    std::string from_email;
    bool use_tls{true};
    std::vector<std::string> to_emails;
    std::vector<std::string> to_emails_production;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["smtp_host"] = smtp_host;
        j["smtp_port"] = smtp_port;
        j["username"] = username;
        j["password"] = password;
        j["from_email"] = from_email;
        j["use_tls"] = use_tls;
        j["to_emails"] = to_emails;
        j["to_emails_production"] = to_emails_production;
        return j;
    }

    void from_json(const nlohmann::json& j) {
        if (j.contains("smtp_host"))
            smtp_host = j.at("smtp_host").get<std::string>();
        if (j.contains("smtp_port"))
            smtp_port = j.at("smtp_port").get<int>();
        if (j.contains("username"))
            username = j.at("username").get<std::string>();
        if (j.contains("password"))
            password = j.at("password").get<std::string>();
        if (j.contains("from_email"))
            from_email = j.at("from_email").get<std::string>();
        if (j.contains("use_tls"))
            use_tls = j.at("use_tls").get<bool>();
        if (j.contains("to_emails"))
            to_emails = j.at("to_emails").get<std::vector<std::string>>();
        if (j.contains("to_emails_production"))
            to_emails_production = j.at("to_emails_production").get<std::vector<std::string>>();
    }
};

/**
 * @brief Database configuration
 */
struct DatabaseConfig {
    std::string host;
    std::string port{"5432"};
    std::string username;
    std::string password;
    std::string name;
    size_t num_connections{5};

    std::string get_connection_string() const {
        return "postgresql://" + username + ":" + password + "@" + host + ":" + port + "/" + name;
    }

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["host"] = host;
        j["port"] = port;
        j["username"] = username;
        j["password"] = password;
        j["name"] = name;
        j["num_connections"] = num_connections;
        return j;
    }

    void from_json(const nlohmann::json& j) {
        if (j.contains("host"))
            host = j.at("host").get<std::string>();
        if (j.contains("port"))
            port = j.at("port").get<std::string>();
        if (j.contains("username"))
            username = j.at("username").get<std::string>();
        if (j.contains("password"))
            password = j.at("password").get<std::string>();
        if (j.contains("name"))
            name = j.at("name").get<std::string>();
        if (j.contains("num_connections"))
            num_connections = j.at("num_connections").get<size_t>();
    }
};

/**
 * @brief Execution configuration
 */
struct ExecutionConfig {
    double commission_rate{0.0005};
    double slippage_bps{1.0};
    double position_limit_backtest{1000.0};
    double position_limit_live{500.0};

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["commission_rate"] = commission_rate;
        j["slippage_bps"] = slippage_bps;
        j["position_limit_backtest"] = position_limit_backtest;
        j["position_limit_live"] = position_limit_live;
        return j;
    }

    void from_json(const nlohmann::json& j) {
        if (j.contains("commission_rate"))
            commission_rate = j.at("commission_rate").get<double>();
        if (j.contains("slippage_bps"))
            slippage_bps = j.at("slippage_bps").get<double>();
        if (j.contains("position_limit_backtest"))
            position_limit_backtest = j.at("position_limit_backtest").get<double>();
        if (j.contains("position_limit_live"))
            position_limit_live = j.at("position_limit_live").get<double>();
    }
};

/**
 * @brief Backtest-specific configuration
 */
struct BacktestSpecificConfig {
    int lookback_years{2};
    bool store_trade_details{true};

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["lookback_years"] = lookback_years;
        j["store_trade_details"] = store_trade_details;
        return j;
    }

    void from_json(const nlohmann::json& j) {
        if (j.contains("lookback_years"))
            lookback_years = j.at("lookback_years").get<int>();
        if (j.contains("store_trade_details"))
            store_trade_details = j.at("store_trade_details").get<bool>();
    }
};

/**
 * @brief Live-specific configuration
 */
struct LiveSpecificConfig {
    int historical_days{300};

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["historical_days"] = historical_days;
        return j;
    }

    void from_json(const nlohmann::json& j) {
        if (j.contains("historical_days"))
            historical_days = j.at("historical_days").get<int>();
    }
};

/**
 * @brief Strategy defaults configuration
 */
struct StrategyDefaultsConfig {
    std::vector<std::pair<int, double>> fdm{{1, 1.0},  {2, 1.03}, {3, 1.08},
                                             {4, 1.13}, {5, 1.19}, {6, 1.26}};
    double max_strategy_allocation{1.0};
    double min_strategy_allocation{0.1};
    bool use_optimization{true};
    bool use_risk_management{true};

    nlohmann::json to_json() const {
        nlohmann::json j;
        nlohmann::json fdm_array = nlohmann::json::array();
        for (const auto& [n, mult] : fdm) {
            fdm_array.push_back({n, mult});
        }
        j["fdm"] = fdm_array;
        j["max_strategy_allocation"] = max_strategy_allocation;
        j["min_strategy_allocation"] = min_strategy_allocation;
        j["use_optimization"] = use_optimization;
        j["use_risk_management"] = use_risk_management;
        return j;
    }

    void from_json(const nlohmann::json& j) {
        if (j.contains("fdm")) {
            fdm.clear();
            for (const auto& item : j.at("fdm")) {
                fdm.push_back({item[0].get<int>(), item[1].get<double>()});
            }
        }
        if (j.contains("max_strategy_allocation"))
            max_strategy_allocation = j.at("max_strategy_allocation").get<double>();
        if (j.contains("min_strategy_allocation"))
            min_strategy_allocation = j.at("min_strategy_allocation").get<double>();
        if (j.contains("use_optimization"))
            use_optimization = j.at("use_optimization").get<bool>();
        if (j.contains("use_risk_management"))
            use_risk_management = j.at("use_risk_management").get<bool>();
    }
};

/**
 * @brief Consolidated application configuration
 *
 * Contains all configuration values loaded from:
 * - config/defaults.json (shared defaults)
 * - config/portfolios/{name}/portfolio.json
 * - config/portfolios/{name}/risk.json
 * - config/portfolios/{name}/email.json
 */
struct AppConfig {
    // Portfolio identification
    std::string portfolio_id;

    // Capital settings
    double initial_capital{500000.0};
    double reserve_capital_pct{0.10};

    // Database configuration
    DatabaseConfig database;

    // Execution configuration
    ExecutionConfig execution;

    // Optimization configuration
    DynamicOptConfig opt_config;

    // Risk configuration
    RiskConfig risk_config;

    // Additional risk limits from portfolio
    double max_drawdown{0.4};
    double max_leverage{4.0};

    // Backtest settings
    BacktestSpecificConfig backtest;

    // Live settings
    LiveSpecificConfig live;

    // Strategy defaults
    StrategyDefaultsConfig strategy_defaults;

    // Email configuration
    EmailConfig email;

    // Raw strategies JSON for strategy factory
    nlohmann::json strategies_config;

    /**
     * @brief Convert config to JSON for serialization
     */
    nlohmann::json to_json() const {
        nlohmann::json j;
        j["portfolio_id"] = portfolio_id;
        j["initial_capital"] = initial_capital;
        j["reserve_capital_pct"] = reserve_capital_pct;
        j["database"] = database.to_json();
        j["execution"] = execution.to_json();
        j["optimization"] = opt_config.to_json();
        j["risk"] = risk_config.to_json();
        j["max_drawdown"] = max_drawdown;
        j["max_leverage"] = max_leverage;
        j["backtest"] = backtest.to_json();
        j["live"] = live.to_json();
        j["strategy_defaults"] = strategy_defaults.to_json();
        j["email"] = email.to_json();
        j["strategies"] = strategies_config;
        return j;
    }
};

/**
 * @brief Configuration loader for the new modular config system
 *
 * Loads configuration from:
 * 1. config/defaults.json (shared defaults)
 * 2. config/portfolios/{portfolio_name}/portfolio.json
 * 3. config/portfolios/{portfolio_name}/risk.json
 * 4. config/portfolios/{portfolio_name}/email.json
 *
 * Values in portfolio-specific files override defaults.
 */
class ConfigLoader {
public:
    /**
     * @brief Load configuration for a specific portfolio
     * @param config_base_path Base path to config directory (e.g., "./config")
     * @param portfolio_name Name of the portfolio (e.g., "base", "conservative")
     * @return Result containing AppConfig or error
     */
    static Result<AppConfig> load(const std::filesystem::path& config_base_path,
                                  const std::string& portfolio_name);

    /**
     * @brief Load configuration from legacy single-file format
     * @param config_file_path Path to legacy config file (e.g., "./config.json")
     * @return Result containing AppConfig or error
     *
     * This method provides backward compatibility with existing config files
     * during the migration period.
     */
    static Result<AppConfig> load_legacy(const std::filesystem::path& config_file_path);

private:
    /**
     * @brief Load and parse a JSON file
     * @param file_path Path to JSON file
     * @return Result containing parsed JSON or error
     */
    static Result<nlohmann::json> load_json_file(const std::filesystem::path& file_path);

    /**
     * @brief Recursively merge JSON objects
     * @param target Target JSON object (modified in place)
     * @param source Source JSON object to merge from
     *
     * For nested objects, performs deep merge. For other types, source overwrites target.
     */
    static void merge_json(nlohmann::json& target, const nlohmann::json& source);

    /**
     * @brief Extract AppConfig from merged JSON
     * @param merged Merged JSON object
     * @return Result containing extracted AppConfig or error
     */
    static Result<AppConfig> extract_config(const nlohmann::json& merged);

    /**
     * @brief Validate required fields after extraction
     * @param config Extracted AppConfig
     * @return Result indicating success or failure
     */
    static Result<void> validate_config(const AppConfig& config);

    /**
     * @brief Log a brief configuration summary
     * @param config Extracted AppConfig
     */
    static void log_config_summary(const AppConfig& config);
};

}  // namespace trade_ngin
