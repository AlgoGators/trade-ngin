#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "trade_ngin/core/config_manager.hpp"

using namespace trade_ngin;

class ConfigManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create temporary test directory
        test_config_dir = std::filesystem::temp_directory_path() / "config_test";
        std::filesystem::create_directories(test_config_dir);

        // Create separate component config files
        std::ofstream strategy_config(test_config_dir / "strategy.json");
        strategy_config << R"({
        "capital_allocation": 1000000.0,
        "max_leverage": 3.0,
        "max_drawdown": 0.3,
        "var_limit": 0.1,
        "correlation_limit": 0.7,
        "risk_target": 0.2,
        "idm": 2.5,
        "ema_windows": [[2, 8], [4, 16], [8, 32]],
        "vol_lookback_short": 22,
        "vol_lookback_long": 252,
        "version": "1.0.0"
    })";
        strategy_config.close();

        std::ofstream risk_config(test_config_dir / "risk.json");
        risk_config << R"({
        "portfolio_var_limit": 0.15,
        "max_drawdown": 0.20,
        "max_correlation": 0.7,
        "max_gross_leverage": 4.0,
        "max_net_leverage": 2.0,
        "confidence_level": 0.99,
        "lookback_period": 252,
        "capital": 1000000.0,
        "version": "1.0.0"
    })";
        risk_config.close();
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
    // Create production environment directory
    std::filesystem::create_directories(test_config_dir / "production");

    // Create prod override with overrides
    std::ofstream prod_config(test_config_dir / "production" / "strategy.json");
    prod_config << R"({
        "risk_target": 0.1,
        "idm": 2.0
    })";
    prod_config.close();

    auto& config_manager = ConfigManager::instance();
    config_manager.initialize(test_config_dir, Environment::PRODUCTION);

    auto result = config_manager.get_config<nlohmann::json>(ConfigType::STRATEGY);
    ASSERT_TRUE(result.is_ok());

    const auto& config = result.value();
    EXPECT_DOUBLE_EQ(config["risk_target"], 0.1);  // Overridden value
    EXPECT_DOUBLE_EQ(config["idm"], 2.0);          // Overridden value
    EXPECT_EQ(config["vol_lookback_short"], 22);   // Original value
}

TEST_F(ConfigManagerTest, ValidationFailure) {
    // Create invalid strategy config with negative values
    std::filesystem::remove(test_config_dir / "strategy.json");
    std::ofstream invalid_config(test_config_dir / "strategy.json");
    invalid_config << R"({
        "capital_allocation": -1000.0,  // Invalid negative value
        "max_leverage": 0.0,            // Invalid zero value
        "version": "1.0.0"
    })";
    invalid_config.close();

    auto& config_manager = ConfigManager::instance();
    auto init_result = config_manager.initialize(test_config_dir);

    // Initialization should fail due to validation errors
    EXPECT_TRUE(init_result.is_error());
    EXPECT_EQ(init_result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(ConfigManagerTest, UpdateConfig) {
    auto& config_manager = ConfigManager::instance();
    config_manager.initialize(test_config_dir);

    nlohmann::json new_config = {{"capital_allocation", 2000000.0},
                                 {"max_leverage", 4.0},
                                 {"max_drawdown", 0.25},
                                 {"var_limit", 0.12},
                                 {"correlation_limit", 0.65},
                                 {"risk_target", 0.15},
                                 {"idm", 3.0},
                                 {"vol_lookback_short", 44},
                                 {"vol_lookback_long", 252},
                                 {"version", "1.0.0"}};

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
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

// TEST_F(ConfigManagerTest, InvalidConfigDirectory) {
//     // Create a path that cannot be created (using invalid characters in Windows)
//     std::filesystem::path invalid_path = "/\\?*:|<>\\invalid\\path";
//
//     auto& config_manager = ConfigManager::instance();
//     auto result = config_manager.initialize(invalid_path);
//
//     // Verify initialization fails
//     EXPECT_TRUE(result.is_error());
// }