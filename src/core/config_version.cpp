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
        ConfigType component_type = ConfigType::STRATEGY; // Default
        std::string key = make_version_key(from_version, to_version);
        migrations_[component_type][key] = step;

        // Update latest version
        if (latest_versions_.find(component_type) == latest_versions_.end() ||
            to_version.to_string() > latest_versions_[component_type].to_string()) {
            latest_versions_[component_type] = to_version;
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

ConfigVersion ConfigVersionManager::get_latest_version(ConfigType component_type) const {
    auto it = latest_versions_.find(component_type);
    if (it != latest_versions_.end()) {
        return it->second;
    }
    return ConfigVersion{1, 0, 0};  // Default version
}

bool ConfigVersionManager::needs_migration(
    const nlohmann::json& config,
    ConfigType component_type) const {
    
    // Extract version from config
    auto version_result = get_config_version(config);
    if (version_result.is_error()) {
        return false;  // Can't determine version
    }

    ConfigVersion current_version = version_result.value();
    ConfigVersion latest_version = get_latest_version(component_type);

    return current_version < latest_version;
}

Result<MigrationPlan> ConfigVersionManager::create_migration_plan(
    const ConfigVersion& from_version,
    const ConfigVersion& to_version) const {

    MigrationPlan plan;
    plan.start_version = from_version;
    plan.target_version = to_version;

    if (from_version == to_version) {
        return Result<MigrationPlan>(plan); // No migration needed
    }
    
    if (to_version < from_version) {
        return make_error<MigrationPlan>(
            ErrorCode::INVALID_ARGUMENT,
            "Cannot migrate from newer to older version",
            "ConfigVersionManager"
        );
    }

    ConfigType component_type = ConfigType::STRATEGY; // Default

    // Find direct migration
    std::string direct_key = make_version_key(from_version, to_version);
    auto comp_migrations = migrations_.find(component_type);
    if (comp_migrations != migrations_.end()) {
        auto direct_it = comp_migrations->second.find(direct_key);
        if (direct_it != comp_migrations->second.end()) {
            plan.steps.push_back(direct_it->second);
            return Result<MigrationPlan>(plan);
        }
    }

    // No direct migration, find path
    std::vector<ConfigVersion> versions;
    for (const auto& [_, step_map] : migrations_) {
        for (const auto& [__, step] : step_map) {
            if (std::find(versions.begin(), versions.end(), step.from_version) == versions.end()) {
                versions.push_back(step.from_version);
            }
            if (std::find(versions.begin(), versions.end(), step.to_version) == versions.end()) {
                versions.push_back(step.to_version);
            }
        }
    }
    
    // Sort versions
    std::sort(versions.begin(), versions.end());
    
    // Find migration path
    ConfigVersion current = from_version;
    while (current < to_version) {
        // Find next version
        auto next_it = std::upper_bound(versions.begin(), versions.end(), current);
        if (next_it == versions.end() || to_version < *next_it) {
            break; // No more versions or next version is past target
        }
        
        // Find migration step
        std::string key = make_version_key(current, *next_it);
        if (comp_migrations != migrations_.end()) {
            auto step_it = comp_migrations->second.find(key);
            if (step_it != comp_migrations->second.end()) {
                plan.steps.push_back(step_it->second);
                current = *next_it;
                continue;
            }
        }
        
        // No migration step found, try next version
        current = *next_it;
    }
    
    if (current < to_version) {
        return make_error<MigrationPlan>(
            ErrorCode::INVALID_ARGUMENT,
            "Cannot find complete migration path",
            "ConfigVersionManager"
        );
    }
    
    return Result<MigrationPlan>(plan);
}

Result<MigrationResult> ConfigVersionManager::execute_migration(
    nlohmann::json& config,
    const MigrationPlan& plan) const {
    
    MigrationResult result;
    result.original_version = plan.start_version;
    result.final_version = plan.start_version;
    
    // Execute each step
    for (const auto& step : plan.steps) {
        try {
            auto migration_result = step.migrate(config);
            if (migration_result.is_error()) {
                result.warnings.push_back(
                    "Error migrating from " + step.from_version.to_string() + 
                    " to " + step.to_version.to_string() + ": " + 
                    migration_result.error()->what()
                );
                return make_error<MigrationResult>(
                    migration_result.error()->code(),
                    migration_result.error()->what(),
                    "ConfigVersionManager"
                );
            }
            
            // Update config
            config = migration_result.value();
            
            // Update version
            config["version"] = step.to_version.to_string();
            
            // Record change
            result.changes.push_back(
                "Migrated from " + step.from_version.to_string() + 
                " to " + step.to_version.to_string() + ": " + 
                step.description
            );
            
            result.final_version = step.to_version;
            
        } catch (const std::exception& e) {
            result.warnings.push_back(
                "Exception migrating from " + step.from_version.to_string() + 
                " to " + step.to_version.to_string() + ": " + e.what()
            );
            return make_error<MigrationResult>(
                ErrorCode::UNKNOWN_ERROR,
                std::string("Error executing migration: ") + e.what(),
                "ConfigVersionManager"
            );
        }
    }
    
    result.success = true;
    return Result<MigrationResult>(result);
}

Result<MigrationResult> ConfigVersionManager::auto_migrate(
    nlohmann::json& config,
    ConfigType component_type) const {
    
    // Get current version
    auto version_result = get_config_version(config);
    if (version_result.is_error()) {
        // Default to version 1.0.0 if not specified
        config["version"] = "1.0.0";
        return Result<MigrationResult>(MigrationResult{
            true,
            ConfigVersion{1, 0, 0},
            ConfigVersion{1, 0, 0},
            {"Initialized version to 1.0.0"},
            {}
        });
    }
    
    ConfigVersion current_version = version_result.value();
    ConfigVersion latest_version = get_latest_version(component_type);
    
    if (latest_version <= current_version) {
        // Already at latest version
        return Result<MigrationResult>(MigrationResult{
            true,
            current_version,
            current_version,
            {},
            {}
        });
    }
    
    // Create migration plan
    auto plan_result = create_migration_plan(current_version, latest_version);
    if (plan_result.is_error()) {
        return make_error<MigrationResult>(
            plan_result.error()->code(),
            plan_result.error()->what(),
            "ConfigVersionManager"
        );
    }
    
    // Execute migration
    return execute_migration(config, plan_result.value());
}

Result<ConfigVersion> ConfigVersionManager::get_config_version(
    const nlohmann::json& config) const {
    
    try {
        if (!config.contains("version") || !config["version"].is_string()) {
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

    if (step.from_version == step.to_version) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "From and to versions cannot be the same",
            "ConfigVersionManager"
        );
    }
    
    if (step.to_version < step.from_version) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "To version must be greater than from version",
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

    return Result<void>();
}

std::string ConfigVersionManager::make_version_key(
    const ConfigVersion& from_version,
    const ConfigVersion& to_version) {
    
    return from_version.to_string() + "_" + to_version.to_string();
}

} // namespace trade_ngin