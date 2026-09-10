#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "trade_ngin/core/config_manager.hpp"

#include <nlohmann/json.hpp>
#include "test_base.hpp"
#include "trade_ngin/core/config_manager.hpp"
#include "trade_ngin/core/config_version.hpp"

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

TEST_F(ConfigManagerTest, InvalidConfigDirectory) {
    // /dev/null is a character device, so create_directories() under it fails
    // with ENOTDIR on Linux/macOS even when running as root.
    std::filesystem::path invalid_path = "/dev/null/cannot_create_here";

    auto& config_manager = ConfigManager::instance();
    auto result = config_manager.initialize(invalid_path);

    // Verify initialization fails
    EXPECT_TRUE(result.is_error());
}

// ===== folded in from tests/core/test_config_manager_extended.cpp =====
// Extended branch coverage for config_manager.cpp. Targets the four
// ConfigValidator subclasses, environment string round-trip, and
// create_default_config for each ConfigType.
namespace config_manager_extended_detail {

using namespace trade_ngin;
using namespace trade_ngin::testing;

class ConfigValidatorTest : public TestBase {};

// ===== StrategyValidator =====

TEST_F(ConfigValidatorTest, StrategyValidatorRejectsMissingCapitalAllocation) {
    StrategyValidator v;
    nlohmann::json cfg = {{"max_leverage", 2.0}};
    auto errs = v.validate(cfg);
    bool found = false;
    for (auto& e : errs) if (e.field == "capital_allocation") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, StrategyValidatorRejectsNonPositiveCapitalAllocation) {
    StrategyValidator v;
    auto errs = v.validate({{"capital_allocation", -1.0}, {"max_leverage", 2.0}});
    bool found = false;
    for (auto& e : errs) if (e.field == "capital_allocation") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, StrategyValidatorRejectsMissingMaxLeverage) {
    StrategyValidator v;
    auto errs = v.validate({{"capital_allocation", 1000.0}});
    bool found = false;
    for (auto& e : errs) if (e.field == "max_leverage") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, StrategyValidatorRejectsNonObjectPositionLimits) {
    StrategyValidator v;
    auto errs = v.validate({
        {"capital_allocation", 1000.0},
        {"max_leverage", 2.0},
        {"position_limits", "not an object"},
    });
    bool found = false;
    for (auto& e : errs) if (e.field == "position_limits") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, StrategyValidatorRejectsMaxDrawdownOutOfRange) {
    StrategyValidator v;
    auto errs = v.validate({
        {"capital_allocation", 1000.0},
        {"max_leverage", 2.0},
        {"max_drawdown", 1.5},  // > 1.0
    });
    bool found = false;
    for (auto& e : errs) if (e.field == "max_drawdown") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, StrategyValidatorRejectsNonArrayEmaWindows) {
    StrategyValidator v;
    auto errs = v.validate({
        {"capital_allocation", 1000.0},
        {"max_leverage", 2.0},
        {"ema_windows", "not an array"},
    });
    bool found = false;
    for (auto& e : errs) if (e.field == "ema_windows") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, StrategyValidatorRejectsShortGreaterThanLongWindow) {
    StrategyValidator v;
    auto errs = v.validate({
        {"capital_allocation", 1000.0},
        {"max_leverage", 2.0},
        {"ema_windows", nlohmann::json::array({nlohmann::json::array({16, 8})})},
    });
    bool found = false;
    for (auto& e : errs) if (e.field == "ema_windows") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, StrategyValidatorRejectsWindowOutOfRange) {
    StrategyValidator v;
    auto errs = v.validate({
        {"capital_allocation", 1000.0},
        {"max_leverage", 2.0},
        {"ema_windows", nlohmann::json::array({nlohmann::json::array({0, 8})})},
    });
    bool found = false;
    for (auto& e : errs) if (e.field == "ema_windows") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, StrategyValidatorAcceptsValidConfig) {
    StrategyValidator v;
    auto errs = v.validate({
        {"capital_allocation", 1000.0},
        {"max_leverage", 2.0},
        {"max_drawdown", 0.2},
        {"ema_windows", nlohmann::json::array({nlohmann::json::array({2, 8}),
                                                nlohmann::json::array({16, 64})})},
    });
    EXPECT_TRUE(errs.empty()) << (errs.empty() ? "" : errs[0].field + ": " + errs[0].message);
}

TEST_F(ConfigValidatorTest, StrategyValidatorTypeIsStrategy) {
    StrategyValidator v;
    EXPECT_EQ(v.get_type(), ConfigType::STRATEGY);
}

// ===== RiskValidator =====

TEST_F(ConfigValidatorTest, RiskValidatorRejectsConfidenceLevelOutOfRange) {
    RiskValidator v;
    auto errs = v.validate({
        {"confidence_level", 1.5},
        {"lookback_period", 252},
        {"capital", 1'000'000.0},
    });
    bool found = false;
    for (auto& e : errs) if (e.field == "confidence_level") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, RiskValidatorRejectsNonPositiveLookbackPeriod) {
    RiskValidator v;
    auto errs = v.validate({
        {"confidence_level", 0.99},
        {"lookback_period", 0},
        {"capital", 1'000'000.0},
    });
    bool found = false;
    for (auto& e : errs) if (e.field == "lookback_period") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, RiskValidatorRejectsNonPositiveCapital) {
    RiskValidator v;
    auto errs = v.validate({
        {"confidence_level", 0.99},
        {"lookback_period", 252},
        {"capital", -1.0},
    });
    bool found = false;
    for (auto& e : errs) if (e.field == "capital") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, RiskValidatorRejectsMaxCorrelationOutOfRange) {
    RiskValidator v;
    auto errs = v.validate({
        {"confidence_level", 0.99},
        {"lookback_period", 252},
        {"capital", 1'000'000.0},
        {"max_correlation", 1.5},  // > 1.0
    });
    bool found = false;
    for (auto& e : errs) if (e.field == "max_correlation") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, RiskValidatorAcceptsValidConfig) {
    RiskValidator v;
    auto errs = v.validate({
        {"confidence_level", 0.99},
        {"lookback_period", 252},
        {"capital", 1'000'000.0},
        {"portfolio_var_limit", 0.15},
        {"max_drawdown", 0.2},
        {"max_correlation", 0.7},
        {"max_net_leverage", 2.0},
    });
    EXPECT_TRUE(errs.empty());
}

TEST_F(ConfigValidatorTest, RiskValidatorTypeIsRisk) {
    RiskValidator v;
    EXPECT_EQ(v.get_type(), ConfigType::RISK);
}

TEST_F(ConfigValidatorTest, RiskValidatorRejectsMissingFields) {
    RiskValidator v;
    auto errs = v.validate(nlohmann::json::object());
    bool found_conf = false, found_lb = false, found_cap = false;
    for (auto& e : errs) {
        if (e.field == "confidence_level") found_conf = true;
        if (e.field == "lookback_period") found_lb = true;
        if (e.field == "capital") found_cap = true;
    }
    EXPECT_TRUE(found_conf);
    EXPECT_TRUE(found_lb);
    EXPECT_TRUE(found_cap);
}

TEST_F(ConfigValidatorTest, RiskValidatorRejectsNonNumericFields) {
    RiskValidator v;
    auto errs = v.validate({
        {"confidence_level", "not a number"},
        {"lookback_period", "not a number"},
        {"capital", "not a number"},
    });
    EXPECT_GE(errs.size(), 3u);
}

TEST_F(ConfigValidatorTest, RiskValidatorRejectsZeroOrNegativeConfidenceLevel) {
    RiskValidator v;
    auto errs = v.validate({
        {"confidence_level", 0.0},  // == 0 is rejected
        {"lookback_period", 252},
        {"capital", 1'000'000.0},
    });
    bool found = false;
    for (auto& e : errs) if (e.field == "confidence_level") found = true;
    EXPECT_TRUE(found);
}

// ===== ExecutionValidator =====

TEST_F(ConfigValidatorTest, ExecutionValidatorEmptyConfigIsValid) {
    ExecutionValidator v;
    auto errs = v.validate(nlohmann::json::object());
    EXPECT_TRUE(errs.empty());
}

TEST_F(ConfigValidatorTest, ExecutionValidatorRejectsSlippageModelMissingType) {
    ExecutionValidator v;
    auto errs = v.validate({{"slippage_model", nlohmann::json::object()}});
    bool found = false;
    for (auto& e : errs) if (e.field == "slippage_model") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, ExecutionValidatorRejectsVolumeBasedSlippageMissingFields) {
    ExecutionValidator v;
    auto errs = v.validate({
        {"slippage_model", {{"type", "volume_based"}}},
    });
    EXPECT_FALSE(errs.empty());
}

TEST_F(ConfigValidatorTest, ExecutionValidatorAcceptsValidVolumeBasedSlippage) {
    ExecutionValidator v;
    auto errs = v.validate({
        {"slippage_model", {{"type", "volume_based"},
                             {"price_impact_coefficient", 1e-3},
                             {"min_volume_ratio", 0.05}}},
    });
    EXPECT_TRUE(errs.empty());
}

TEST_F(ConfigValidatorTest, ExecutionValidatorRejectsCommissionModelNegativeBaseRate) {
    ExecutionValidator v;
    auto errs = v.validate({
        {"commission_model", {{"base_rate", -0.001}}},
    });
    bool found = false;
    for (auto& e : errs) if (e.field == "base_rate") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, ExecutionValidatorRejectsCommissionNegativeMinAndClearingFee) {
    ExecutionValidator v;
    auto errs = v.validate({
        {"commission_model", {{"base_rate", 0.001},
                                {"min_commission", -1.0},
                                {"clearing_fee", -0.5}}},
    });
    bool found_min = false, found_cf = false;
    for (auto& e : errs) {
        if (e.field == "min_commission") found_min = true;
        if (e.field == "clearing_fee") found_cf = true;
    }
    EXPECT_TRUE(found_min);
    EXPECT_TRUE(found_cf);
}

TEST_F(ConfigValidatorTest, ExecutionValidatorTypeIsExecution) {
    ExecutionValidator v;
    EXPECT_EQ(v.get_type(), ConfigType::EXECUTION);
}

// ===== DatabaseValidator =====

TEST_F(ConfigValidatorTest, DatabaseValidatorRejectsAllRequiredFieldsMissing) {
    DatabaseValidator v;
    auto errs = v.validate(nlohmann::json::object());
    // Should report errors for host, port, database, user (4 fields)
    EXPECT_GE(errs.size(), 4u);
}

TEST_F(ConfigValidatorTest, DatabaseValidatorRejectsEmptyStringFields) {
    DatabaseValidator v;
    auto errs = v.validate({
        {"host", ""},
        {"port", 5432},
        {"database", ""},
        {"user", ""},
    });
    EXPECT_GE(errs.size(), 3u);  // host, database, user empty
}

TEST_F(ConfigValidatorTest, DatabaseValidatorRejectsPortOutOfRange) {
    DatabaseValidator v;
    auto errs = v.validate({
        {"host", "h"}, {"database", "d"}, {"user", "u"},
        {"port", 99999},  // > 65535
    });
    bool found = false;
    for (auto& e : errs) if (e.field == "port") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, DatabaseValidatorRejectsNonPositiveCacheSize) {
    DatabaseValidator v;
    auto errs = v.validate({
        {"host", "h"}, {"port", 5432}, {"database", "d"}, {"user", "u"},
        {"cache_size", 0},
    });
    bool found = false;
    for (auto& e : errs) if (e.field == "cache_size") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, DatabaseValidatorRejectsPrefetchDaysOutOfRange) {
    DatabaseValidator v;
    auto errs = v.validate({
        {"host", "h"}, {"port", 5432}, {"database", "d"}, {"user", "u"},
        {"prefetch_days", 31},  // > 30
    });
    bool found = false;
    for (auto& e : errs) if (e.field == "prefetch_days") found = true;
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidatorTest, DatabaseValidatorAcceptsValidConfig) {
    DatabaseValidator v;
    auto errs = v.validate({
        {"host", "localhost"}, {"port", 5432}, {"database", "mydb"}, {"user", "u"},
        {"cache_size", 100}, {"prefetch_days", 7},
    });
    EXPECT_TRUE(errs.empty());
}

TEST_F(ConfigValidatorTest, DatabaseValidatorTypeIsDatabase) {
    DatabaseValidator v;
    EXPECT_EQ(v.get_type(), ConfigType::DATABASE);
}

// ===== Environment string round-trip =====

class ConfigManagerStaticTest : public TestBase {};

TEST_F(ConfigManagerStaticTest, EnvironmentToStringForAllValues) {
    EXPECT_EQ(ConfigManager::environment_to_string(Environment::DEVELOPMENT), "development");
    EXPECT_EQ(ConfigManager::environment_to_string(Environment::STAGING), "staging");
    EXPECT_EQ(ConfigManager::environment_to_string(Environment::PRODUCTION), "production");
    EXPECT_EQ(ConfigManager::environment_to_string(Environment::BACKTEST), "backtest");
}

TEST_F(ConfigManagerStaticTest, StringToEnvironmentForAllValues) {
    EXPECT_EQ(ConfigManager::string_to_environment("development"), Environment::DEVELOPMENT);
    EXPECT_EQ(ConfigManager::string_to_environment("staging"), Environment::STAGING);
    EXPECT_EQ(ConfigManager::string_to_environment("production"), Environment::PRODUCTION);
    EXPECT_EQ(ConfigManager::string_to_environment("backtest"), Environment::BACKTEST);
}

TEST_F(ConfigManagerStaticTest, StringToEnvironmentUnknownDefaultsToDevelopment) {
    EXPECT_EQ(ConfigManager::string_to_environment("garbage"), Environment::DEVELOPMENT);
}

TEST_F(ConfigManagerStaticTest, EnvironmentRoundTripPreservesValue) {
    for (Environment e : {Environment::DEVELOPMENT, Environment::STAGING,
                           Environment::PRODUCTION, Environment::BACKTEST}) {
        EXPECT_EQ(ConfigManager::string_to_environment(
                      ConfigManager::environment_to_string(e)),
                  e);
    }
}

// ===== create_default_config =====

class ConfigManagerInstanceTest : public TestBase {};

TEST_F(ConfigManagerInstanceTest, CreateDefaultConfigForStrategyHasRequiredFields) {
    auto j = ConfigManager::instance().create_default_config(ConfigType::STRATEGY);
    EXPECT_TRUE(j.contains("capital_allocation"));
    EXPECT_TRUE(j.contains("max_leverage"));
}

TEST_F(ConfigManagerInstanceTest, CreateDefaultConfigForRiskHasRequiredFields) {
    auto j = ConfigManager::instance().create_default_config(ConfigType::RISK);
    EXPECT_TRUE(j.contains("confidence_level"));
    EXPECT_TRUE(j.contains("lookback_period"));
    EXPECT_TRUE(j.contains("capital"));
}

TEST_F(ConfigManagerInstanceTest, CreateDefaultConfigForExecutionIsObject) {
    auto j = ConfigManager::instance().create_default_config(ConfigType::EXECUTION);
    EXPECT_TRUE(j.is_object());
}

// CFG-seed-invalid-data-json: the seeded database config must satisfy the
// validator that will read it back, so the test asserts against the validator
// rather than against a hand-copied key list that can drift from it the same way
// the seed did.
TEST_F(ConfigManagerInstanceTest, CreateDefaultConfigForDatabaseSatisfiesItsOwnValidator) {
    auto j = ConfigManager::instance().create_default_config(ConfigType::DATABASE);

    DatabaseValidator validator;
    auto errors = validator.validate(j);
    std::string joined;
    for (const auto& e : errors) joined += e.field + ": " + e.message + "; ";
    EXPECT_TRUE(errors.empty())
        << "the seeded data.json would be rejected on the next start: " << joined;

    // port is a NUMBER here. This is ConfigManager's schema; ConfigLoader's
    // unrelated DatabaseConfig::port is a std::string, and a swap in either
    // direction breaks the other loader at startup.
    EXPECT_TRUE(j["port"].is_number());
    EXPECT_TRUE(j.contains("max_connections"));
    EXPECT_TRUE(j.contains("timeout_seconds"));
    EXPECT_FALSE(j.contains("connection_string"))
        << "a seeded file carrying both a DSN and the parts has two sources of truth";
}

TEST_F(ConfigManagerInstanceTest, CreateDefaultConfigForLoggingIsObject) {
    auto j = ConfigManager::instance().create_default_config(ConfigType::LOGGING);
    EXPECT_TRUE(j.is_object());
}

TEST_F(ConfigManagerInstanceTest, IsProductionReflectsCurrentEnvironment) {
    // Default is DEVELOPMENT — never production unless someone called initialize.
    auto& mgr = ConfigManager::instance();
    if (mgr.get_environment() != Environment::PRODUCTION) {
        EXPECT_FALSE(mgr.is_production());
    }
}

// ===== StrategyValidator validate_numeric_range (private; reach via #define private public) =====
//
// (Done via the StrategyValidator subclass test; the validate_numeric_range
// helper is private. We exercise it indirectly through validate_ema_windows
// path coverage above.)

// ===== initialize / save_configs file IO =====

class ConfigManagerInitTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        // Other test suites mutate the ConfigVersionManager singleton; reset
        // it here so auto_migrate during load_config_files sees a clean slate.
        ConfigVersionManager::reset_instance();
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = std::filesystem::temp_directory_path() /
               ("trade_ngin_cfg_mgr_" + std::string(info->name()));
        std::filesystem::remove_all(dir_);
    }
    void TearDown() override {
        std::filesystem::remove_all(dir_);
        ConfigVersionManager::reset_instance();
        TestBase::TearDown();
    }
    std::filesystem::path dir_;
};

// TEST-config-manager-seed-branch (2026-09-09).
//
// The FIXME that stood here said the seed-defaults branch could not be tested:
// initialize() took a lock_guard on a non-recursive mutex and then reached
// save_configs(), which took the same mutex, so a non-existent config_path
// deadlocked. That was true when it was written and is not any more -- mutex_ is
// a std::recursive_mutex (C-16, fixed on main in May 2026 by ca8a3f9b), so the
// re-entrant take is legal and the branch runs.
//
// The branch had therefore never been exercised, on a path that runs exactly
// once per deployment: the first start against a fresh config directory. It
// creates the directory, seeds five default component configs and writes them to
// disk. If it were broken, the failure would land on a brand-new environment and
// nowhere else.
//
// The three tests below cover it: that it completes at all (the deadlock is
// gone), that it writes every component file with valid parseable content, and
// that a second initialize() reads back what the first one seeded rather than
// re-seeding over it.

TEST_F(ConfigManagerInitTest, InitializeSeedsDefaultsWhenTheDirectoryDoesNotExist) {
    ASSERT_FALSE(std::filesystem::exists(dir_)) << "the fixture must start with no directory";

    auto& mgr = ConfigManager::instance();
    auto r = mgr.initialize(dir_, Environment::DEVELOPMENT);

    // Reaching this line at all is half the test: before ca8a3f9b it deadlocked
    // here and the suite hung rather than failed.
    ASSERT_TRUE(r.is_ok()) << (r.error() ? r.error()->what() : "no error");
    EXPECT_TRUE(std::filesystem::exists(dir_))
        << "the seed branch did not create the config directory";
    EXPECT_EQ(mgr.get_environment(), Environment::DEVELOPMENT);
}

TEST_F(ConfigManagerInitTest, SeededDefaultsAreWrittenForEveryComponent) {
    auto& mgr = ConfigManager::instance();
    ASSERT_TRUE(mgr.initialize(dir_, Environment::DEVELOPMENT).is_ok());

    // The five components load_config_files() iterates, by the names
    // get_component_name() gives them. "data" rather than "database" is
    // deliberate and is what the loader looks for on the next start.
    for (const char* component : {"strategy", "risk", "execution", "data", "logging"}) {
        const auto file = dir_ / (std::string(component) + ".json");
        ASSERT_TRUE(std::filesystem::exists(file))
            << "the seed branch did not write " << component << ".json, so the next start "
               "would find a directory that exists but has nothing in it";
        std::ifstream in(file);
        nlohmann::json parsed;
        ASSERT_NO_THROW(in >> parsed)
            << component << ".json is not parseable JSON";
        EXPECT_TRUE(parsed.is_object()) << component << ".json is not a JSON object";
    }
}

// CFG-seed-invalid-data-json, FIXED. This replaces the test T-1 left here, which
// pinned the defect as it stood and was written to fail the day it was fixed.
//
// The defect: create_default_database_config() wrote connection_string /
// max_connections / timeout_seconds while DatabaseValidator requires host, port,
// database and user. The seed path returns save_configs() directly and never
// validates what it just wrote; the load path validates everything it reads. So a
// fresh deployment started once and then failed on every subsequent start with
// "Configuration validation failed for data: - host: Required field missing".
//
// The assertion that matters is the ROUND TRIP -- seed, then start again over
// what was seeded -- because that is the sequence a new deployment performs and
// the only one in which the two halves of the contract meet. Asserting the key
// list instead would be a second hand-written copy of the validator, free to
// drift from it exactly as the seed did.
TEST_F(ConfigManagerInitTest, SeededDefaultsLoadCleanlyOnTheNextStart) {
    auto& mgr = ConfigManager::instance();
    ASSERT_TRUE(mgr.initialize(dir_, Environment::DEVELOPMENT).is_ok())
        << "the first start, which seeds, must succeed";

    ConfigVersionManager::reset_instance();
    auto second = mgr.initialize(dir_, Environment::DEVELOPMENT);

    ASSERT_TRUE(second.is_ok())
        << "a second start over the seeded directory was rejected: "
        << (second.error() ? second.error()->what() : "no error");

    // And the file on disk is the one that was validated, not an in-memory
    // default that happens to be valid: read it back and check the four keys the
    // validator requires are actually there.
    std::ifstream in(dir_ / "data.json");
    nlohmann::json parsed;
    ASSERT_NO_THROW(in >> parsed) << "data.json is not parseable JSON";
    for (const char* field : {"host", "port", "database", "user"}) {
        EXPECT_TRUE(parsed.contains(field))
            << "seeded data.json is missing " << field << ", which the validator requires";
    }
}

TEST_F(ConfigManagerInitTest, InitializeReReadsExistingFiles) {
    std::filesystem::create_directories(dir_);
    nlohmann::json risk = {{"confidence_level", 0.95}, {"lookback_period", 100},
                           {"capital", 500'000.0}, {"version", "1.0.0"}};
    std::ofstream(dir_ / "risk.json") << risk.dump(2);
    nlohmann::json strat = {{"capital_allocation", 250'000.0}, {"max_leverage", 2.0},
                            {"version", "1.0.0"}};
    std::ofstream(dir_ / "strategy.json") << strat.dump(2);

    auto& mgr = ConfigManager::instance();
    auto r = mgr.initialize(dir_, Environment::BACKTEST);
    ASSERT_TRUE(r.is_ok()) << (r.error() ? r.error()->what() : "no error");
    EXPECT_EQ(mgr.get_environment(), Environment::BACKTEST);
}

TEST_F(ConfigManagerInitTest, InitializeRejectsMalformedConfigFile) {
    std::filesystem::create_directories(dir_);
    // Write garbage that contains valid filename so the load path is taken.
    std::ofstream(dir_ / "strategy.json") << "{ this is not valid json";

    auto& mgr = ConfigManager::instance();
    auto r = mgr.initialize(dir_, Environment::DEVELOPMENT);
    EXPECT_TRUE(r.is_error());
}

}  // namespace config_manager_extended_detail
