#include "trade_ngin/data/credential_store.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <fstream>
#include <filesystem>

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
    EXPECT_EQ(credentials.get<std::string>("database", "host"), "test-host.example.com");
    EXPECT_EQ(credentials.get<int>("database", "port"), 5432);
    EXPECT_EQ(credentials.get<std::string>("database", "username"), "test_user");
    EXPECT_EQ(credentials.get<std::string>("database", "password"), "test_password");
}

TEST_F(CredentialStoreTest, HandlesNumericValues) {
    CredentialStore credentials("test_config.json");
    
    EXPECT_EQ(credentials.get<int>("database", "port"), 5432);
    EXPECT_DOUBLE_EQ(credentials.get<double>("trading", "risk_limit"), 0.05);
    EXPECT_EQ(credentials.get<int>("trading", "max_position"), 1000000);
}

TEST_F(CredentialStoreTest, ThrowsOnMissingFile) {
    EXPECT_THROW(CredentialStore credentials("nonexistent_file.json"), std::runtime_error);
}

TEST_F(CredentialStoreTest, ThrowsOnMissingSection) {
    CredentialStore credentials("test_config.json");
    
    EXPECT_THROW(credentials.get<std::string>("nonexistent_section", "key"), std::runtime_error);
}

TEST_F(CredentialStoreTest, ThrowsOnMissingKey) {
    CredentialStore credentials("test_config.json");
    
    EXPECT_THROW(credentials.get<std::string>("database", "nonexistent_key"), std::runtime_error);
}

TEST_F(CredentialStoreTest, HandlesDefaultValues) {
    CredentialStore credentials("test_config.json");
    
    // Test with existing keys
    EXPECT_EQ(credentials.getWithDefault<std::string>("database", "host", "default-host"), "test-host.example.com");
    EXPECT_DOUBLE_EQ(credentials.getWithDefault<double>("trading", "risk_limit", 0.1), 0.05);
    
    // Test with non-existent keys
    EXPECT_EQ(credentials.getWithDefault<std::string>("database", "nonexistent_key", "default_value"), "default_value");
    EXPECT_DOUBLE_EQ(credentials.getWithDefault<double>("trading", "nonexistent_key", 0.1), 0.1);
    EXPECT_EQ(credentials.getWithDefault<int>("nonexistent_section", "key", 42), 42);
}

TEST_F(CredentialStoreTest, HandlesEmptySection) {
    CredentialStore credentials("test_config.json");
    
    EXPECT_THROW(credentials.get<std::string>("empty_section", "any_key"), std::runtime_error);
    EXPECT_EQ(credentials.getWithDefault<std::string>("empty_section", "any_key", "default"), "default");
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
    EXPECT_EQ(credentials.get<std::string>("database", "host"), "env-host.example.com");
    EXPECT_EQ(credentials.get<int>("database", "port"), 1234);
    
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
    EXPECT_EQ(credentials.get<std::string>("database", "host"), "test-host.example.com");
    
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
    credentials.loadConfig();
    EXPECT_EQ(credentials.get<std::string>("database", "host"), "updated-host.example.com");
}