// src/core/config_loader.cpp

#include "trade_ngin/core/config_loader.hpp"

#include <fstream>
#include <iostream>

#include <pqxx/pqxx>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/postgres_database.hpp"

namespace trade_ngin {

Result<nlohmann::json> ConfigLoader::load_json_file(const std::filesystem::path& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        return make_error<nlohmann::json>(ErrorCode::FILE_NOT_FOUND,
                                          "Failed to open config file: " + file_path.string(),
                                          "ConfigLoader");
    }

    try {
        nlohmann::json j;
        file >> j;
        return j;
    } catch (const nlohmann::json::parse_error& e) {
        return make_error<nlohmann::json>(
            ErrorCode::JSON_PARSE_ERROR,
            "Failed to parse JSON file " + file_path.string() + ": " + e.what(), "ConfigLoader");
    } catch (const std::exception& e) {
        return make_error<nlohmann::json>(ErrorCode::FILE_IO_ERROR,
                                          "Error reading config file " + file_path.string() + ": " +
                                              e.what(),
                                          "ConfigLoader");
    }
}

void ConfigLoader::merge_json(nlohmann::json& target, const nlohmann::json& source) {
    for (auto it = source.begin(); it != source.end(); ++it) {
        const auto& key = it.key();
        const auto& value = it.value();

        if (target.contains(key) && target[key].is_object() && value.is_object()) {
            // Recursive merge for nested objects
            merge_json(target[key], value);
        } else {
            // Override or add
            target[key] = value;
        }
    }
}

Result<AppConfig> ConfigLoader::extract_config(const nlohmann::json& merged) {
    try {
        AppConfig config;

        // Portfolio identification
        if (merged.contains("portfolio_id")) {
            config.portfolio_id = merged.at("portfolio_id").get<std::string>();
        }

        // Capital settings
        if (merged.contains("initial_capital")) {
            config.initial_capital = merged.at("initial_capital").get<double>();
        }
        if (merged.contains("reserve_capital_pct")) {
            config.reserve_capital_pct = merged.at("reserve_capital_pct").get<double>();
        }

        // Database configuration
        if (merged.contains("database")) {
            config.database.from_json(merged.at("database"));
        }

        // Execution configuration
        if (merged.contains("execution")) {
            config.execution.from_json(merged.at("execution"));
        }

        // Optimization configuration
        if (merged.contains("optimization")) {
            config.opt_config.from_json(merged.at("optimization"));
        }
        // Set capital in opt_config
        config.opt_config.capital = config.initial_capital;

        // Risk configuration - from risk_defaults and risk section
        if (merged.contains("risk_defaults")) {
            const auto& risk_defaults = merged.at("risk_defaults");
            if (risk_defaults.contains("confidence_level")) {
                config.risk_config.confidence_level =
                    risk_defaults.at("confidence_level").get<double>();
            }
            if (risk_defaults.contains("lookback_period")) {
                config.risk_config.lookback_period =
                    risk_defaults.at("lookback_period").get<int>();
            }
            if (risk_defaults.contains("max_correlation")) {
                config.risk_config.max_correlation =
                    risk_defaults.at("max_correlation").get<double>();
            }
        }

        if (merged.contains("risk")) {
            config.risk_config.from_json(merged.at("risk"));

            // Additional risk limits
            const auto& risk = merged.at("risk");
            if (risk.contains("max_drawdown")) {
                config.max_drawdown = risk.at("max_drawdown").get<double>();
            }
            if (risk.contains("max_leverage")) {
                config.max_leverage = risk.at("max_leverage").get<double>();
            }
        }
        // Set capital in risk_config
        config.risk_config.capital = Decimal(config.initial_capital);

        // Backtest settings
        if (merged.contains("backtest")) {
            config.backtest.from_json(merged.at("backtest"));
        }

        // Live settings
        if (merged.contains("live")) {
            config.live.from_json(merged.at("live"));
        }

        // Strategy defaults
        if (merged.contains("strategy_defaults")) {
            config.strategy_defaults.from_json(merged.at("strategy_defaults"));
        }

        // Email configuration
        if (merged.contains("email")) {
            config.email.from_json(merged.at("email"));
        }

        // Strategies (raw JSON for factory)
        if (merged.contains("strategies")) {
            config.strategies_config = merged.at("strategies");
        }

        return config;

    } catch (const std::exception& e) {
        return make_error<AppConfig>(ErrorCode::INVALID_DATA,
                                     "Failed to extract config: " + std::string(e.what()),
                                     "ConfigLoader");
    }
}

Result<void> ConfigLoader::validate_config(const AppConfig& config) {
    if (config.portfolio_id.empty()) {
        return make_error<void>(ErrorCode::INVALID_DATA, "Missing portfolio_id", "ConfigLoader");
    }
    if (config.database.host.empty() || config.database.username.empty() ||
        config.database.password.empty() || config.database.name.empty()) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "Missing required database configuration fields",
                                "ConfigLoader");
    }
    if (config.initial_capital <= 0.0) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "initial_capital must be positive",
                                "ConfigLoader");
    }
    if (config.reserve_capital_pct < 0.0 || config.reserve_capital_pct >= 1.0) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "reserve_capital_pct must be in [0.0, 1.0)",
                                "ConfigLoader");
    }
    if (config.strategies_config.is_null() || !config.strategies_config.is_object() ||
        config.strategies_config.empty()) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "strategies configuration is missing or empty",
                                "ConfigLoader");
    }
    return Result<void>();
}

void ConfigLoader::log_config_summary(const AppConfig& config) {
    auto& logger = Logger::instance();
    if (!logger.is_initialized()) {
        return;
    }
    INFO("Config summary: portfolio_id=" + config.portfolio_id +
         ", initial_capital=" + std::to_string(config.initial_capital) +
         ", reserve_pct=" + std::to_string(config.reserve_capital_pct));
    INFO("Config summary: db=" + config.database.host + ":" + config.database.port +
         "/" + config.database.name +
         ", connections=" + std::to_string(config.database.num_connections));
    INFO("Config summary: strategies=" + std::to_string(config.strategies_config.size()) +
         ", backtest_lookback_years=" + std::to_string(config.backtest.lookback_years) +
         ", live_historical_days=" + std::to_string(config.live.historical_days));
}

Result<AppConfig> ConfigLoader::load(const std::filesystem::path& config_base_path,
                                     const std::string& portfolio_name) {
    // 1. Load defaults.json
    auto defaults_path = config_base_path / "defaults.json";
    auto defaults_result = load_json_file(defaults_path);
    if (defaults_result.is_error()) {
        return make_error<AppConfig>(defaults_result.error()->code(),
                                     "Failed to load defaults.json: " +
                                         std::string(defaults_result.error()->what()),
                                     "ConfigLoader");
    }
    nlohmann::json merged = defaults_result.value();

    // 2. Load portfolio-specific configs
    auto portfolio_path = config_base_path / "portfolios" / portfolio_name;

    // Load portfolio.json
    auto portfolio_json_path = portfolio_path / "portfolio.json";
    auto portfolio_result = load_json_file(portfolio_json_path);
    if (portfolio_result.is_error()) {
        return make_error<AppConfig>(portfolio_result.error()->code(),
                                     "Failed to load portfolio.json: " +
                                         std::string(portfolio_result.error()->what()),
                                     "ConfigLoader");
    }
    merge_json(merged, portfolio_result.value());

    // Load risk.json
    auto risk_json_path = portfolio_path / "risk.json";
    auto risk_result = load_json_file(risk_json_path);
    if (risk_result.is_error()) {
        return make_error<AppConfig>(risk_result.error()->code(),
                                     "Failed to load risk.json: " +
                                         std::string(risk_result.error()->what()),
                                     "ConfigLoader");
    }
    merged["risk"] = risk_result.value();

    // Load email.json
    auto email_json_path = portfolio_path / "email.json";
    auto email_result = load_json_file(email_json_path);
    if (email_result.is_error()) {
        return make_error<AppConfig>(email_result.error()->code(),
                                     "Failed to load email.json: " +
                                         std::string(email_result.error()->what()),
                                     "ConfigLoader");
    }
    merged["email"] = email_result.value();

    // 3. Extract config
    auto config_result = extract_config(merged);
    if (config_result.is_error()) {
        return config_result;
    }

    auto validation_result = validate_config(config_result.value());
    if (validation_result.is_error()) {
        return make_error<AppConfig>(validation_result.error()->code(),
                                     validation_result.error()->what(),
                                     "ConfigLoader");
    }

    log_config_summary(config_result.value());
    return config_result;
}

Result<AppConfig> ConfigLoader::load_legacy(const std::filesystem::path& config_file_path) {
    // Load the single config file
    auto json_result = load_json_file(config_file_path);
    if (json_result.is_error()) {
        return make_error<AppConfig>(json_result.error()->code(),
                                     "Failed to load legacy config: " +
                                         std::string(json_result.error()->what()),
                                     "ConfigLoader");
    }

    const auto& config_json = json_result.value();

    try {
        AppConfig config;

        // Portfolio ID
        if (config_json.contains("portfolio_id")) {
            config.portfolio_id = config_json.at("portfolio_id").get<std::string>();
        }

        // Database configuration
        if (config_json.contains("database")) {
            config.database.from_json(config_json.at("database"));
        }

        // Email configuration
        if (config_json.contains("email")) {
            config.email.from_json(config_json.at("email"));
        }

        // Strategies (from portfolio.strategies)
        if (config_json.contains("portfolio") &&
            config_json.at("portfolio").contains("strategies")) {
            config.strategies_config = config_json.at("portfolio").at("strategies");
        }

        // Legacy configs don't have execution/optimization/risk in file
        // These are hardcoded in the application - return defaults
        // Applications should fill these in after loading

        auto validation_result = validate_config(config);
        if (validation_result.is_error()) {
            return make_error<AppConfig>(validation_result.error()->code(),
                                         validation_result.error()->what(),
                                         "ConfigLoader");
        }
        log_config_summary(config);
        return config;

    } catch (const std::exception& e) {
        return make_error<AppConfig>(ErrorCode::INVALID_DATA,
                                     "Failed to extract legacy config: " + std::string(e.what()),
                                     "ConfigLoader");
    }
}

Result<AppConfig> ConfigLoader::load(const std::filesystem::path& config_base_path,
                                      const std::string& portfolio_name,
                                      PostgresDatabase* db) {
    // Load file config first (existing call path).
    auto file_result = load(config_base_path, portfolio_name);
    if (file_result.is_error()) {
        return file_result;  // File load failed; return error, no DB fallback here.
    }
    AppConfig config = file_result.value();

    // If no database supplied, return file config as-is.
    if (db == nullptr) {
        LOGGER_INFO("ConfigLoader", "No database supplied; using file config only.");
        return config;
    }

    // Attempt to load DB override.
    auto db_override_result = load_db_override(db, config.portfolio_id);
    if (db_override_result.is_error()) {
        LOGGER_WARN("ConfigLoader",
                    "Failed to load DB override for portfolio " + config.portfolio_id +
                        ": " + db_override_result.error()->what() +
                        "; continuing with file config.");
        // Fallback: use file config, but log which source was used.
        return config;
    }

    nlohmann::json override = db_override_result.value();
    if (override.is_null() || override.empty()) {
        LOGGER_INFO("ConfigLoader",
                    "No active DB override for portfolio " + config.portfolio_id +
                        "; using file config.");
        return config;
    }

    // Validate override does not contain protected fields.
    auto validate_result = validate_override_no_credentials(override);
    if (validate_result.is_error()) {
        LOGGER_ERROR("ConfigLoader",
                     "DB override validation failed: " + validate_result.error()->what());
        // SECURITY: reject the override entirely if it contains protected fields.
        // Do not merge it; fall back to file config.
        return config;
    }

    // Merge file config with DB override (DB wins).
    // Convert current config to JSON, merge override on top, re-extract.
    nlohmann::json file_json = config.to_json();
    merge_json(file_json, override);

    // Re-extract to AppConfig.
    auto merged_result = extract_config(file_json);
    if (merged_result.is_error()) {
        LOGGER_WARN("ConfigLoader",
                    "Failed to extract config after DB merge: " + merged_result.error()->what() +
                        "; using file config.");
        return config;
    }

    AppConfig merged_config = merged_result.value();
    LOGGER_INFO("ConfigLoader",
                "Successfully merged DB override for portfolio " + config.portfolio_id + ".");

    // Publish effective config to manifest.
    nlohmann::json manifest = strip_credentials_for_manifest(merged_config);
    auto pub_result = publish_config_manifest(db, merged_config.portfolio_id, manifest);
    if (pub_result.is_error()) {
        LOGGER_WARN("ConfigLoader",
                    "Failed to publish config manifest: " + pub_result.error()->what() +
                        "; trading run continues with merged config.");
        // Publishing failure does not stop the run.
    }

    return merged_config;
}

Result<nlohmann::json> ConfigLoader::load_db_override(PostgresDatabase* db,
                                                       const std::string& portfolio_id) {
    // Query: SELECT overrides FROM trading.strategy_config
    //        WHERE portfolio_id = $1 AND is_active = true LIMIT 1
    // Return empty object if no row; error if query fails.

    if (!db || !db->is_connected()) {
        return make_error<nlohmann::json>(ErrorCode::DATABASE_ERROR,
                                          "Database not connected for config override lookup.",
                                          "ConfigLoader::load_db_override");
    }

    try {
        auto conn = db->get_connection();
        if (!conn) {
            return make_error<nlohmann::json>(ErrorCode::DATABASE_ERROR,
                                              "Could not acquire database connection.",
                                              "ConfigLoader::load_db_override");
        }

        // Parameterized query to prevent injection.
        pqxx::result r = conn->exec_params(
            "SELECT overrides FROM trading.strategy_config "
            "WHERE portfolio_id = $1 AND is_active = true "
            "LIMIT 1",
            portfolio_id);

        if (r.empty()) {
            // No active override; return empty object (not an error).
            return nlohmann::json::object();
        }

        std::string overrides_str = r[0][0].as<std::string>();
        nlohmann::json overrides = nlohmann::json::parse(overrides_str);
        return overrides;
    } catch (const pqxx::sql_error& e) {
        return make_error<nlohmann::json>(
            ErrorCode::DATABASE_ERROR,
            "SQL error querying strategy_config: " + std::string(e.what()),
            "ConfigLoader::load_db_override");
    } catch (const pqxx::broken_connection& e) {
        return make_error<nlohmann::json>(ErrorCode::DATABASE_ERROR,
                                          "Database connection lost: " + std::string(e.what()),
                                          "ConfigLoader::load_db_override");
    } catch (const nlohmann::json::parse_error& e) {
        return make_error<nlohmann::json>(
            ErrorCode::JSON_PARSE_ERROR,
            "Failed to parse overrides JSONB: " + std::string(e.what()),
            "ConfigLoader::load_db_override");
    } catch (const std::exception& e) {
        return make_error<nlohmann::json>(ErrorCode::DATABASE_ERROR,
                                          "Unexpected error: " + std::string(e.what()),
                                          "ConfigLoader::load_db_override");
    }
}

Result<void> ConfigLoader::validate_override_no_credentials(const nlohmann::json& override) {
    // Recursive check: reject if override contains database.* or email.password.
    if (!override.is_object()) {
        return ResultVoid::ok();  // Non-object, no fields to validate.
    }

    // Check top-level fields.
    if (override.contains("database")) {
        return make_error<void>(
            ErrorCode::VALIDATION_ERROR,
            "Config override cannot contain 'database' field (security restriction: "
            "prevents credential hijacking).",
            "ConfigLoader::validate_override_no_credentials");
    }

    if (override.contains("email")) {
        const auto& email_obj = override.at("email");
        if (email_obj.is_object() && email_obj.contains("password")) {
            return make_error<void>(
                ErrorCode::VALIDATION_ERROR,
                "Config override cannot contain 'email.password' field (security restriction: "
                "prevents credential hijacking).",
                "ConfigLoader::validate_override_no_credentials");
        }
    }

    return ResultVoid::ok();
}

nlohmann::json ConfigLoader::strip_credentials_for_manifest(const AppConfig& config) {
    // Start with full config JSON.
    nlohmann::json j = config.to_json();

    // Remove database section entirely.
    if (j.contains("database")) {
        j.erase("database");
    }

    // Remove email.password (but keep other email fields).
    if (j.contains("email")) {
        nlohmann::json& email_obj = j.at("email");
        if (email_obj.is_object() && email_obj.contains("password")) {
            email_obj.erase("password");
        }
    }

    return j;
}

Result<void> ConfigLoader::publish_config_manifest(PostgresDatabase* db,
                                                    const std::string& portfolio_id,
                                                    const nlohmann::json& manifest) {
    // INSERT into trading.config_manifest (portfolio_id, effective, published_at)
    // VALUES ($1, $2, now())
    // published_at defaults to now() in the schema.

    if (!db || !db->is_connected()) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Database not connected for config manifest publish.",
                                "ConfigLoader::publish_config_manifest");
    }

    try {
        auto conn = db->get_connection();
        if (!conn) {
            return make_error<void>(ErrorCode::DATABASE_ERROR,
                                    "Could not acquire database connection.",
                                    "ConfigLoader::publish_config_manifest");
        }

        std::string manifest_str = manifest.dump();

        // Parameterized INSERT.
        conn->exec_params(
            "INSERT INTO trading.config_manifest (portfolio_id, effective, published_at) "
            "VALUES ($1, $2, now())",
            portfolio_id, manifest_str);

        return ResultVoid::ok();
    } catch (const pqxx::sql_error& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            "SQL error publishing config manifest: " + std::string(e.what()),
            "ConfigLoader::publish_config_manifest");
    } catch (const pqxx::broken_connection& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Database connection lost: " + std::string(e.what()),
                                "ConfigLoader::publish_config_manifest");
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Unexpected error: " + std::string(e.what()),
                                "ConfigLoader::publish_config_manifest");
    }
}

}  // namespace trade_ngin
