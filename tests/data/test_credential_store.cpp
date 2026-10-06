#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "trade_ngin/data/credential_store.hpp"

#include <cstdlib>
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

// ===== folded in from tests/data/test_credential_store_extended.cpp =====
// Extended branch coverage for credential_store.cpp. Targets:
// - store_credential / get_credential / has_credential / remove_credential
// - save_config round-trip
// - validate_names rejection paths (bad section / bad key)
// - validate_credential pattern rejection and length cap
// - encrypted store/get round-trip via TRADING_ENCRYPTION_KEY
// - Conversion errors in get<T>
//
// Each test uses a per-test temp file so they don't share state.
namespace credential_store_extended_detail {

namespace fs = std::filesystem;
using namespace trade_ngin;

namespace {

void write_minimal_config(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << R"({"existing": {"key": "value"}})";
}

}  // namespace

class CredentialStoreExtendedTest : public ::testing::Test {
protected:
    void SetUp() override {
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        path_ = fs::temp_directory_path() /
                ("creds_" + std::string(info->name()) + ".json");
        fs::remove(path_);
    }
    void TearDown() override {
        fs::remove(path_);
        fs::remove(path_.string() + ".key");
        unsetenv("TRADING_CONFIG_PATH");
        unsetenv("TRADING_ENCRYPTION_KEY");
    }
    fs::path path_;
};

// ===== store_credential and get_credential round-trip =====

TEST_F(CredentialStoreExtendedTest, StoreAndGetCredentialRoundTrip) {
    write_minimal_config(path_);
    CredentialStore cs(path_.string());
    auto store = cs.store_credential("section1", "username", "alice123",
                                     /*encrypt=*/false);
    ASSERT_TRUE(store.is_ok()) << (store.error() ? store.error()->what() : "");
    auto got = cs.get_credential("section1", "username");
    ASSERT_TRUE(got.is_ok());
    EXPECT_EQ(got.value(), "alice123");
}

TEST_F(CredentialStoreExtendedTest, HasCredentialReflectsStoreThenRemove) {
    write_minimal_config(path_);
    CredentialStore cs(path_.string());
    EXPECT_FALSE(cs.has_credential("api", "url"));
    ASSERT_TRUE(cs.store_credential("api", "url", "https://x.example.com:443/p", false).is_ok());
    EXPECT_TRUE(cs.has_credential("api", "url"));
    ASSERT_TRUE(cs.remove_credential("api", "url").is_ok());
    EXPECT_FALSE(cs.has_credential("api", "url"));
}

TEST_F(CredentialStoreExtendedTest, RemoveCredentialOnMissingErrors) {
    write_minimal_config(path_);
    CredentialStore cs(path_.string());
    auto r = cs.remove_credential("missing", "nope");
    EXPECT_TRUE(r.is_error());
}

TEST_F(CredentialStoreExtendedTest, GetCredentialOnMissingErrors) {
    write_minimal_config(path_);
    CredentialStore cs(path_.string());
    auto r = cs.get_credential("missing", "nope");
    EXPECT_TRUE(r.is_error());
}

// ===== validate_names — rejected via store/get paths =====

TEST_F(CredentialStoreExtendedTest, StoreRejectsInvalidSectionName) {
    write_minimal_config(path_);
    CredentialStore cs(path_.string());
    auto r = cs.store_credential("bad section!", "key", "value", false);
    EXPECT_TRUE(r.is_error());
}

TEST_F(CredentialStoreExtendedTest, StoreRejectsInvalidKeyName) {
    write_minimal_config(path_);
    CredentialStore cs(path_.string());
    auto r = cs.store_credential("good", "bad key!", "value", false);
    EXPECT_TRUE(r.is_error());
}

TEST_F(CredentialStoreExtendedTest, HasCredentialReturnsFalseForInvalidNames) {
    write_minimal_config(path_);
    CredentialStore cs(path_.string());
    EXPECT_FALSE(cs.has_credential("bad section!", "key"));
}

TEST_F(CredentialStoreExtendedTest, RemoveRejectsInvalidNames) {
    write_minimal_config(path_);
    CredentialStore cs(path_.string());
    EXPECT_TRUE(cs.remove_credential("bad!", "key").is_error());
}

// ===== validate_credential rejects pattern mismatches =====

TEST_F(CredentialStoreExtendedTest, StoreRejectsHostWithIllegalCharacters) {
    write_minimal_config(path_);
    CredentialStore cs(path_.string());
    auto r = cs.store_credential("db", "host", "host with spaces", false);
    EXPECT_TRUE(r.is_error());
}

TEST_F(CredentialStoreExtendedTest, StoreRejectsPasswordTooShort) {
    write_minimal_config(path_);
    CredentialStore cs(path_.string());
    auto r = cs.store_credential("db", "password", "short", false);
    EXPECT_TRUE(r.is_error());
}

TEST_F(CredentialStoreExtendedTest, StoreAcceptsValidPort) {
    write_minimal_config(path_);
    CredentialStore cs(path_.string());
    EXPECT_TRUE(cs.store_credential("db", "port", "5432", false).is_ok());
}

TEST_F(CredentialStoreExtendedTest, StoreRejectsValueTooLong) {
    write_minimal_config(path_);
    CredentialStore cs(path_.string());
    // Use an unknown-key (no specific pattern) so only the length check applies.
    std::string huge(600, 'x');
    auto r = cs.store_credential("section", "freeform", huge, false);
    EXPECT_TRUE(r.is_error());
}

// ===== save_config round-trip =====

TEST_F(CredentialStoreExtendedTest, SaveAndReloadPreservesCredentials) {
    write_minimal_config(path_);
    {
        CredentialStore cs(path_.string());
        ASSERT_TRUE(cs.store_credential("db", "host", "h.example.com", false).is_ok());
        ASSERT_TRUE(cs.save_config().is_ok());
    }
    CredentialStore reloaded(path_.string());
    auto r = reloaded.get_credential("db", "host");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), "h.example.com");
}

TEST_F(CredentialStoreExtendedTest, SaveToUnwritableDirectoryReturnsError) {
    write_minimal_config(path_);
    CredentialStore cs(path_.string());
    // Move the config_path_ to something we can't write — but the API doesn't
    // expose mutation of config_path_, so emulate by removing the path's
    // parent directory after construction. save_config opens the existing
    // path; we can simulate by deleting the file and dir.
    fs::remove_all(path_.parent_path() / "definitely_not_a_dir");
    // Instead, point load_config at a path without a writable parent. The
    // direct pathway: invoke save_config when path_ refers to a directory.
    fs::remove(path_);
    fs::create_directory(path_);  // path_ now refers to a directory
    auto r = cs.save_config();
    EXPECT_TRUE(r.is_error());
    fs::remove_all(path_);
}

// ===== Encrypted store/get round-trip =====

TEST_F(CredentialStoreExtendedTest, EncryptedStoreRoundTripWithEnvKey) {
    setenv("TRADING_ENCRYPTION_KEY",
           "0123456789abcdef0123456789abcdef0123", 1);  // > 32 bytes
    write_minimal_config(path_);
    CredentialStore cs(path_.string(), /*use_encryption=*/true);
    ASSERT_TRUE(cs.store_credential("db", "username", "alice123", true).is_ok());
    auto got = cs.get_credential("db", "username");
    ASSERT_TRUE(got.is_ok());
    EXPECT_EQ(got.value(), "alice123");
}

TEST_F(CredentialStoreExtendedTest, EncryptionDisablesWhenKeyMissing) {
    // No env key, no key file → encryption disabled but constructor succeeds.
    write_minimal_config(path_);
    EXPECT_NO_THROW(CredentialStore cs(path_.string(), /*use_encryption=*/true));
}

TEST_F(CredentialStoreExtendedTest, EncryptionKeyFromKeyFileWorks) {
    write_minimal_config(path_);
    {
        std::ofstream key_file(path_.string() + ".key", std::ios::binary);
        key_file << "0123456789abcdef0123456789abcdef0123456789";  // > 32 bytes
    }
    EXPECT_NO_THROW(CredentialStore cs(path_.string(), /*use_encryption=*/true));
}

// ===== Templated get<T> conversion error =====

TEST_F(CredentialStoreExtendedTest, GetWithWrongTypeReturnsConversionError) {
    fs::create_directories(path_.parent_path());
    {
        std::ofstream f(path_);
        f << R"({"section": {"key": "not_a_number"}})";
    }
    CredentialStore cs(path_.string());
    auto r = cs.get<int>("section", "key");
    EXPECT_TRUE(r.is_error());
}

TEST_F(CredentialStoreExtendedTest, GetWithDefaultReturnsDefaultOnMissing) {
    write_minimal_config(path_);
    CredentialStore cs(path_.string());
    EXPECT_EQ(cs.get_with_default<int>("nosuch", "nokey", 99), 99);
}

// ===== Environment override on TRADING_CONFIG_PATH =====

TEST_F(CredentialStoreExtendedTest, EnvVarPathOverrideIgnoresNonJsonExtension) {
    write_minimal_config(path_);
    auto bogus = path_.parent_path() / "creds_env.txt";
    {
        std::ofstream f(bogus);
        f << R"({"x": {"y": "z"}})";
    }
    setenv("TRADING_CONFIG_PATH", bogus.c_str(), 1);
    // Override is ignored because extension != .json. Falls back to passed path.
    CredentialStore cs(path_.string());
    auto r = cs.get_credential("existing", "key");
    EXPECT_TRUE(r.is_ok());
    fs::remove(bogus);
}

}  // namespace credential_store_extended_detail
