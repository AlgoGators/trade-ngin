#include <gtest/gtest.h>
#include <string>
#include "trade_ngin/core/config_version.hpp"

using namespace trade_ngin;

class ConfigVersionManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset instance between tests
        ConfigVersionManager::reset_instance();
        manager = &ConfigVersionManager::instance();
    }

    ConfigVersionManager* manager;
};

TEST_F(ConfigVersionManagerTest, VersionParsing) {
    EXPECT_NO_THROW({
        auto version = ConfigVersion::from_string("1.2.3");
        EXPECT_EQ(version.major, 1);
        EXPECT_EQ(version.minor, 2);
        EXPECT_EQ(version.patch, 3);
    });

    EXPECT_THROW(ConfigVersion::from_string("invalid"), std::runtime_error);
}

TEST_F(ConfigVersionManagerTest, VersionComparison) {
    ConfigVersion v1{1, 0, 0};
    ConfigVersion v2{1, 1, 0};
    ConfigVersion v3{1, 1, 1};
    ConfigVersion v4{2, 0, 0};

    EXPECT_TRUE(v1 < v2);
    EXPECT_TRUE(v2 < v3);
    EXPECT_TRUE(v3 < v4);
    EXPECT_FALSE(v2 < v1);
    EXPECT_FALSE(v4 < v3);

    ConfigVersion same1{1, 1, 0};
    ConfigVersion same2{1, 1, 0};
    EXPECT_TRUE(same1 == same2);
    EXPECT_FALSE(same1 < same2);
}

TEST_F(ConfigVersionManagerTest, RegisterMigration) {
    ConfigVersion from{1, 0, 0};
    ConfigVersion to{1, 1, 0};

    auto migration = [](const nlohmann::json& config) -> Result<nlohmann::json> {
        nlohmann::json new_config = config;
        new_config["migrated"] = true;
        return Result<nlohmann::json>(new_config);
    };

    auto result = manager->register_migration(from, to, migration, "Test migration");
    EXPECT_TRUE(result.is_ok());
}

TEST_F(ConfigVersionManagerTest, InvalidMigrationRegistration) {
    ConfigVersion from{1, 0, 0};
    ConfigVersion to{1, 0, 0};  // Same version

    auto migration = [](const nlohmann::json& config) -> Result<nlohmann::json> {
        return Result<nlohmann::json>(config);
    };

    auto result = manager->register_migration(from, to, migration, "Invalid migration");
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(ConfigVersionManagerTest, CreateMigrationPlan) {
    // Register a chain of migrations
    ConfigVersion v1{1, 0, 0};
    ConfigVersion v2{1, 1, 0};
    ConfigVersion v3{1, 2, 0};

    auto migration = [](const nlohmann::json& config) -> Result<nlohmann::json> {
        return Result<nlohmann::json>(config);
    };

    manager->register_migration(v1, v2, migration, "Step 1");
    manager->register_migration(v2, v3, migration, "Step 2");

    auto plan_result = manager->create_migration_plan(v1, v3);
    ASSERT_TRUE(plan_result.is_ok());

    const auto& plan = plan_result.value();
    EXPECT_EQ(plan.steps.size(), 2);
    EXPECT_EQ(plan.start_version.to_string(), "1.0.0");
    EXPECT_EQ(plan.target_version.to_string(), "1.2.0");
}

TEST_F(ConfigVersionManagerTest, ExecuteMigration) {
    ConfigVersion v1{1, 0, 0};
    ConfigVersion v2{1, 1, 0};

    // Register migration that adds a field
    auto migration = [](const nlohmann::json& config) -> Result<nlohmann::json> {
        nlohmann::json new_config = config;
        new_config["new_field"] = "added";
        return Result<nlohmann::json>(new_config);
    };

    manager->register_migration(v1, v2, migration, "Add field");

    // Create initial config
    nlohmann::json config = {{"version", "1.0.0"}, {"existing_field", "value"}};

    auto plan_result = manager->create_migration_plan(v1, v2);
    ASSERT_TRUE(plan_result.is_ok());

    auto result = manager->execute_migration(config, plan_result.value());
    ASSERT_TRUE(result.is_ok());

    const auto& migrated = result.value();
    EXPECT_TRUE(migrated.success);
    EXPECT_EQ(migrated.original_version.to_string(), "1.0.0");
    EXPECT_EQ(migrated.final_version.to_string(), "1.1.0");
    EXPECT_EQ(config["new_field"], "added");
}

TEST_F(ConfigVersionManagerTest, AutoMigrate) {
    ConfigVersion v1{1, 0, 0};
    ConfigVersion v2{1, 1, 0};

    // Register migration
    auto migration = [](const nlohmann::json& config) -> Result<nlohmann::json> {
        nlohmann::json new_config = config;
        new_config["auto_migrated"] = true;
        return Result<nlohmann::json>(new_config);
    };

    manager->register_migration(v1, v2, migration, "Auto migration");

    // Create config needing migration
    nlohmann::json config = {{"version", "1.0.0"}, {"field", "value"}};

    auto result = manager->auto_migrate(config, ConfigType::STRATEGY);
    ASSERT_TRUE(result.is_ok());

    const auto& migrated = result.value();
    EXPECT_TRUE(migrated.success);
    EXPECT_TRUE(config.contains("auto_migrated"));
    ASSERT_TRUE(config["auto_migrated"].is_boolean());  // Ensure type is correct
}

TEST_F(ConfigVersionManagerTest, NeedsMigration) {
    nlohmann::json old_config = {{"version", "1.0.0"}};

    nlohmann::json current_config = {{"version", "1.1.0"}};

    // Set latest version to 1.1.0
    ConfigVersion v1{1, 0, 0};
    ConfigVersion v2{1, 1, 0};
    auto migration = [](const nlohmann::json& config) -> Result<nlohmann::json> {
        return Result<nlohmann::json>(config);
    };
    manager->register_migration(v1, v2, migration, "Test");

    EXPECT_TRUE(manager->needs_migration(old_config, ConfigType::STRATEGY));
    EXPECT_FALSE(manager->needs_migration(current_config, ConfigType::STRATEGY));
}
