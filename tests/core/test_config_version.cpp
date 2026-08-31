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

// ===== folded in from tests/core/test_config_version_extended.cpp =====
// Extended branch coverage for config_version.cpp. Targets:
// - ConfigVersion::to_string and from_string failure modes
// - get_latest_version with no migrations registered
// - needs_migration when no version field
// - create_migration_plan: no-op (from==to), invalid (to<from), no path
// - auto_migrate: no version → seed; already at latest; downgrade
// - validate_migration_step null function
// - execute_migration migration that errors
namespace config_version_extended_detail {

using namespace trade_ngin;

class ConfigVersionExtendedTest : public ::testing::Test {
protected:
    void SetUp() override {
        ConfigVersionManager::reset_instance();
    }
};

// ===== ConfigVersion =====

TEST_F(ConfigVersionExtendedTest, ConfigVersionToStringMatchesDottedFormat) {
    EXPECT_EQ((ConfigVersion{1, 2, 3}).to_string(), "1.2.3");
    EXPECT_EQ((ConfigVersion{0, 0, 0}).to_string(), "0.0.0");
}

TEST_F(ConfigVersionExtendedTest, ConfigVersionFromStringRejectsTwoComponents) {
    EXPECT_THROW(ConfigVersion::from_string("1.2"), std::runtime_error);
}

TEST_F(ConfigVersionExtendedTest, ConfigVersionFromStringRejectsEmpty) {
    EXPECT_THROW(ConfigVersion::from_string(""), std::runtime_error);
}

TEST_F(ConfigVersionExtendedTest, ConfigVersionRoundTripPreservesValue) {
    auto v = ConfigVersion::from_string("3.14.159");
    EXPECT_EQ(v.major, 3);
    EXPECT_EQ(v.minor, 14);
    EXPECT_EQ(v.patch, 159);
}

TEST_F(ConfigVersionExtendedTest, ConfigVersionEqualityAndOrdering) {
    EXPECT_TRUE((ConfigVersion{1, 0, 0}) == (ConfigVersion{1, 0, 0}));
    EXPECT_FALSE((ConfigVersion{1, 0, 0}) == (ConfigVersion{1, 0, 1}));
    EXPECT_TRUE((ConfigVersion{1, 0, 0}) < (ConfigVersion{1, 0, 1}));
    EXPECT_TRUE((ConfigVersion{0, 9, 0}) < (ConfigVersion{1, 0, 0}));
}

// ===== Manager basic queries =====

TEST_F(ConfigVersionExtendedTest, GetLatestVersionDefaultIsOneZeroZero) {
    auto v = ConfigVersionManager::instance().get_latest_version(ConfigType::STRATEGY);
    EXPECT_EQ(v.to_string(), "1.0.0");
}

TEST_F(ConfigVersionExtendedTest, NeedsMigrationFalseWhenVersionFieldMissing) {
    nlohmann::json cfg = {{"foo", "bar"}};
    EXPECT_FALSE(
        ConfigVersionManager::instance().needs_migration(cfg, ConfigType::STRATEGY));
}

TEST_F(ConfigVersionExtendedTest, NeedsMigrationFalseWhenVersionNotString) {
    nlohmann::json cfg = {{"version", 1.0}};
    EXPECT_FALSE(
        ConfigVersionManager::instance().needs_migration(cfg, ConfigType::STRATEGY));
}

// ===== create_migration_plan =====

TEST_F(ConfigVersionExtendedTest, CreateMigrationPlanFromEqualsTargetIsNoOp) {
    auto plan = ConfigVersionManager::instance().create_migration_plan({1, 0, 0}, {1, 0, 0});
    ASSERT_TRUE(plan.is_ok());
    EXPECT_EQ(plan.value().steps.size(), 0u);
}

TEST_F(ConfigVersionExtendedTest, CreateMigrationPlanRejectsDowngrade) {
    auto plan = ConfigVersionManager::instance().create_migration_plan({2, 0, 0}, {1, 0, 0});
    EXPECT_TRUE(plan.is_error());
}

TEST_F(ConfigVersionExtendedTest, CreateMigrationPlanFailsWhenNoPathRegistered) {
    auto plan = ConfigVersionManager::instance().create_migration_plan({1, 0, 0}, {2, 0, 0});
    EXPECT_TRUE(plan.is_error());
}

TEST_F(ConfigVersionExtendedTest, CreateMigrationPlanUsesDirectMigration) {
    auto m = [](const nlohmann::json& c) -> Result<nlohmann::json> {
        return Result<nlohmann::json>(c);
    };
    ConfigVersionManager::instance().register_migration({1, 0, 0}, {2, 0, 0}, m, "direct");
    auto plan = ConfigVersionManager::instance().create_migration_plan({1, 0, 0}, {2, 0, 0});
    ASSERT_TRUE(plan.is_ok());
    EXPECT_EQ(plan.value().steps.size(), 1u);
}

TEST_F(ConfigVersionExtendedTest, CreateMigrationPlanChainsThroughIntermediateVersions) {
    auto m = [](const nlohmann::json& c) -> Result<nlohmann::json> {
        return Result<nlohmann::json>(c);
    };
    ConfigVersionManager::instance().register_migration({1, 0, 0}, {1, 1, 0}, m, "step1");
    ConfigVersionManager::instance().register_migration({1, 1, 0}, {1, 2, 0}, m, "step2");
    auto plan = ConfigVersionManager::instance().create_migration_plan({1, 0, 0}, {1, 2, 0});
    ASSERT_TRUE(plan.is_ok());
    EXPECT_EQ(plan.value().steps.size(), 2u);
}

// ===== auto_migrate =====

TEST_F(ConfigVersionExtendedTest, AutoMigrateSeedsVersionFieldWhenMissing) {
    nlohmann::json cfg = {{"foo", "bar"}};
    auto r = ConfigVersionManager::instance().auto_migrate(cfg, ConfigType::STRATEGY);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().success);
    EXPECT_EQ(cfg["version"], "1.0.0");
}

TEST_F(ConfigVersionExtendedTest, AutoMigrateNoOpWhenAlreadyAtLatest) {
    nlohmann::json cfg = {{"version", "1.0.0"}};
    auto r = ConfigVersionManager::instance().auto_migrate(cfg, ConfigType::STRATEGY);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().success);
    EXPECT_EQ(r.value().changes.size(), 0u);
}

// ===== register_migration validation =====

TEST_F(ConfigVersionExtendedTest, RegisterMigrationRejectsToLessThanFrom) {
    auto m = [](const nlohmann::json& c) -> Result<nlohmann::json> {
        return Result<nlohmann::json>(c);
    };
    auto r =
        ConfigVersionManager::instance().register_migration({2, 0, 0}, {1, 0, 0}, m, "downgrade");
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConfigVersionExtendedTest, RegisterMigrationRejectsNullFunction) {
    auto r = ConfigVersionManager::instance().register_migration(
        {1, 0, 0}, {2, 0, 0}, MigrationFunction{}, "null");
    EXPECT_TRUE(r.is_error());
}

// ===== execute_migration error path =====

TEST_F(ConfigVersionExtendedTest, ExecuteMigrationPropagatesStepError) {
    auto failing = [](const nlohmann::json&) -> Result<nlohmann::json> {
        return make_error<nlohmann::json>(ErrorCode::UNKNOWN_ERROR, "fail", "test");
    };
    ConfigVersionManager::instance().register_migration(
        {1, 0, 0}, {2, 0, 0}, failing, "fails");

    nlohmann::json cfg = {{"version", "1.0.0"}};
    auto plan = ConfigVersionManager::instance().create_migration_plan({1, 0, 0}, {2, 0, 0});
    ASSERT_TRUE(plan.is_ok());

    auto r = ConfigVersionManager::instance().execute_migration(cfg, plan.value());
    EXPECT_TRUE(r.is_error());
}

}  // namespace config_version_extended_detail
