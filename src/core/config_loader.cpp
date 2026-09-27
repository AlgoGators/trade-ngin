// src/core/config_loader.cpp

#include "trade_ngin/core/config_loader.hpp"

#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <string_view>

#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

namespace {

struct InvalidProjectionField {};

using Json = nlohmann::json;

Json finite_number(double value) {
    if (!std::isfinite(value)) throw InvalidProjectionField{};
    return value;
}

Json typed_integer(const Json& value) {
    if (!value.is_number_integer()) throw InvalidProjectionField{};
    if (value.is_number_unsigned()) {
        const auto n = value.get<std::uint64_t>();
        if (n > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            throw InvalidProjectionField{};
        return static_cast<int>(n);
    }
    const auto n = value.get<std::int64_t>();
    if (n < std::numeric_limits<int>::min() || n > std::numeric_limits<int>::max())
        throw InvalidProjectionField{};
    return static_cast<int>(n);
}

Json typed_pairs(const Json& value, bool second_integer) {
    if (!value.is_array()) throw InvalidProjectionField{};
    Json result = Json::array();
    for (const auto& pair : value) {
        if (!pair.is_array() || pair.size() != 2) throw InvalidProjectionField{};
        Json second;
        if (second_integer) {
            second = typed_integer(pair.at(1));
        } else {
            if (!pair.at(1).is_number()) throw InvalidProjectionField{};
            second = finite_number(pair.at(1).get<double>());
        }
        result.push_back(Json::array({typed_integer(pair.at(0)), second}));
    }
    return result;
}

std::optional<Json> supplied(const Json* source, std::string_view key,
                             std::string_view value_type) {
    if (source == nullptr) return std::nullopt;
    const auto it = source->find(std::string(key));
    if (it == source->end()) return std::nullopt;
    const auto& value = *it;
    if (value_type == "number") {
        if (!value.is_number()) throw InvalidProjectionField{};
        return finite_number(value.get<double>());
    }
    if (value_type == "integer") return typed_integer(value);
    if (value_type == "boolean") {
        if (!value.is_boolean()) throw InvalidProjectionField{};
        return value.get<bool>();
    }
    if (value_type == "enum_string") {
        if (!value.is_string()) throw InvalidProjectionField{};
        return value.get<std::string>();
    }
    if (value_type == "integer_pairs" || value_type == "integer_number_pairs")
        return typed_pairs(value, value_type == "integer_pairs");
    throw InvalidProjectionField{};
}

void add_field(Json& fields, const std::string& path, const char* classification,
               const char* reason, const char* condition, const char* value_type,
               const char* unit, const char* origin, const char* state,
               std::optional<Json> value = std::nullopt) {
    Json field = {{"path", path}, {"scope", "exact"},
                  {"classification", classification}, {"reason", reason},
                  {"condition", condition}, {"value_type", value_type},
                  {"unit", unit}, {"value_origin", origin},
                  {"value_state", state}};
    if (value) field["value"] = std::move(*value);
    fields.push_back(std::move(field));
}

void member(Json& fields, const char* path, const char* classification,
            const char* reason, const char* condition, const char* value_type,
            const char* unit, Json value) {
    add_field(fields, path, classification, reason, condition, value_type, unit,
              "app_config_member", "included", std::move(value));
}

void descriptor(Json& fields, const char* path, const char* classification,
                const char* reason, const char* condition, const char* value_type,
                const char* unit) {
    add_field(fields, path, classification, reason, condition, value_type, unit,
              "not_projected", "omitted");
}

void strategy_leaf(Json& fields, const std::string& root, const Json* source,
                   const char* key, const char* classification, const char* reason,
                   const char* condition, const char* value_type, const char* unit) {
    auto value = supplied(source, key, value_type);
    add_field(fields, root + key, classification, reason, condition, value_type,
              unit, "configured_strategy_leaf", value ? "included" : "absent_in_input",
              std::move(value));
}

bool valid_strategy_id(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
               (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
}

bool known_strategy_type(const std::string& type) {
    return type == "TrendFollowingStrategy" ||
           type == "TrendFollowingFastStrategy" ||
           type == "TrendFollowingSlowStrategy";
}

void add_strategy(Json& fields, const std::string& id, const Json& definition) {
    if (!valid_strategy_id(id) || !definition.is_object()) throw InvalidProjectionField{};
    const std::string root = "/strategies/" + id + "/";
    const Json* params = nullptr;
    const auto config_it = definition.find("config");
    if (config_it != definition.end()) {
        if (!config_it->is_object()) throw InvalidProjectionField{};
        params = &*config_it;
    }
    strategy_leaf(fields, root, &definition, "enabled_live", "source_supported_config_input",
                  "source_reader", "strategy_selection", "boolean", "flag");
    strategy_leaf(fields, root, &definition, "default_allocation", "source_supported_config_input",
                  "source_reader", "strategy_selection", "number", "fraction");
    strategy_leaf(fields, root, &definition, "enabled_backtest", "unsupported_in_profile",
                  "backtest_only", "no_active_profile_reader", "boolean", "flag");
    auto type = supplied(&definition, "type", "enum_string");
    if (type && !known_strategy_type(type->get<std::string>())) {
        add_field(fields, root + "type", "unsupported_in_profile", "unknown_strategy_type",
                  "no_active_profile_reader", "enum_string", "strategy_type",
                  "not_projected", "omitted");
        return;
    }
    add_field(fields, root + "type", "source_supported_config_input", "source_reader",
              "strategy_dispatch", "enum_string", "strategy_type",
              "configured_strategy_leaf", type ? "included" : "absent_in_input",
              std::move(type));
    const std::string cfg = root + "config/";
    strategy_leaf(fields, cfg, params, "weight", "source_supported_config_input", "source_reader",
                  "selected_known_strategy_buffering_enabled", "number", "multiplier");
    strategy_leaf(fields, cfg, params, "risk_target", "source_supported_config_input",
                  "source_reader", "selected_known_strategy", "number",
                  "annualized_volatility_fraction");
    strategy_leaf(fields, cfg, params, "idm", "source_supported_config_input", "source_reader",
                  "selected_known_strategy", "number", "multiplier");
    strategy_leaf(fields, cfg, params, "max_symbol_concentration", "source_supported_config_input",
                  "source_reader", "selected_known_strategy", "number", "fraction");
    strategy_leaf(fields, cfg, params, "use_position_buffering", "source_supported_config_input",
                  "source_reader", "selected_known_strategy", "boolean", "flag");
    strategy_leaf(fields, cfg, params, "carver_buffer_floor", "source_supported_config_input",
                  "source_reader", "selected_known_strategy_buffering_enabled", "number", "contracts");
    strategy_leaf(fields, cfg, params, "carver_buffer_position_factor",
                  "source_supported_config_input", "source_reader",
                  "selected_known_strategy_buffering_enabled", "number", "fraction");
    strategy_leaf(fields, cfg, params, "ema_windows", "source_supported_config_input",
                  "source_reader", "selected_known_strategy", "integer_pairs", "short_long_bar_pairs");
    strategy_leaf(fields, cfg, params, "vol_lookback_short", "source_supported_config_input",
                  "source_reader", "selected_known_strategy", "integer", "bar_windows");
    strategy_leaf(fields, cfg, params, "vol_lookback_long", "source_supported_config_input",
                  "source_reader", "selected_known_strategy", "integer", "bar_windows");
    add_field(fields, cfg + "fx_rate", "unsupported_in_profile", "typed_member_not_input_wired",
              "no_active_profile_reader", "number", "currency_ratio", "not_projected", "omitted");
    add_field(fields, cfg + "max_history_size", "unsupported_in_profile",
              "typed_member_not_input_wired", "no_active_profile_reader", "integer",
              "bar_records", "not_projected", "omitted");
    add_field(fields, cfg + "fdm", "unsupported_in_profile", "typed_member_not_input_wired",
              "no_active_profile_reader", "integer_number_pairs", "rule_count_multiplier_pairs",
              "not_projected", "omitted");
}

}  // namespace

Result<nlohmann::json> project_live_config_fields(const AppConfig& config) {
    try {
        Json fields = Json::array();
        descriptor(fields, "/portfolio_id", "read_only_metadata", "identity_metadata",
                   "metadata_only", "string", "identity");
        member(fields, "/initial_capital", "source_supported_config_input", "source_reader",
               "source_path", "number", "account_currency", finite_number(config.initial_capital));
        member(fields, "/reserve_capital_pct", "unsupported_in_profile", "stored_metadata_only",
               "no_active_profile_reader", "number", "fraction",
               finite_number(config.reserve_capital_pct));
        if (config.benchmark_mode != "live" && config.benchmark_mode != "deferred")
            throw InvalidProjectionField{};
        member(fields, "/benchmark_mode", "source_supported_config_input", "source_reader",
               "benchmark_stage", "enum_string", "mode", config.benchmark_mode);

        member(fields, "/execution/commission_rate", "unsupported_in_profile",
               "not_wired_to_futures_cost_model", "no_active_profile_reader", "number",
               "unverified_rate", finite_number(config.execution.commission_rate));
        member(fields, "/execution/slippage_bps", "unsupported_in_profile",
               "not_wired_to_futures_cost_model", "no_active_profile_reader", "number",
               "basis_points", finite_number(config.execution.slippage_bps));
        member(fields, "/execution/position_limit_backtest", "unsupported_in_profile",
               "backtest_only", "no_active_profile_reader", "number", "contracts",
               finite_number(config.execution.position_limit_backtest));
        member(fields, "/execution/position_limit_live", "source_supported_config_input",
               "source_reader", "base_position_validation", "number", "contracts",
               finite_number(config.execution.position_limit_live));

        member(fields, "/optimization/tau", "source_supported_config_input", "source_reader",
               "optimizer_succeeded_and_buffering_enabled", "number", "risk_scale",
               finite_number(config.opt_config.tau));
        member(fields, "/optimization/capital", "read_only_metadata", "derived_alias",
               "metadata_only", "number", "account_currency", finite_number(config.opt_config.capital));
        member(fields, "/optimization/cost_penalty_scalar", "source_supported_config_input",
               "source_reader", "optimizer_enabled", "number", "multiplier",
               finite_number(config.opt_config.cost_penalty_scalar));
        member(fields, "/optimization/asymmetric_risk_buffer", "unsupported_in_profile",
               "no_active_reader", "no_active_profile_reader", "number", "unverified_buffer_fraction",
               finite_number(config.opt_config.asymmetric_risk_buffer));
        member(fields, "/optimization/max_iterations", "source_supported_config_input",
               "source_reader", "optimizer_enabled", "integer", "iterations",
               config.opt_config.max_iterations);
        member(fields, "/optimization/convergence_threshold", "source_supported_config_input",
               "source_reader", "optimizer_enabled", "number", "objective_difference",
               finite_number(config.opt_config.convergence_threshold));
        member(fields, "/optimization/use_buffering", "source_supported_config_input",
               "source_reader", "optimizer_enabled", "boolean", "flag", config.opt_config.use_buffering);
        member(fields, "/optimization/buffer_size_factor", "source_supported_config_input",
               "source_reader", "optimizer_succeeded_and_buffering_enabled", "number", "multiplier",
               finite_number(config.opt_config.buffer_size_factor));
        descriptor(fields, "/optimization/version", "read_only_metadata", "version_metadata",
                   "metadata_only", "string", "config_version");

        member(fields, "/risk/var_limit", "source_supported_config_input", "source_reader",
               "risk_enabled", "number", "annualized_volatility_fraction",
               finite_number(config.risk_config.var_limit));
        member(fields, "/risk/jump_risk_limit", "source_supported_config_input", "source_reader",
               "risk_enabled", "number", "fraction", finite_number(config.risk_config.jump_risk_limit));
        member(fields, "/risk/corr_shock_threshold", "unsupported_in_profile", "inactive_alternative",
               "no_active_profile_reader", "number", "annualized_volatility_fraction",
               finite_number(config.risk_config.corr_shock_threshold));
        member(fields, "/risk/jump_shock_threshold", "unsupported_in_profile", "inactive_alternative",
               "no_active_profile_reader", "number", "annualized_volatility_fraction",
               finite_number(config.risk_config.jump_shock_threshold));
        member(fields, "/risk/max_gross_leverage", "source_supported_config_input", "source_reader",
               "risk_enabled", "number", "leverage_multiple",
               finite_number(config.risk_config.max_gross_leverage));
        member(fields, "/risk/max_net_leverage", "source_supported_config_input", "source_reader",
               "risk_enabled", "number", "leverage_multiple",
               finite_number(config.risk_config.max_net_leverage));
        member(fields, "/risk/capital", "read_only_metadata", "derived_alias", "metadata_only",
               "number", "account_currency", finite_number(static_cast<double>(config.risk_config.capital)));
        descriptor(fields, "/risk/version", "read_only_metadata", "version_metadata",
                   "metadata_only", "string", "config_version");
        member(fields, "/risk/max_drawdown", "source_supported_config_input", "source_reader",
               "base_strategy_risk_check", "number", "fraction", finite_number(config.max_drawdown));
        member(fields, "/risk/max_leverage", "source_supported_config_input", "diagnostic_reader",
               "base_strategy_risk_check", "number", "leverage_multiple", finite_number(config.max_leverage));
        member(fields, "/risk_defaults/confidence_level", "source_supported_config_input",
               "source_reader", "risk_enabled", "number", "probability",
               finite_number(config.risk_config.confidence_level));
        member(fields, "/risk_defaults/lookback_period", "source_supported_config_input",
               "source_reader", "risk_enabled", "integer", "bar_records",
               config.risk_config.lookback_period);
        member(fields, "/risk_defaults/max_correlation", "source_supported_config_input",
               "source_reader", "risk_enabled", "number", "absolute_correlation",
               finite_number(config.risk_config.max_correlation));

        member(fields, "/backtest/lookback_years", "unsupported_in_profile", "backtest_only",
               "no_active_profile_reader", "integer", "years", config.backtest.lookback_years);
        member(fields, "/backtest/store_trade_details", "unsupported_in_profile", "backtest_only",
               "no_active_profile_reader", "boolean", "flag", config.backtest.store_trade_details);
        member(fields, "/live/historical_days", "source_supported_config_input", "source_reader",
               "source_path", "integer", "calendar_days", config.live.historical_days);

        Json fdm = Json::array();
        for (const auto& [count, multiplier] : config.strategy_defaults.fdm)
            fdm.push_back(Json::array({count, finite_number(multiplier)}));
        member(fields, "/strategy_defaults/fdm", "unsupported_in_profile", "blocked_default_fallback",
               "no_active_profile_reader", "integer_number_pairs", "rule_count_multiplier_pairs",
               std::move(fdm));
        member(fields, "/strategy_defaults/max_strategy_allocation", "source_supported_config_input",
               "source_reader", "allocation_validation", "number", "fraction",
               finite_number(config.strategy_defaults.max_strategy_allocation));
        member(fields, "/strategy_defaults/min_strategy_allocation", "source_supported_config_input",
               "source_reader", "allocation_validation", "number", "fraction",
               finite_number(config.strategy_defaults.min_strategy_allocation));
        member(fields, "/strategy_defaults/use_optimization", "source_supported_config_input",
               "source_reader", "source_path", "boolean", "flag",
               config.strategy_defaults.use_optimization);
        member(fields, "/strategy_defaults/use_risk_management", "source_supported_config_input",
               "source_reader", "source_path", "boolean", "flag",
               config.strategy_defaults.use_risk_management);
        member(fields, "/strategy_defaults/carver_buffer_floor", "source_supported_config_input",
               "source_reader", "strategy_config_present_leaf_absent_and_buffering_enabled",
               "number", "contracts", finite_number(config.strategy_defaults.carver_buffer_floor));
        member(fields, "/strategy_defaults/carver_buffer_position_factor",
               "source_supported_config_input", "source_reader",
               "strategy_config_present_leaf_absent_and_buffering_enabled", "number", "fraction",
               finite_number(config.strategy_defaults.carver_buffer_position_factor));

        if (config.strategies_config.is_object()) {
            for (auto it = config.strategies_config.begin(); it != config.strategies_config.end(); ++it)
                add_strategy(fields, it.key(), it.value());
        } else if (!config.strategies_config.is_null()) {
            throw InvalidProjectionField{};
        }
        std::sort(fields.begin(), fields.end(), [](const Json& a, const Json& b) {
            return a.at("path").get_ref<const std::string&>() <
                   b.at("path").get_ref<const std::string&>();
        });
        for (std::size_t i = 1; i < fields.size(); ++i) {
            if (fields[i - 1].at("path") == fields[i].at("path"))
                throw InvalidProjectionField{};
        }
        return Json{{"projection_version", 1},
                    {"profile", "live_portfolio_runner_futures"},
                    {"coverage", "current_typed_fields_and_known_strategy_leaves"},
                    {"authority", "inspection_only"},
                    {"consumption_evidence", "not_collected"},
                    {"fields", std::move(fields)}};
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        return make_error<nlohmann::json>(ErrorCode::INVALID_DATA,
                                          "config_projection_invalid_field",
                                          "ConfigFieldProjection");
    }
}

bool validate_live_config_projection_for_publication(const nlohmann::json& projection) {
    try {
        if (!projection.is_object() || projection.size() != 6 ||
            !projection.at("projection_version").is_number_integer() ||
            projection.at("projection_version") != 1 ||
            projection.at("profile") != "live_portfolio_runner_futures" ||
            projection.at("coverage") != "current_typed_fields_and_known_strategy_leaves" ||
            projection.at("authority") != "inspection_only" ||
            projection.at("consumption_evidence") != "not_collected" ||
            !projection.at("fields").is_array() ||
            projection.dump().size() > 2u * 1024u * 1024u) return false;

        const auto& fields = projection.at("fields");
        if (fields.size() > 20000) return false;
        std::map<std::string, const Json*> by_path;
        std::set<std::string> strategy_ids;
        std::string prior;
        for (const auto& field : fields) {
            if (!field.is_object() || !field.contains("path") ||
                !field.at("path").is_string()) return false;
            const auto& path = field.at("path").get_ref<const std::string&>();
            if (!prior.empty() && path <= prior) return false;
            prior = path;
            by_path.emplace(path, &field);
            if (path.rfind("/strategies/", 0) == 0) {
                const auto slash = path.find('/', 12);
                if (slash == std::string::npos ||
                    !valid_strategy_id(path.substr(12, slash - 12))) return false;
                strategy_ids.insert(path.substr(12, slash - 12));
                if (strategy_ids.size() > 1024) return false;
            }
        }

        // Only descriptors come from this safe synthetic input. Its values are
        // never compared with, substituted for, or used to resolve the capture.
        AppConfig catalog_input;
        catalog_input.strategies_config = Json::object();
        for (const auto& id : strategy_ids) {
            const auto type = by_path.find("/strategies/" + id + "/type");
            if (type == by_path.end() || !type->second->contains("reason") ||
                !type->second->at("reason").is_string()) return false;
            const bool unknown = type->second->at("reason") == "unknown_strategy_type";
            catalog_input.strategies_config[id] = {
                {"type", unknown ? "UnknownSynthetic" : "TrendFollowingStrategy"}};
        }
        const auto projected_catalog = project_live_config_fields(catalog_input);
        if (projected_catalog.is_error()) return false;
        const auto& catalog = projected_catalog.value().at("fields");
        if (fields.size() != catalog.size()) return false;
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const auto& field = fields[index];
            const auto& expected = catalog[index];
            for (const char* key : {"path", "scope", "classification", "reason",
                                    "condition", "value_type", "unit", "value_origin"})
                if (!field.contains(key) || field.at(key) != expected.at(key)) return false;
            if (!field.contains("value_state") || !field.at("value_state").is_string())
                return false;
            const auto& state = field.at("value_state").get_ref<const std::string&>();
            const auto& origin = expected.at("value_origin").get_ref<const std::string&>();
            const bool included = state == "included";
            if (origin == "not_projected") {
                if (state != "omitted") return false;
            } else if (origin == "app_config_member") {
                if (!included) return false;
            } else if (origin == "configured_strategy_leaf") {
                if (!included && state != "absent_in_input") return false;
            } else return false;
            if (field.size() != (included ? 10u : 9u) ||
                field.contains("value") != included) return false;
            if (!included) continue;

            const auto& value = field.at("value");
            const auto& type = expected.at("value_type").get_ref<const std::string&>();
            if (type == "number") {
                if (!value.is_number() || !std::isfinite(value.get<double>())) return false;
            } else if (type == "integer") {
                (void)typed_integer(value);
            } else if (type == "boolean") {
                if (!value.is_boolean()) return false;
            } else if (type == "enum_string") {
                if (!value.is_string()) return false;
                if (field.at("path") == "/benchmark_mode") {
                    if (value != "live" && value != "deferred") return false;
                } else if (!known_strategy_type(value.get<std::string>())) return false;
            } else if (type == "integer_pairs" || type == "integer_number_pairs") {
                (void)typed_pairs(value, type == "integer_pairs");
            } else return false;
        }
        return true;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        return false;
    } catch (const InvalidProjectionField&) {
        return false;
    }
}

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

        // Benchmark mode
        if (merged.contains("benchmark_mode")) {
            std::string mode = merged.at("benchmark_mode").get<std::string>();
            if (mode == "live" || mode == "deferred") {
                config.benchmark_mode = mode;
            } else {
                WARN("Invalid benchmark_mode '" + mode +
                     "' (expected 'live' or 'deferred'); defaulting to 'live'");
                config.benchmark_mode = "live";
            }
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
            const auto& risk = merged.at("risk");
            config.risk_config.from_json(risk);

            // Canonical nested limits take precedence over legacy top-level input.
            if (risk.contains("max_drawdown")) {
                config.max_drawdown = risk.at("max_drawdown").get<double>();
            } else if (merged.contains("max_drawdown")) {
                config.max_drawdown = merged.at("max_drawdown").get<double>();
            }
            if (risk.contains("max_leverage")) {
                config.max_leverage = risk.at("max_leverage").get<double>();
            } else if (merged.contains("max_leverage")) {
                config.max_leverage = merged.at("max_leverage").get<double>();
            }
        } else {
            if (merged.contains("max_drawdown")) {
                config.max_drawdown = merged.at("max_drawdown").get<double>();
            }
            if (merged.contains("max_leverage")) {
                config.max_leverage = merged.at("max_leverage").get<double>();
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

}  // namespace trade_ngin
