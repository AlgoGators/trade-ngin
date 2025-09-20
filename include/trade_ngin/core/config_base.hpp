// include/trade_ngin/core/config_base.hpp
#pragma once

#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include "trade_ngin/core/error.hpp"

namespace trade_ngin {

/**
 * @brief Base class for all configuration types
 * Provides common serialization and deserialization methods
 */
class ConfigBase {
public:
    virtual ~ConfigBase() = default;

    /**
     * @brief Save configuration to JSON file
     * @param filepath Path to save the file
     * @return Result indicating success or failure
     */
    virtual Result<void> save_to_file(const std::string& filepath) const;

    /**
     * @brief Load configuration from JSON file
     * @param filepath Path to the file
     * @return Result indicating success or failure
     */
    virtual Result<void> load_from_file(const std::string& filepath);

    /**
     * @brief Convert configuration to JSON
     * @return JSON representation of the configuration
     */
    virtual nlohmann::json to_json() const = 0;

    /**
     * @brief Load configuration from JSON
     * @param json JSON object to load from
     */
    virtual void from_json(const nlohmann::json& j) = 0;
};

}  // namespace trade_ngin
