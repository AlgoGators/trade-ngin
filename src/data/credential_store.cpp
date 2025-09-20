#include "trade_ngin/data/credential_store.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <regex>
#include <sstream>

namespace trade_ngin {

CredentialStore::CredentialStore(const std::string& path, bool use_encryption)
    : config_path_(path), use_encryption_(use_encryption) {
    // Check for environment variable to override path
    const char* env_config = std::getenv("TRADING_CONFIG_PATH");
    if (env_config) {
        // Validate the environment path
        std::filesystem::path env_path(env_config);
        if (env_path.filename().extension() == ".json" && env_path.string().length() < 512) {
            config_path_ = env_config;
        }
    }

    init_validation_patterns();

    if (use_encryption_) {
        auto key_result = get_encryption_key();
        if (key_result.is_error()) {
            // For now, disable encryption if key not available
            use_encryption_ = false;
            std::cerr << "Warning: Encryption disabled - " << key_result.error()->what()
                      << std::endl;
        } else {
            encryption_key_ = key_result.value();
        }
    }

    auto load_result = load_config();
    if (load_result.is_error()) {
        throw std::runtime_error("Failed to load config: " +
                                 std::string(load_result.error()->what()));
    }
}

void CredentialStore::init_validation_patterns() {
    // Database connection patterns
    validation_patterns_["host"] = R"(^[a-zA-Z0-9.-]+$)";
    validation_patterns_["port"] = R"(^[1-9][0-9]{0,4}$)";
    validation_patterns_["username"] = R"(^[a-zA-Z0-9_-]{1,64}$)";
    validation_patterns_["password"] = R"(^[a-zA-Z0-9!@#$%^&*()_+=-]{8,128}$)";
    validation_patterns_["name"] = R"(^[a-zA-Z0-9_-]{1,64}$)";

    // API key patterns
    validation_patterns_["api_key"] = R"(^[a-zA-Z0-9]{32,256}$)";
    validation_patterns_["secret_key"] = R"(^[a-zA-Z0-9+/=]{40,512}$)";

    // URL patterns
    validation_patterns_["url"] = R"(^https?://[a-zA-Z0-9.-]+:[0-9]{1,5}(/.*)?$)";
}

Result<void> CredentialStore::load_config() {
    if (!std::filesystem::exists(config_path_)) {
        return make_error<void>(ErrorCode::FILE_NOT_FOUND, "Config file not found: " + config_path_,
                                "CredentialStore");
    }

    // Check file permissions (should not be world-readable) - simplified check
    try {
        auto perms = std::filesystem::status(config_path_).permissions();
        if ((perms & std::filesystem::perms::others_read) != std::filesystem::perms::none) {
            std::cerr << "Warning: Config file has potentially unsafe permissions: " << config_path_
                      << std::endl;
        }
    } catch (const std::exception& e) {
        // If permission check fails, just warn and continue
        std::cerr << "Warning: Could not check file permissions: " << e.what() << std::endl;
    }

    std::ifstream config_file(config_path_);
    if (!config_file.is_open()) {
        return make_error<void>(ErrorCode::FILE_IO_ERROR,
                                "Failed to open config file: " + config_path_, "CredentialStore");
    }

    try {
        config_file >> config_;
    } catch (const json::parse_error& e) {
        return make_error<void>(ErrorCode::JSON_PARSE_ERROR,
                                "Failed to parse config file: " + std::string(e.what()),
                                "CredentialStore");
    }

    return Result<void>();
}

Result<std::string> CredentialStore::get_encryption_key() const {
    // Try environment variable first
    const char* env_key = std::getenv("TRADING_ENCRYPTION_KEY");
    if (env_key) {
        std::string key(env_key);
        if (key.length() >= 32) {  // Minimum 256-bit key
            return Result<std::string>(key.substr(0, 32));
        }
    }

    // Try key file
    std::string key_file_path = config_path_ + ".key";
    if (std::filesystem::exists(key_file_path)) {
        std::ifstream key_file(key_file_path, std::ios::binary);
        if (key_file.is_open()) {
            std::string key((std::istreambuf_iterator<char>(key_file)),
                            std::istreambuf_iterator<char>());
            if (key.length() >= 32) {
                return Result<std::string>(key.substr(0, 32));
            }
        }
    }

    return make_error<std::string>(ErrorCode::ENCRYPTION_ERROR,
                                   "No valid encryption key found. Set TRADING_ENCRYPTION_KEY "
                                   "environment variable or create key file",
                                   "CredentialStore");
}

std::string CredentialStore::encrypt_string(const std::string& plaintext) const {
    if (!use_encryption_) {
        return plaintext;
    }

    // Simple XOR "encryption" for now (NOT secure, just for compilation)
    // TODO: Replace with proper encryption when OpenSSL is available
    std::string result = plaintext;
    for (size_t i = 0; i < result.length(); ++i) {
        result[i] ^= encryption_key_[i % encryption_key_.length()];
    }

    // Simple base64-like encoding
    std::string encoded;
    for (unsigned char c : result) {
        encoded += std::to_string(static_cast<int>(c)) + ";";
    }

    return encoded;
}

std::string CredentialStore::decrypt_string(const std::string& ciphertext) const {
    if (!use_encryption_) {
        return ciphertext;
    }

    // Decode simple encoding
    std::string decoded;
    std::stringstream ss(ciphertext);
    std::string token;

    while (std::getline(ss, token, ';')) {
        if (!token.empty()) {
            try {
                int val = std::stoi(token);
                decoded += static_cast<char>(val);
            } catch (const std::exception&) {
                throw std::runtime_error("Invalid encrypted data format");
            }
        }
    }

    // Simple XOR "decryption"
    for (size_t i = 0; i < decoded.length(); ++i) {
        decoded[i] ^= encryption_key_[i % encryption_key_.length()];
    }

    return decoded;
}

Result<void> CredentialStore::validate_credential(const std::string& key,
                                                  const std::string& value) const {
    auto it = validation_patterns_.find(key);
    if (it != validation_patterns_.end()) {
        std::regex pattern(it->second);
        if (!std::regex_match(value, pattern)) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "Invalid format for credential: " + key, "CredentialStore");
        }
    }

    // Additional length validation
    if (value.length() > 512) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Credential value too long: " + key,
                                "CredentialStore");
    }

    return Result<void>();
}

Result<void> CredentialStore::validate_names(const std::string& section,
                                             const std::string& key) const {
    // Validate section name
    std::regex section_pattern(R"(^[a-zA-Z0-9_]{1,64}$)");
    if (!std::regex_match(section, section_pattern)) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Invalid section name: " + section,
                                "CredentialStore");
    }

    // Validate key name
    std::regex key_pattern(R"(^[a-zA-Z0-9_]{1,64}$)");
    if (!std::regex_match(key, key_pattern)) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Invalid key name: " + key,
                                "CredentialStore");
    }

    return Result<void>();
}

Result<void> CredentialStore::store_credential(const std::string& section, const std::string& key,
                                               const std::string& value, bool encrypt) {
    auto name_validation = validate_names(section, key);
    if (name_validation.is_error()) {
        return name_validation;
    }

    auto value_validation = validate_credential(key, value);
    if (value_validation.is_error()) {
        return value_validation;
    }

    try {
        std::string stored_value = (encrypt && use_encryption_) ? encrypt_string(value) : value;
        config_[section][key] = stored_value;

        if (encrypt && use_encryption_) {
            // Mark as encrypted
            config_[section][key + "_encrypted"] = true;
        }

        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::ENCRYPTION_ERROR,
                                "Failed to store credential: " + std::string(e.what()),
                                "CredentialStore");
    }
}

Result<std::string> CredentialStore::get_credential(const std::string& section,
                                                    const std::string& key) const {
    auto name_validation = validate_names(section, key);
    if (name_validation.is_error()) {
        return make_error<std::string>(name_validation.error()->code(),
                                       name_validation.error()->what());
    }

    if (!config_.contains(section) || !config_[section].contains(key)) {
        return make_error<std::string>(ErrorCode::INVALID_ARGUMENT,
                                       "Credential not found: " + section + "." + key,
                                       "CredentialStore");
    }

    try {
        std::string value = config_[section][key].get<std::string>();

        // Check if encrypted
        if (config_[section].contains(key + "_encrypted") &&
            config_[section][key + "_encrypted"].get<bool>()) {
            return Result<std::string>(decrypt_string(value));
        }

        return Result<std::string>(value);
    } catch (const std::exception& e) {
        return make_error<std::string>(ErrorCode::DECRYPTION_ERROR,
                                       "Failed to get credential: " + std::string(e.what()),
                                       "CredentialStore");
    }
}

bool CredentialStore::has_credential(const std::string& section, const std::string& key) const {
    auto name_validation = validate_names(section, key);
    if (name_validation.is_error()) {
        return false;
    }

    return config_.contains(section) && config_[section].contains(key);
}

Result<void> CredentialStore::remove_credential(const std::string& section,
                                                const std::string& key) {
    auto name_validation = validate_names(section, key);
    if (name_validation.is_error()) {
        return name_validation;
    }

    if (!config_.contains(section) || !config_[section].contains(key)) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Credential not found: " + section + "." + key, "CredentialStore");
    }

    config_[section].erase(key);
    config_[section].erase(key + "_encrypted");

    if (config_[section].empty()) {
        config_.erase(section);
    }

    return Result<void>();
}

Result<void> CredentialStore::save_config() const {
    try {
        std::ofstream config_file(config_path_);
        if (!config_file.is_open()) {
            return make_error<void>(ErrorCode::FILE_IO_ERROR,
                                    "Failed to open config file for writing: " + config_path_,
                                    "CredentialStore");
        }

        config_file << std::setw(4) << config_ << std::endl;

        // Try to set secure file permissions (may fail on some systems)
        try {
            std::filesystem::permissions(config_path_, std::filesystem::perms::owner_read |
                                                           std::filesystem::perms::owner_write);
        } catch (const std::exception& e) {
            std::cerr << "Warning: Could not set secure file permissions: " << e.what()
                      << std::endl;
        }

        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::FILE_IO_ERROR,
                                "Failed to save config: " + std::string(e.what()),
                                "CredentialStore");
    }
}

}  // namespace trade_ngin
