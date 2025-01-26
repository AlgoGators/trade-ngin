#include <gtest/gtest.h>
#include "trade_ngin/core/config_manager.hpp"
#include <filesystem>
#include <fstream>

using namespace trade_ngin;

class ConfigManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create temporary test directory
        test_config_dir = std::filesystem::temp_directory_path() / "config_test";
        std::filesystem::create_directories(test_config_dir);
        
        // Create base config file
        std::ofstream base_config(test_config_dir / "base.json");
        base_config << R"({
            "strategy": {
                "risk_target": 0.2,
                "idm": 2.5,
                "ema_windows": [[2, 8], [4, 16], [8, 32]],
                "vol_lookback_short": 22,
                "vol_lookback_long": 252
            },
            "risk": {
                "portfolio_var_limit": 0.15,
                "max_drawdown": 0.20,
                "max_correlation": 0.7,
                "max_leverage": 4.0
            }
        })";
        base_config.close();
    }

    void TearDown() override {
        // Clean up test directory
        std::filesystem::remove_all(test_config_dir);
    }

    std::filesystem::path test_config_dir;
};

TEST_F(ConfigManagerTest, InitializeSuccess) {
    auto& config_manager = ConfigManager::instance();
    auto result = config_manager.initialize(test_config_dir);
    EXPECT_TRUE(result.is_ok());
}

TEST_F(ConfigManagerTest, GetStrategyConfig) {
    auto& config_manager = ConfigManager::instance();
    config_manager.initialize(test_config_dir);

    auto result = config_manager.get_config<nlohmann::json>(ConfigType::STRATEGY);
    ASSERT_TRUE(result.is_ok());
    
    const auto& config = result.value();
    EXPECT_DOUBLE_EQ(config["risk_target"], 0.2);
    EXPECT_DOUBLE_EQ(config["idm"], 2.5);
    EXPECT_EQ(config["vol_lookback_short"], 22);
}

TEST_F(ConfigManagerTest, EnvironmentOverrides) {
    // Create prod config with overrides
    std::ofstream prod_config(test_config_dir / "production.json");
    prod_config << R"({
        "strategy": {
            "risk_target": 0.1,
            "idm": 2.0
        }
    })";
    prod_config.close();

    auto& config_manager = ConfigManager::instance();
    config_manager.initialize(test_config_dir, Environment::PRODUCTION);

    auto result = config_manager.get_config<nlohmann::json>(ConfigType::STRATEGY);
    ASSERT_TRUE(result.is_ok());
    
    const auto& config = result.value();
    EXPECT_DOUBLE_EQ(config["risk_target"], 0.1); // Overridden value
    EXPECT_DOUBLE_EQ(config["idm"], 2.0);         // Overridden value
    EXPECT_EQ(config["vol_lookback_short"], 22);  // Original value
}

TEST_F(ConfigManagerTest, ValidationFailure) {
    // Create invalid config
    std::ofstream invalid_config(test_config_dir / "base.json");
    invalid_config << R"({
        "strategy": {
            "risk_target": -0.1,  // Invalid negative value
            "idm": 0.0            // Invalid zero value
        }
    })";
    invalid_config.close();

    auto& config_manager = ConfigManager::instance();
    auto init_result = config_manager.initialize(test_config_dir);
    
    // EXPECT initialization to FAIL
    EXPECT_TRUE(init_result.is_error());  // Changed from is_ok()
    EXPECT_EQ(init_result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(ConfigManagerTest, UpdateConfig) {
    auto& config_manager = ConfigManager::instance();
    config_manager.initialize(test_config_dir);

    nlohmann::json new_config = {
        {"risk_target", 0.15},
        {"idm", 3.0},
        {"vol_lookback_short", 44}
    };

    auto update_result = config_manager.update_config(ConfigType::STRATEGY, new_config);
    EXPECT_TRUE(update_result.is_ok());

    auto get_result = config_manager.get_config<nlohmann::json>(ConfigType::STRATEGY);
    ASSERT_TRUE(get_result.is_ok());
    
    const auto& config = get_result.value();
    EXPECT_DOUBLE_EQ(config["risk_target"], 0.15);
    EXPECT_DOUBLE_EQ(config["idm"], 3.0);
    EXPECT_EQ(config["vol_lookback_short"], 44);
}

TEST_F(ConfigManagerTest, NonExistentComponent) {
    auto& config_manager = ConfigManager::instance();
    config_manager.initialize(test_config_dir);

    auto result = config_manager.get_config<nlohmann::json>(
        static_cast<ConfigType>(999)  // Invalid component type
    );
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::CONVERSION_ERROR);  // Changed code
}

TEST_F(ConfigManagerTest, InvalidConfigDirectory) {
    auto& config_manager = ConfigManager::instance();
    auto result = config_manager.initialize("/nonexistent/directory");
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}