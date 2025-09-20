// test_config_base.cpp
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/strategy/types.hpp"  // For StrategyConfig

using namespace trade_ngin;

class ConfigBaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create temp test directory
        test_dir = std::filesystem::temp_directory_path() / "config_base_test";
        std::filesystem::create_directories(test_dir);
    }

    void TearDown() override {
        // Clean up
        std::filesystem::remove_all(test_dir);
    }

    std::filesystem::path test_dir;
};

// Test concrete implementation of ConfigBase
class TestConfig : public ConfigBase {
public:
    std::string name = "default";
    int value = 42;
    double ratio = 0.5;

    nlohmann::json to_json() const override {
        nlohmann::json j;
        j["name"] = name;
        j["value"] = value;
        j["ratio"] = ratio;
        return j;
    }

    void from_json(const nlohmann::json& j) override {
        if (j.contains("name"))
            name = j["name"].get<std::string>();
        if (j.contains("value"))
            value = j["value"].get<int>();
        if (j.contains("ratio"))
            ratio = j["ratio"].get<double>();
    }
};

TEST_F(ConfigBaseTest, SaveAndLoadFile) {
    TestConfig config;
    config.name = "test";
    config.value = 100;
    config.ratio = 1.5;

    std::filesystem::path file_path = test_dir / "test_config.json";

    // Save to file
    auto save_result = config.save_to_file(file_path.string());
    ASSERT_TRUE(save_result.is_ok())
        << "Failed to save config: "
        << (save_result.error() ? save_result.error()->what() : "unknown error");

    // Verify file exists
    ASSERT_TRUE(std::filesystem::exists(file_path));

    // Load into new config
    TestConfig loaded_config;
    auto load_result = loaded_config.load_from_file(file_path.string());
    ASSERT_TRUE(load_result.is_ok())
        << "Failed to load config: "
        << (load_result.error() ? load_result.error()->what() : "unknown error");

    // Verify values loaded correctly
    EXPECT_EQ(loaded_config.name, "test");
    EXPECT_EQ(loaded_config.value, 100);
    EXPECT_DOUBLE_EQ(loaded_config.ratio, 1.5);
}

TEST_F(ConfigBaseTest, DefaultValuesPreserved) {
    TestConfig config;  // Has default values

    // Create partial JSON with only some fields
    nlohmann::json partial;
    partial["name"] = "partial";
    // Leave out "value" and "ratio"

    // Load partial JSON
    config.from_json(partial);

    // Expect specified field to change
    EXPECT_EQ(config.name, "partial");

    // Expect defaults to be preserved for unspecified fields
    EXPECT_EQ(config.value, 42);          // Default value
    EXPECT_DOUBLE_EQ(config.ratio, 0.5);  // Default value
}

TEST_F(ConfigBaseTest, InvalidJsonHandling) {
    TestConfig config;

    // Create invalid JSON file
    std::filesystem::path file_path = test_dir / "invalid.json";
    std::ofstream file(file_path);
    file << "{ this is not valid JSON }";
    file.close();

    // Try to load invalid JSON
    auto result = config.load_from_file(file_path.string());
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::UNKNOWN_ERROR);
}

// Test with actual StrategyConfig
TEST_F(ConfigBaseTest, StrategyConfigSerialization) {
    // Only run this test if StrategyConfig inherits from ConfigBase
    if (std::is_base_of<ConfigBase, StrategyConfig>::value) {
        StrategyConfig config;
        config.capital_allocation = 1000000.0;
        config.max_leverage = 2.5;

        // Convert to JSON and back
        nlohmann::json j = config.to_json();

        StrategyConfig new_config;
        new_config.from_json(j);

        // Verify values
        EXPECT_DOUBLE_EQ(new_config.capital_allocation, 1000000.0);
        EXPECT_DOUBLE_EQ(new_config.max_leverage, 2.5);
    } else {
        GTEST_SKIP() << "StrategyConfig doesn't inherit from ConfigBase yet";
    }
}
