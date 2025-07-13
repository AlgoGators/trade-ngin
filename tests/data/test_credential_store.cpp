#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "trade_ngin/data/credential_store.hpp"

namespace fs = std::filesystem;
using namespace trade_ngin;

class CredentialStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a test JSON file
        std::ofstream test_config("test_config.json");
        test_config << R"({
            "database": {
                "host": "test-host.example.com",
                "port": 5432,
                "username": "test_user",
                "password": "test_password",
                "name": "test_db"
            },
            "api": {
                "key": "test_api_key",
                "secret": "test_api_secret"
            },
            "trading": {
                "risk_limit": 0.05,
                "max_position": 1000000
            },
            "empty_section": {}
        })";
        test_config.close();
    }

    void TearDown() override {
        // Clean up test file
        if (fs::exists("test_config.json")) {
            fs::remove("test_config.json");
        }
    }
};

TEST_F(CredentialStoreTest, LoadsConfigurationSuccessfully) {
    CredentialStore credentials("test_config.json");

    // Test basic retrieval
    auto host_result = credentials.get<std::string>("database", "host");
    EXPECT_TRUE(host_result.is_ok());
    EXPECT_EQ(host_result.value(), "test-host.example.com");

    auto port_result = credentials.get<int>("database", "port");
    EXPECT_TRUE(port_result.is_ok());
    EXPECT_EQ(port_result.value(), 5432);

    auto username_result = credentials.get<std::string>("database", "username");
    EXPECT_TRUE(username_result.is_ok());
    EXPECT_EQ(username_result.value(), "test_user");

    auto password_result = credentials.get<std::string>("database", "password");
    EXPECT_TRUE(password_result.is_ok());
    EXPECT_EQ(password_result.value(), "test_password");
}

TEST_F(CredentialStoreTest, HandlesNumericValues) {
    CredentialStore credentials("test_config.json");

    auto port_result = credentials.get<int>("database", "port");
    EXPECT_TRUE(port_result.is_ok());
    EXPECT_EQ(port_result.value(), 5432);

    auto risk_result = credentials.get<double>("trading", "risk_limit");
    EXPECT_TRUE(risk_result.is_ok());
    EXPECT_DOUBLE_EQ(risk_result.value(), 0.05);

    auto position_result = credentials.get<int>("trading", "max_position");
    EXPECT_TRUE(position_result.is_ok());
    EXPECT_EQ(position_result.value(), 1000000);
}

TEST_F(CredentialStoreTest, ThrowsOnMissingFile) {
    EXPECT_THROW(CredentialStore credentials("nonexistent_file.json"), std::runtime_error);
}

TEST_F(CredentialStoreTest, ThrowsOnMissingSection) {
    CredentialStore credentials("test_config.json");

    auto result = credentials.get<std::string>("nonexistent_section", "key");
    EXPECT_TRUE(result.is_error());
}

TEST_F(CredentialStoreTest, ThrowsOnMissingKey) {
    CredentialStore credentials("test_config.json");

    auto result = credentials.get<std::string>("database", "nonexistent_key");
    EXPECT_TRUE(result.is_error());
}

TEST_F(CredentialStoreTest, HandlesDefaultValues) {
    CredentialStore credentials("test_config.json");

    // Test with existing keys
    EXPECT_EQ(credentials.get_with_default<std::string>("database", "host", "default-host"),
              "test-host.example.com");
    EXPECT_DOUBLE_EQ(credentials.get_with_default<double>("trading", "risk_limit", 0.1), 0.05);

    // Test with non-existent keys
    EXPECT_EQ(
        credentials.get_with_default<std::string>("database", "nonexistent_key", "default_value"),
        "default_value");
    EXPECT_DOUBLE_EQ(credentials.get_with_default<double>("trading", "nonexistent_key", 0.1), 0.1);
    EXPECT_EQ(credentials.get_with_default<int>("nonexistent_section", "key", 42), 42);
}

TEST_F(CredentialStoreTest, HandlesEmptySection) {
    CredentialStore credentials("test_config.json");

    auto result = credentials.get<std::string>("empty_section", "any_key");
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(credentials.get_with_default<std::string>("empty_section", "any_key", "default"),
              "default");
}

// Test with environment variable override
TEST_F(CredentialStoreTest, RespectsEnvironmentVariableOverride) {
    // First, create a different config file
    std::ofstream env_config("env_config.json");
    env_config << R"({
        "database": {
            "host": "env-host.example.com",
            "port": 1234
        }
    })";
    env_config.close();

// Set environment variable
#ifdef _WIN32
    _putenv_s("TRADING_CONFIG_PATH", "env_config.json");
#else
    setenv("TRADING_CONFIG_PATH", "env_config.json", 1);
#endif

    // Instantiate with a different default path, should use env path instead
    CredentialStore credentials("test_config.json");

    // Check that it loaded from the env config
    auto host_result = credentials.get<std::string>("database", "host");
    EXPECT_TRUE(host_result.is_ok());
    EXPECT_EQ(host_result.value(), "env-host.example.com");

    auto port_result = credentials.get<int>("database", "port");
    EXPECT_TRUE(port_result.is_ok());
    EXPECT_EQ(port_result.value(), 1234);

// Clean up
#ifdef _WIN32
    _putenv_s("TRADING_CONFIG_PATH", "");
#else
    unsetenv("TRADING_CONFIG_PATH");
#endif

    if (fs::exists("env_config.json")) {
        fs::remove("env_config.json");
    }
}

// Test corrupted JSON
TEST_F(CredentialStoreTest, ThrowsOnInvalidJson) {
    // Create a corrupt JSON file
    std::ofstream corrupt_config("corrupt_config.json");
    corrupt_config << R"({
        "database": {
            "host": "corrupt-host.example.com",
            "port": 
        }
    })";
    corrupt_config.close();

    EXPECT_THROW(CredentialStore credentials("corrupt_config.json"), std::runtime_error);

    // Clean up
    if (fs::exists("corrupt_config.json")) {
        fs::remove("corrupt_config.json");
    }
}

// Test reloading configuration
TEST_F(CredentialStoreTest, SuccessfullyReloadsConfiguration) {
    CredentialStore credentials("test_config.json");

    // Check initial value
    auto host_result = credentials.get<std::string>("database", "host");
    EXPECT_TRUE(host_result.is_ok());
    EXPECT_EQ(host_result.value(), "test-host.example.com");

    // Update the config file
    std::ofstream updated_config("test_config.json");
    updated_config << R"({
        "database": {
            "host": "updated-host.example.com",
            "port": 5432,
            "username": "test_user",
            "password": "test_password"
        }
    })";
    updated_config.close();

    // Reload and check updated value
    credentials.load_config();
    auto updated_host_result = credentials.get<std::string>("database", "host");
    EXPECT_TRUE(updated_host_result.is_ok());
    EXPECT_EQ(updated_host_result.value(), "updated-host.example.com");
}