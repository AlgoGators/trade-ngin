// src/core/config_version.cpp
#include "trade_ngin/core/config_version.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/error.hpp"
#include <sstream>
#include <regex>
#include <queue>
#include <set>

namespace trade_ngin {

std::string ConfigVersion::to_string() const {
    std::stringstream ss;
    ss << major << "." << minor << "." << patch;
    return ss.str();
}

ConfigVersion ConfigVersion::from_string(const std::string& version_str) {
    std::regex version_regex(R"((\d+)\.(\d+)\.(\d+))");
    std::smatch matches;
    
    if (!std::regex_match(version_str, matches, version_regex)) {
        throw std::runtime_error("Invalid version string: " + version_str);
    }

    return ConfigVersion{
        std::stoi(matches[1].str()),
        std::stoi(matches[2].str()),
        std::stoi(matches[3].str())
    };
}

bool ConfigVersion::operator<(const ConfigVersion& other) const {
    if (major != other.major) return major < other.major;
    if (minor != other.minor) return minor < other.minor;
    return patch < other.patch;
}

bool ConfigVersion::operator==(const ConfigVersion& other) const {
    return major == other.major && 
           minor == other.minor && 
           patch == other.patch;
}

Result<void> ConfigVersionManager::register_migration(
    const ConfigVersion& from_version,
    const ConfigVersion& to_version,
    MigrationFunction migration,
    const std::string& description) {
    
    try {
        MigrationStep step{
            from_version,
            to_version,
            std::move(migration),
            description
        };

        // Validate step
        auto validation = validate_migration_step(step);
        if (validation.is_error()) {
            return validation;
        }

        // Store migration
        std::string key = make_version_key(from_version, to_version);
        migrations_[ConfigType::STRATEGY][key] = std::move(step);

        // Update latest version if needed
        auto& latest = latest_versions_[ConfigType::STRATEGY];
        if (latest < to_version) {
            latest = to_version;
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error registering migration: ") + e.what(),
            "ConfigVersionManager"
        );
    }
}

ConfigVersion ConfigVersionManager::get_latest_version(
    ConfigType component_type) const {
    
    auto it = latest_versions_.find(component_type);
    if (it != latest_versions_.end()) {
        return it->second;
    }
    return ConfigVersion{1, 0, 0};  // Default version
}

bool ConfigVersionManager::needs_migration(
    const nlohmann::json& config,
    ConfigType component_type) const {
    
    auto version_result = get_config_version(config);
    if (version_result.is_error()) {
        return true;  // No version info means migration needed
    }

    return version_result.value() < get_latest_version(component_type);
}

Result<MigrationPlan> ConfigVersionManager::create_migration_plan(
    const ConfigVersion& from_version,
    const ConfigVersion& to_version) const {
    
    if (!(from_version < to_version)) {
        return make_error<MigrationPlan>(
            ErrorCode::INVALID_ARGUMENT,
            "Target version must be greater than current version",
            "ConfigVersionManager"
        );
    }

    // Find all possible paths using BFS
    std::queue<std::vector<MigrationStep>> paths;
    std::set<ConfigVersion> visited;
    
    // Start with empty path
    paths.push({});
    visited.insert(from_version);

    MigrationPlan best_plan;
    bool found_path = false;

    while (!paths.empty()) {
        auto current_path = paths.front();
        paths.pop();

        ConfigVersion current_version = current_path.empty() ? 
            from_version : current_path.back().to_version;

        if (current_version == to_version) {
            if (!found_path || current_path.size() < best_plan.steps.size()) {
                best_plan.steps = current_path;
                best_plan.start_version = from_version;
                best_plan.target_version = to_version;
                found_path = true;
            }
            continue;
        }

        // Try all possible next steps
        for (const auto& [_, migrations] : migrations_) {
            for (const auto& [__, step] : migrations) {
                if (step.from_version == current_version &&
                    visited.find(step.to_version) == visited.end() &&
                    step.to_version <= to_version) {
                    
                    auto new_path = current_path;
                    new_path.push_back(step);
                    paths.push(new_path);
                    visited.insert(step.to_version);
                }
            }
        }
    }

    if (!found_path) {
        return make_error<MigrationPlan>(
            ErrorCode::INVALID_ARGUMENT,
            "No migration path found",
            "ConfigVersionManager"
        );
    }

    return Result<MigrationPlan>(best_plan);
}

Result<MigrationResult> ConfigVersionManager::execute_migration(
    nlohmann::json& config,
    const MigrationPlan& plan) const {
    
    MigrationResult result;
    result.original_version = plan.start_version;
    
    try {
        // Execute each migration step
        for (const auto& step : plan.steps) {
            auto migration_result = step.migrate(config);
            if (migration_result.is_error()) {
                return make_error<MigrationResult>(
                    migration_result.error()->code(),
                    migration_result.error()->what(),
                    "ConfigVersionManager"
                );
            }

            config = migration_result.value();
            result.changes.push_back(step.description);
            
            // Update version in config
            config["version"] = step.to_version.to_string();
        }

        result.success = true;
        result.final_version = plan.target_version;
        return Result<MigrationResult>(result);

    } catch (const std::exception& e) {
        return make_error<MigrationResult>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error executing migration: ") + e.what(),
            "ConfigVersionManager"
        );
    }
}

Result<MigrationResult> ConfigVersionManager::auto_migrate(
    nlohmann::json& config,
    ConfigType component_type) const {
    
    try {
        // Get current version
        auto version_result = get_config_version(config);
        if (version_result.is_error()) {
            // No version info, assume oldest version
            version_result = Result<ConfigVersion>(ConfigVersion{1, 0, 0});
        }

        ConfigVersion current_version = version_result.value();
        ConfigVersion latest_version = get_latest_version(component_type);

        if (!(current_version < latest_version)) {
            // Already at latest version
            MigrationResult result;
            result.success = true;
            result.original_version = current_version;
            result.final_version = current_version;
            return Result<MigrationResult>(result);
        }

        // Create and execute migration plan
        auto plan_result = create_migration_plan(current_version, latest_version);
        if (plan_result.is_error()) {
            return make_error<MigrationResult>(
                plan_result.error()->code(),
                plan_result.error()->what(),
                "ConfigVersionManager"
            );
        }

        return execute_migration(config, plan_result.value());

    } catch (const std::exception& e) {
        return make_error<MigrationResult>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error in auto migration: ") + e.what(),
            "ConfigVersionManager"
        );
    }
}

Result<ConfigVersion> ConfigVersionManager::get_config_version(
    const nlohmann::json& config) const {
    
    try {
        if (!config.contains("version")) {
            return make_error<ConfigVersion>(
                ErrorCode::INVALID_ARGUMENT,
                "No version field in config",
                "ConfigVersionManager"
            );
        }

        return Result<ConfigVersion>(
            ConfigVersion::from_string(config["version"].get<std::string>())
        );

    } catch (const std::exception& e) {
        return make_error<ConfigVersion>(
            ErrorCode::INVALID_ARGUMENT,
            std::string("Error parsing version: ") + e.what(),
            "ConfigVersionManager"
        );
    }
}

Result<void> ConfigVersionManager::validate_migration_step(
    const MigrationStep& step) const {
    
    if (!(step.from_version < step.to_version)) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Target version must be greater than source version",
            "ConfigVersionManager"
        );
    }

    if (!step.migrate) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Migration function cannot be null",
            "ConfigVersionManager"
        );
    }

    if (step.description.empty()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Migration description cannot be empty",
            "ConfigVersionManager"
        );
    }

    return Result<void>();
}

std::string ConfigVersionManager::make_version_key(
    const ConfigVersion& from_version,
    const ConfigVersion& to_version) {
    
    return from_version.to_string() + "_" + to_version.to_string();
}

} // namespace trade_ngin