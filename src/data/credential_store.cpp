#include "trade_ngin/data/credential_store.hpp"
#include <fstream>
#include <iostream>
#include <cstdlib>

namespace trade_ngin {

CredentialStore::CredentialStore(const std::string& path) : config_path(path) {
    // Check for environment variable to override path
    const char* env_config = std::getenv("TRADING_CONFIG_PATH");
    if (env_config) {
        config_path = env_config;
    }
    
    loadConfig();
}

void CredentialStore::loadConfig() {
    std::ifstream config_file(config_path);
    if (!config_file.is_open()) {
        // Print error code
        std::cout << "Error opening file: " << std::strerror(errno) << " (code: " << errno << ")" << std::endl;
        throw std::runtime_error("Failed to open config file: " + config_path);
    }
    
    try {
        config_file >> config;
    } catch (const json::parse_error& e) {
        throw std::runtime_error("Failed to parse config file: " + std::string(e.what()));
    }
}
} // namespace trade_ngin