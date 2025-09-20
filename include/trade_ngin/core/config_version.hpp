#pragma once

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <regex>
#include <string>
#include <unordered_map>
#include "trade_ngin/core/config_manager.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

/**
 * @brief Semantic version for configuration
 */
struct ConfigVersion {
    int major{0};
    int minor{0};
    int patch{0};

    std::string to_string() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    static ConfigVersion from_string(const std::string& version_str) {
        std::regex version_regex(R"((\d+)\.(\d+)\.(\d+))");
        std::smatch matches;

        if (!std::regex_match(version_str, matches, version_regex)) {
            throw std::runtime_error("Invalid version format: " + version_str);
        }

        ConfigVersion version;
        version.major = std::stoi(matches[1]);
        version.minor = std::stoi(matches[2]);
        version.patch = std::stoi(matches[3]);

        return version;
    }

    bool operator<(const ConfigVersion& other) const {
        if (major != other.major)
            return major < other.major;
        if (minor != other.minor)
            return minor < other.minor;
        return patch < other.patch;
    }

    bool operator==(const ConfigVersion& other) const {
        return major == other.major && minor == other.minor && patch == other.patch;
    }

    bool operator<=(const ConfigVersion& other) const {
        return *this < other || *this == other;
    }
};

/**
 * @brief Migration function type
 */
using MigrationFunction = std::function<Result<nlohmann::json>(const nlohmann::json&)>;

/**
 * @brief Configuration migration definition
 */
struct MigrationStep {
    ConfigVersion from_version;
    ConfigVersion to_version;
    MigrationFunction migrate;
    std::string description;
};

/**
 * @brief Migration plan for a series of upgrades
 */
struct MigrationPlan {
    std::vector<MigrationStep> steps;
    ConfigVersion start_version;
    ConfigVersion target_version;
};

/**
 * @brief Migration result including changes made
 */
struct MigrationResult {
    bool success{false};
    ConfigVersion original_version;
    ConfigVersion final_version;
    std::vector<std::string> changes;
    std::vector<std::string> warnings;
};

/**
 * @brief Configuration version manager
 */
class ConfigVersionManager {
public:
    /**
     * @brief Get singleton instance
     */
    static ConfigVersionManager& instance() {
        static ConfigVersionManager instance;
        return instance;
    }

    /**
     * @brief Register a migration step
     * @param from_version Starting version
     * @param to_version Target version
     * @param migration Migration function
     * @param description Description of changes
     * @return Result indicating success or failure
     */
    Result<void> register_migration(const ConfigVersion& from_version,
                                    const ConfigVersion& to_version, MigrationFunction migration,
                                    const std::string& description);

    /**
     * @brief Get latest version for a component
     * @param component_type Type of component
     * @return Latest version number
     */
    ConfigVersion get_latest_version(ConfigType component_type) const;

    /**
     * @brief Check if config needs migration
     * @param config Configuration to check
     * @param component_type Component type
     * @return true if migration needed
     */
    bool needs_migration(const nlohmann::json& config, ConfigType component_type) const;

    /**
     * @brief Create migration plan
     * @param from_version Starting version
     * @param to_version Target version
     * @return Migration plan or error
     */
    Result<MigrationPlan> create_migration_plan(const ConfigVersion& from_version,
                                                const ConfigVersion& to_version) const;

    /**
     * @brief Execute migration plan
     * @param config Configuration to migrate
     * @param plan Migration plan to execute
     * @return Migration result
     */
    Result<MigrationResult> execute_migration(nlohmann::json& config,
                                              const MigrationPlan& plan) const;

    /**
     * @brief Automatically migrate config to latest version
     * @param config Configuration to migrate
     * @param component_type Component type
     * @return Migration result
     */
    Result<MigrationResult> auto_migrate(nlohmann::json& config, ConfigType component_type) const;

    /**
     * @brief Reset the manager's state (for testing)
     */
    static void reset_instance() {
        instance().migrations_.clear();
        instance().latest_versions_.clear();
    }

private:
    ConfigVersionManager() = default;

    // Store migrations by component type and version pair
    std::unordered_map<ConfigType,
                       std::unordered_map<std::string,  // version pair string
                                                        // "from_to"
                                          MigrationStep>>
        migrations_;

    // Latest version by component type
    std::unordered_map<ConfigType, ConfigVersion> latest_versions_;

    /**
     * @brief Get version from config
     * @param config Configuration to check
     * @return Version or error
     */
    Result<ConfigVersion> get_config_version(const nlohmann::json& config) const;

    /**
     * @brief Validate migration step
     * @param step Step to validate
     * @return Result indicating success or failure
     */
    Result<void> validate_migration_step(const MigrationStep& step) const;

    /**
     * @brief Create version pair key
     * @param from_version Starting version
     * @param to_version Target version
     * @return String key for migrations map
     */
    static std::string make_version_key(const ConfigVersion& from_version,
                                        const ConfigVersion& to_version);
};

}  // namespace trade_ngin
