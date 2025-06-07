//include/data/credential_store.hpp
#pragma once

#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace trade_ngin {

class CredentialStore {
private:
    json config;
    std::string config_path;

public:
    // Constructor with default config path
    CredentialStore(const std::string& path = "config.json");
    
    // Load or reload configuration from file
    void loadConfig();
    
    // Get a value from the config, throws if not found
    template<typename T>
    T get(const std::string& section, const std::string& key);
    
    // Get a value with default fallback
    template<typename T>
    T getWithDefault(const std::string& section, const std::string& key, const T& defaultValue);
};

// Template methods must be defined in the header
template<typename T>
T CredentialStore::get(const std::string& section, const std::string& key) {
    if (!config.contains(section) || !config[section].contains(key)) {
        throw std::runtime_error("Configuration not found: " + section + "." + key);
    }
    return config[section][key].get<T>();
}

template<typename T>
T CredentialStore::getWithDefault(const std::string& section, const std::string& key, const T& defaultValue) {
    if (!config.contains(section) || !config[section].contains(key)) {
        return defaultValue;
    }
    return config[section][key].get<T>();
}
} // namespace trade_ngin