// include/data/credential_store.hpp
#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/error.hpp"

using json = nlohmann::json;

namespace trade_ngin {

/**
 * @brief Secure credential store with encryption support
 * Handles encrypted storage of sensitive configuration data
 */
class CredentialStore {
private:
    json config_;
    std::string config_path_;
    std::string encryption_key_;
    bool use_encryption_;

    // Validation patterns for different types of credentials
    std::unordered_map<std::string, std::string> validation_patterns_;

    /**
     * @brief Initialize validation patterns
     */
    void init_validation_patterns();

    /**
     * @brief Encrypt a string value
     * @param plaintext The text to encrypt
     * @return Encrypted string (base64 encoded)
     */
    std::string encrypt_string(const std::string& plaintext) const;

    /**
     * @brief Decrypt a string value
     * @param ciphertext The encrypted text (base64 encoded)
     * @return Decrypted string
     */
    std::string decrypt_string(const std::string& ciphertext) const;

    /**
     * @brief Validate credential value against pattern
     * @param key The credential key
     * @param value The credential value
     * @return Result indicating if validation passed
     */
    Result<void> validate_credential(const std::string& key, const std::string& value) const;

    /**
     * @brief Get encryption key from environment or file
     * @return Result with encryption key
     */
    Result<std::string> get_encryption_key() const;

    /**
     * @brief Validate section and key names
     * @param section Section name
     * @param key Key name
     * @return Result indicating if names are valid
     */
    Result<void> validate_names(const std::string& section, const std::string& key) const;

public:
    /**
     * @brief Constructor with default config path
     * @param path Path to configuration file
     * @param use_encryption Whether to use encryption (default: false for now)
     */
    explicit CredentialStore(const std::string& path = "config.json", bool use_encryption = false);

    /**
     * @brief Load or reload configuration from file
     * @return Result indicating success or failure
     */
    Result<void> load_config();

    /**
     * @brief Store a credential securely
     * @param section Configuration section
     * @param key Configuration key
     * @param value Value to store
     * @param encrypt Whether this specific value should be encrypted
     * @return Result indicating success or failure
     */
    Result<void> store_credential(const std::string& section, const std::string& key,
                                  const std::string& value, bool encrypt = true);

    /**
     * @brief Get a credential value
     * @param section Configuration section
     * @param key Configuration key
     * @return Result with the decrypted value
     */
    Result<std::string> get_credential(const std::string& section, const std::string& key) const;

    /**
     * @brief Get a value from the config, throws if not found
     * @param section Configuration section
     * @param key Configuration key
     * @return The configuration value
     */
    template <typename T>
    Result<T> get(const std::string& section, const std::string& key) const;

    /**
     * @brief Get a value with default fallback
     * @param section Configuration section
     * @param key Configuration key
     * @param default_value Default value if not found
     * @return The configuration value or default
     */
    template <typename T>
    T get_with_default(const std::string& section, const std::string& key,
                       const T& default_value) const;

    /**
     * @brief Check if a credential exists
     * @param section Configuration section
     * @param key Configuration key
     * @return True if exists, false otherwise
     */
    bool has_credential(const std::string& section, const std::string& key) const;

    /**
     * @brief Remove a credential
     * @param section Configuration section
     * @param key Configuration key
     * @return Result indicating success or failure
     */
    Result<void> remove_credential(const std::string& section, const std::string& key);

    /**
     * @brief Save current configuration to file
     * @return Result indicating success or failure
     */
    Result<void> save_config() const;
};

// Template methods must be defined in the header
template <typename T>
Result<T> CredentialStore::get(const std::string& section, const std::string& key) const {
    auto name_validation = validate_names(section, key);
    if (name_validation.is_error()) {
        return make_error<T>(name_validation.error()->code(), name_validation.error()->what());
    }

    if (!config_.contains(section) || !config_[section].contains(key)) {
        return make_error<T>(ErrorCode::INVALID_ARGUMENT,
                             "Configuration not found: " + section + "." + key, "CredentialStore");
    }

    try {
        return Result<T>(config_[section][key].get<T>());
    } catch (const std::exception& e) {
        return make_error<T>(ErrorCode::CONVERSION_ERROR,
                             "Failed to convert configuration value: " + std::string(e.what()),
                             "CredentialStore");
    }
}

template <typename T>
T CredentialStore::get_with_default(const std::string& section, const std::string& key,
                                    const T& default_value) const {
    auto result = get<T>(section, key);
    return result.is_error() ? default_value : result.value();
}

}  // namespace trade_ngin
