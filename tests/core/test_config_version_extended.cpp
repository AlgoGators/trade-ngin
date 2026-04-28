// Extended branch coverage for config_version.cpp. Targets:
// - ConfigVersion::to_string and from_string failure modes
// - get_latest_version with no migrations registered
// - needs_migration when no version field
// - create_migration_plan: no-op (from==to), invalid (to<from), no path
// - auto_migrate: no version → seed; already at latest; downgrade
// - validate_migration_step null function
// - execute_migration migration that errors

#include <gtest/gtest.h>
#include "trade_ngin/core/config_version.hpp"

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
