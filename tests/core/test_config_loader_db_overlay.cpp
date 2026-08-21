// Tests for ConfigLoader database overlay, merge precedence, security enforcement, and manifest publishing.
// Uses temp test configs and validates database override behavior.

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "test_base.hpp"

#define private public
#include "trade_ngin/core/config_loader.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

nlohmann::json minimal_defaults() {
    return {
        {"database", {{"host", "default_host"}, {"port", "5432"}, {"username", "default_user"},
                      {"password", "default_pass"}, {"name", "default_db"}, {"num_connections", 5}}},
        {"execution", {{"commission_rate", 0.0005}, {"slippage_bps", 1.0},
                        {"position_limit_backtest", 1000.0}, {"position_limit_live", 500.0}}},
        {"optimization", {{"tau", 1.0}, {"capital", 500000.0},
                          {"cost_penalty_scalar", 50}, {"asymmetric_risk_buffer", 0.1},
                          {"max_iterations", 100}, {"convergence_threshold", 1e-6},
                          {"use_buffering", true}, {"buffer_size_factor", 0.05}}},
        {"backtest", {{"lookback_years", 2}, {"store_trade_details", true}}},
        {"live", {{"historical_days", 300}}},
        {"strategy_defaults", {{"max_strategy_allocation", 1.0},
                                {"min_strategy_allocation", 0.1},
                                {"use_optimization", true},
                                {"use_risk_management", true},
                                {"fdm", nlohmann::json::array({{1, 1.0}, {2, 1.03}})}}},
        {"risk_defaults", {{"confidence_level", 0.99}, {"lookback_period", 252},
                           {"max_correlation", 0.7}}},
    };
}

nlohmann::json minimal_portfolio() {
    return {
        {"portfolio_id", "TEST_PORTFOLIO"},
        {"initial_capital", 1'000'000.0},
        {"reserve_capital_pct", 0.10},
        {"max_drawdown", 0.4},
        {"max_leverage", 4.0},
        {"strategies", {{"TREND_FOLLOWING", {{"weight", 1.0}, {"allocation", 1.0}}}}},
    };
}

nlohmann::json minimal_risk() {
    return {
        {"var_limit", 0.15},
        {"jump_risk_limit", 0.10},
        {"max_gross_leverage", 4.0},
        {"max_net_leverage", 2.0},
    };
}

nlohmann::json minimal_email() {
    return {
        {"smtp_host", "smtp.test.com"},
        {"smtp_port", 587},
        {"username", "u"},
        {"password", "p"},
        {"from_email", "f@test.com"},
        {"to_emails", nlohmann::json::array({"a@test.com"})},
    };
}

void write_json(const std::filesystem::path& p, const nlohmann::json& j) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p);
    f << j.dump(2);
}

}  // namespace

class ConfigLoaderDBOverlayTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        base_ = std::filesystem::temp_directory_path() /
                ("trade_ngin_config_db_" + std::string(info->name()));
        std::filesystem::remove_all(base_);
        std::filesystem::create_directories(base_);

        // Create standard test config files.
        write_json(base_ / "defaults.json", minimal_defaults());
        std::filesystem::create_directories(base_ / "portfolios" / "test");
        write_json(base_ / "portfolios" / "test" / "portfolio.json", minimal_portfolio());
        write_json(base_ / "portfolios" / "test" / "risk.json", minimal_risk());
        write_json(base_ / "portfolios" / "test" / "email.json", minimal_email());
    }

    void TearDown() override {
        std::filesystem::remove_all(base_);
        TestBase::TearDown();
    }

    std::filesystem::path base_;
};

// TEST 1: File-only load still produces the same AppConfig (no behavior change when no DB).
TEST_F(ConfigLoaderDBOverlayTest, FileOnlyLoadUnchanged) {
    auto result = ConfigLoader::load(base_, "test", nullptr);
    ASSERT_FALSE(result.is_error()) << "File-only load failed: " << result.error()->what();

    AppConfig config = result.value();
    EXPECT_EQ(config.portfolio_id, "TEST_PORTFOLIO");
    EXPECT_EQ(config.initial_capital, 1'000'000.0);
    EXPECT_EQ(config.database.host, "default_host");
    EXPECT_EQ(config.database.password, "default_pass");
}

// TEST 2: Validate override rejects database.host.
TEST_F(ConfigLoaderDBOverlayTest, ValidateRejectsDatabaseHost) {
    nlohmann::json bad_override = {
        {"database", {{"host", "attacker.com"}}}
    };

    auto result = ConfigLoader::validate_override_no_credentials(bad_override);
    EXPECT_TRUE(result.is_error()) << "Should reject database.host";
    std::string err_msg = result.error()->what();
    EXPECT_TRUE(err_msg.find("database") != std::string::npos) << "Error should mention database";
}

// TEST 3: Validate override rejects database.password.
TEST_F(ConfigLoaderDBOverlayTest, ValidateRejectsDatabasePassword) {
    nlohmann::json bad_override = {
        {"database", {{"password", "hacked"}}}
    };

    auto result = ConfigLoader::validate_override_no_credentials(bad_override);
    EXPECT_TRUE(result.is_error()) << "Should reject database.password";
    std::string err_msg = result.error()->what();
    EXPECT_TRUE(err_msg.find("database") != std::string::npos) << "Error should mention database";
}

// TEST 4: Validate override rejects email.password.
TEST_F(ConfigLoaderDBOverlayTest, ValidateRejectsEmailPassword) {
    nlohmann::json bad_override = {
        {"email", {{"password", "hacked_email"}}}
    };

    auto result = ConfigLoader::validate_override_no_credentials(bad_override);
    EXPECT_TRUE(result.is_error()) << "Should reject email.password";
    std::string err_msg = result.error()->what();
    EXPECT_TRUE(err_msg.find("email.password") != std::string::npos) << "Error should mention email.password";
}

// TEST 5: Validate override accepts non-protected fields.
TEST_F(ConfigLoaderDBOverlayTest, ValidateAcceptsNonProtectedFields) {
    nlohmann::json good_override = {
        {"execution", {{"commission_rate", 0.001}}}
    };

    auto result = ConfigLoader::validate_override_no_credentials(good_override);
    EXPECT_FALSE(result.is_error()) << "Good override rejected: " << result.error()->what();
}

// TEST 6: strip_credentials_for_manifest removes database section.
TEST_F(ConfigLoaderDBOverlayTest, StripCredentialsRemovesDatabase) {
    auto file_result = ConfigLoader::load(base_, "test", nullptr);
    ASSERT_FALSE(file_result.is_error());
    AppConfig config = file_result.value();

    nlohmann::json manifest = ConfigLoader::strip_credentials_for_manifest(config);

    EXPECT_FALSE(manifest.contains("database")) << "Manifest still contains database section";
}

// TEST 7: strip_credentials_for_manifest removes email.password.
TEST_F(ConfigLoaderDBOverlayTest, StripCredentialsRemovesEmailPassword) {
    auto file_result = ConfigLoader::load(base_, "test", nullptr);
    ASSERT_FALSE(file_result.is_error());
    AppConfig config = file_result.value();

    nlohmann::json manifest = ConfigLoader::strip_credentials_for_manifest(config);

    if (manifest.contains("email")) {
        EXPECT_FALSE(manifest.at("email").contains("password"))
            << "Manifest email still contains password";
    }
}

// TEST 8: strip_credentials_for_manifest preserves other email fields.
TEST_F(ConfigLoaderDBOverlayTest, StripCredentialsPreservesEmailOtherFields) {
    auto file_result = ConfigLoader::load(base_, "test", nullptr);
    ASSERT_FALSE(file_result.is_error());
    AppConfig config = file_result.value();

    nlohmann::json manifest = ConfigLoader::strip_credentials_for_manifest(config);

    if (manifest.contains("email")) {
        EXPECT_TRUE(manifest.at("email").contains("smtp_host"));
        EXPECT_TRUE(manifest.at("email").contains("from_email"));
    }
}

// TEST 9: merge_json deep-merges nested objects without losing siblings.
TEST_F(ConfigLoaderDBOverlayTest, MergeJsonDeepMergesNested) {
    nlohmann::json target = {
        {"execution", {{"commission_rate", 0.0005}, {"slippage_bps", 1.0}}}
    };
    nlohmann::json source = {
        {"execution", {{"commission_rate", 0.001}}}
    };

    ConfigLoader::merge_json(target, source);

    EXPECT_EQ(target.at("execution").at("commission_rate"), 0.001);
    EXPECT_EQ(target.at("execution").at("slippage_bps"), 1.0)
        << "Sibling field was lost during merge";
}

// TEST 10: Configuration loads without database (nullptr).
TEST_F(ConfigLoaderDBOverlayTest, LoadWithNullDatabasePointer) {
    auto result = ConfigLoader::load(base_, "test", nullptr);
    EXPECT_FALSE(result.is_error()) << "Load failed with nullptr db: " << result.error()->what();
}

// TEST 11: validate_override_no_credentials rejects override with database field.
TEST_F(ConfigLoaderDBOverlayTest, ValidateRejectsDatabaseField) {
    nlohmann::json bad_override = {
        {"database", {{"host", "evil.com"}}}
    };
    auto result = ConfigLoader::validate_override_no_credentials(bad_override);
    EXPECT_TRUE(result.is_error()) << "Should reject override with database field";
    EXPECT_TRUE(result.error()->what() != nullptr);
    EXPECT_TRUE(std::string(result.error()->what()).find("database") != std::string::npos);
}

// TEST 13: validate_override_no_credentials accepts valid override.
TEST_F(ConfigLoaderDBOverlayTest, ValidateAcceptsValidOverride) {
    nlohmann::json good_override = {
        {"execution", {{"commission_rate", 0.001}}},
        {"backtest", {{"lookback_years", 3}}}
    };
    auto result = ConfigLoader::validate_override_no_credentials(good_override);
    EXPECT_FALSE(result.is_error()) << "Should accept valid override";
}

// TEST 14: validate_override_no_credentials accepts empty override.
TEST_F(ConfigLoaderDBOverlayTest, ValidateAcceptsEmptyOverride) {
    nlohmann::json empty_override = nlohmann::json::object();
    auto result = ConfigLoader::validate_override_no_credentials(empty_override);
    EXPECT_FALSE(result.is_error()) << "Should accept empty override";
}

// TEST 15: validate_override_no_credentials accepts non-object types without error.
TEST_F(ConfigLoaderDBOverlayTest, ValidateAcceptsNonObject) {
    nlohmann::json non_obj = "string_value";
    auto result = ConfigLoader::validate_override_no_credentials(non_obj);
    EXPECT_FALSE(result.is_error()) << "Should accept non-object (no fields to validate)";
}

// TEST 16: validate_override_no_credentials accepts override with other fields.
TEST_F(ConfigLoaderDBOverlayTest, ValidateAcceptsOtherFields) {
    nlohmann::json good_override = {
        {"execution", {{"commission_rate", 0.002}}},
        {"optimization", {{"tau", 1.5}}},
        {"backtest", {{"store_trade_details", false}}}
    };
    auto result = ConfigLoader::validate_override_no_credentials(good_override);
    EXPECT_FALSE(result.is_error()) << "Should accept override with allowed fields";
}

// TEST 17: validate_override_no_credentials with email object but no password.
TEST_F(ConfigLoaderDBOverlayTest, ValidateAcceptsEmailWithoutPassword) {
    nlohmann::json override_with_email = {
        {"email", {{"smtp_host", "smtp.new.com"}, {"smtp_port", 587}}}
    };
    auto result = ConfigLoader::validate_override_no_credentials(override_with_email);
    EXPECT_FALSE(result.is_error()) << "Should accept email override without password";
}

// TEST 18: validate_override_no_credentials rejects nested database fields.
TEST_F(ConfigLoaderDBOverlayTest, ValidateRejectsNestedDatabaseFields) {
    nlohmann::json bad_override = {
        {"database", {
            {"host", "evil.com"},
            {"port", "9999"},
            {"username", "hacker"},
            {"password", "stolen"}
        }}
    };
    auto result = ConfigLoader::validate_override_no_credentials(bad_override);
    EXPECT_TRUE(result.is_error()) << "Should reject nested database fields";
}

// TEST 19: Configuration loads and merges override successfully (with nullptr db).
TEST_F(ConfigLoaderDBOverlayTest, LoadMergesConfigCorrectly) {
    auto result = ConfigLoader::load(base_, "test", nullptr);
    EXPECT_FALSE(result.is_error());
    if (!result.is_error()) {
        AppConfig config = result.value();
        EXPECT_EQ(config.portfolio_id, "TEST_PORTFOLIO");
        EXPECT_EQ(config.initial_capital, 1'000'000.0);
        EXPECT_TRUE(config.strategies_config.contains("TREND_FOLLOWING"));
    }
}

// TEST 20: merge_json with empty source.
TEST_F(ConfigLoaderDBOverlayTest, MergeJsonWithEmptySource) {
    nlohmann::json target = {
        {"execution", {{"commission_rate", 0.0005}}}
    };
    nlohmann::json empty_source = nlohmann::json::object();

    ConfigLoader::merge_json(target, empty_source);

    EXPECT_EQ(target.at("execution").at("commission_rate"), 0.0005);
}

// TEST 21: merge_json replaces primitive values.
TEST_F(ConfigLoaderDBOverlayTest, MergeJsonReplacesPrimitiveValues) {
    nlohmann::json target = {
        {"backtest", {{"lookback_years", 2}}}
    };
    nlohmann::json source = {
        {"backtest", {{"lookback_years", 5}}}
    };

    ConfigLoader::merge_json(target, source);

    EXPECT_EQ(target.at("backtest").at("lookback_years"), 5);
}

// TEST 22: merge_json handles arrays (replaces rather than merges).
TEST_F(ConfigLoaderDBOverlayTest, MergeJsonHandlesArrays) {
    nlohmann::json target = {
        {"strategy_defaults", {{"fdm", nlohmann::json::array({{1, 1.0}, {2, 1.03}})}}}
    };
    nlohmann::json source = {
        {"strategy_defaults", {{"fdm", nlohmann::json::array({{1, 1.1}})}}}
    };

    ConfigLoader::merge_json(target, source);

    auto& fdm = target.at("strategy_defaults").at("fdm");
    EXPECT_EQ(fdm.size(), 1) << "Arrays should be replaced, not merged";
}

// TEST 23: strip_credentials_for_manifest removes database section.
TEST_F(ConfigLoaderDBOverlayTest, StripCredentialsRemovesDatabaseSection) {
    auto file_result = ConfigLoader::load(base_, "test", nullptr);
    ASSERT_FALSE(file_result.is_error());
    AppConfig config = file_result.value();

    nlohmann::json manifest = ConfigLoader::strip_credentials_for_manifest(config);

    EXPECT_FALSE(manifest.contains("database"))
        << "Manifest should not contain database section";
}

// TEST 24: strip_credentials_for_manifest preserves execution section.
TEST_F(ConfigLoaderDBOverlayTest, StripCredentialsPreservesExecutionSection) {
    auto file_result = ConfigLoader::load(base_, "test", nullptr);
    ASSERT_FALSE(file_result.is_error());
    AppConfig config = file_result.value();

    nlohmann::json manifest = ConfigLoader::strip_credentials_for_manifest(config);

    EXPECT_TRUE(manifest.contains("execution"))
        << "Manifest should contain execution section";
    EXPECT_TRUE(manifest.at("execution").contains("commission_rate"));
}

// TEST 25: Load with invalid portfolio directory.
TEST_F(ConfigLoaderDBOverlayTest, LoadWithInvalidPortfolioDirectory) {
    auto result = ConfigLoader::load(base_, "nonexistent_portfolio", nullptr);
    EXPECT_TRUE(result.is_error())
        << "Should fail when portfolio directory does not exist";
}

// TEST 26: merge_json with multiple nested levels.
TEST_F(ConfigLoaderDBOverlayTest, MergeJsonMultipleLevels) {
    nlohmann::json target = {
        {"optimization", {
            {"tau", 1.0},
            {"capital", 500000.0},
            {"cost_penalty_scalar", 50}
        }}
    };
    nlohmann::json source = {
        {"optimization", {
            {"tau", 1.5},
            {"max_iterations", 200}
        }}
    };

    ConfigLoader::merge_json(target, source);

    EXPECT_EQ(target.at("optimization").at("tau"), 1.5);
    EXPECT_EQ(target.at("optimization").at("capital"), 500000.0);
    EXPECT_EQ(target.at("optimization").at("max_iterations"), 200);
}

// ===== Tests for load_db_override, publish_config_manifest, and load() with DB =====
// These tests verify the DB overlay functionality using MockPostgresDatabase.
// Coverage includes: DB connection errors, validation, credential stripping, and fallback paths

// TEST 15: load_db_override with disconnected DB returns error
TEST_F(ConfigLoaderDBOverlayTest, LoadDbOverrideNotConnected) {
    MockPostgresDatabase mock_db("postgresql://test");
    // Don't connect - DB is disconnected

    auto result = ConfigLoader::load_db_override(&mock_db, "TEST_PORTFOLIO");
    ASSERT_TRUE(result.is_error()) << "Should return error when DB not connected";
    EXPECT_TRUE(std::string(result.error()->what()).find("not connected") != std::string::npos);
}

// TEST 16: publish_config_manifest with disconnected DB returns error
TEST_F(ConfigLoaderDBOverlayTest, PublishConfigManifestNotConnected) {
    MockPostgresDatabase mock_db("postgresql://test");
    // Don't connect

    nlohmann::json manifest = {{"portfolio_id", "TEST_PORTFOLIO"}};

    auto result = ConfigLoader::publish_config_manifest(&mock_db, "TEST_PORTFOLIO", manifest);
    ASSERT_TRUE(result.is_error()) << "Should return error when DB not connected";
    EXPECT_TRUE(std::string(result.error()->what()).find("not connected") != std::string::npos);
}

// TEST 17: load() with nullptr DB parameter works (backward compat)
TEST_F(ConfigLoaderDBOverlayTest, LoadWithNullDbPointerWorks) {
    auto result = ConfigLoader::load(base_, "test", nullptr);
    ASSERT_FALSE(result.is_error()) << "load() with nullptr should work";

    AppConfig config = result.value();
    EXPECT_EQ(config.portfolio_id, "TEST_PORTFOLIO");
    EXPECT_EQ(config.execution.commission_rate, 0.0005);
}

// TEST 18: load() with disconnected DB falls back to file config gracefully
TEST_F(ConfigLoaderDBOverlayTest, LoadWithDisconnectedDbFallsback) {
    MockPostgresDatabase mock_db("postgresql://test");
    // Don't call connect() - DB is disconnected

    auto result = ConfigLoader::load(base_, "test", &mock_db);
    ASSERT_FALSE(result.is_error()) << "load() should fallback to file config on DB error";

    AppConfig config = result.value();
    EXPECT_EQ(config.portfolio_id, "TEST_PORTFOLIO");
    EXPECT_EQ(config.initial_capital, 1'000'000.0);
    EXPECT_EQ(config.execution.commission_rate, 0.0005)
        << "Should use file config when DB connection fails";
}

// TEST 19: validate_override_no_credentials rejects database field
TEST_F(ConfigLoaderDBOverlayTest, ValidateRejectsDatabaseField2) {
    nlohmann::json bad = {{"database", {{"host", "attacker.com"}}}};

    auto result = ConfigLoader::validate_override_no_credentials(bad);
    ASSERT_TRUE(result.is_error());
    EXPECT_TRUE(std::string(result.error()->what()).find("database") != std::string::npos);
}

// TEST 20: validate_override_no_credentials rejects email.password field
TEST_F(ConfigLoaderDBOverlayTest, ValidateRejectsEmailPasswordField) {
    nlohmann::json bad = {{"email", {{"password", "hacked"}}}};

    auto result = ConfigLoader::validate_override_no_credentials(bad);
    ASSERT_TRUE(result.is_error());
    EXPECT_TRUE(std::string(result.error()->what()).find("email.password") != std::string::npos);
}

// TEST 21: validate_override_no_credentials accepts valid overrides
TEST_F(ConfigLoaderDBOverlayTest, ValidateAcceptsGoodOverride) {
    nlohmann::json good = {
        {"execution", {{"commission_rate", 0.001}}},
        {"backtest", {{"lookback_years", 3}}}
    };

    auto result = ConfigLoader::validate_override_no_credentials(good);
    ASSERT_FALSE(result.is_error()) << "Good override should be accepted";
}

// TEST 22: validate_override_no_credentials accepts empty object
TEST_F(ConfigLoaderDBOverlayTest, ValidateAcceptsEmpty) {
    nlohmann::json empty = nlohmann::json::object();

    auto result = ConfigLoader::validate_override_no_credentials(empty);
    ASSERT_FALSE(result.is_error()) << "Empty override should be accepted";
}

// TEST 23: strip_credentials_for_manifest removes database section
TEST_F(ConfigLoaderDBOverlayTest, StripCredentialsRemovesDatabase) {
    auto file_result = ConfigLoader::load(base_, "test", nullptr);
    ASSERT_FALSE(file_result.is_error());

    AppConfig config = file_result.value();
    nlohmann::json manifest = ConfigLoader::strip_credentials_for_manifest(config);

    EXPECT_FALSE(manifest.contains("database"))
        << "Manifest should not contain database section";
}

// TEST 24: strip_credentials_for_manifest removes email.password only
TEST_F(ConfigLoaderDBOverlayTest, StripCredentialsRemovesPasswordOnly) {
    auto file_result = ConfigLoader::load(base_, "test", nullptr);
    ASSERT_FALSE(file_result.is_error());

    AppConfig config = file_result.value();
    nlohmann::json manifest = ConfigLoader::strip_credentials_for_manifest(config);

    ASSERT_TRUE(manifest.contains("email")) << "Email section should exist";
    EXPECT_FALSE(manifest.at("email").contains("password")) << "Password should be removed";
    EXPECT_TRUE(manifest.at("email").contains("smtp_host")) << "Other fields preserved";
}

// TEST 25: strip_credentials_for_manifest preserves execution config
TEST_F(ConfigLoaderDBOverlayTest, StripCredentialsPreservesExecution) {
    auto file_result = ConfigLoader::load(base_, "test", nullptr);
    ASSERT_FALSE(file_result.is_error());

    AppConfig config = file_result.value();
    nlohmann::json manifest = ConfigLoader::strip_credentials_for_manifest(config);

    EXPECT_TRUE(manifest.contains("execution"));
    EXPECT_TRUE(manifest.at("execution").contains("commission_rate"));
}

// TEST 26: merge_json deep merges nested objects
TEST_F(ConfigLoaderDBOverlayTest, MergeJsonNestedMerge) {
    nlohmann::json target = {
        {"execution", {{"commission_rate", 0.0005}, {"slippage_bps", 1.0}}}
    };
    nlohmann::json source = {
        {"execution", {{"commission_rate", 0.001}}}
    };

    ConfigLoader::merge_json(target, source);

    EXPECT_EQ(target.at("execution").at("commission_rate"), 0.001);
    EXPECT_EQ(target.at("execution").at("slippage_bps"), 1.0)
        << "Sibling fields should not be lost";
}

// TEST 27: merge_json replaces scalar values
TEST_F(ConfigLoaderDBOverlayTest, MergeJsonScalarReplacement) {
    nlohmann::json target = {
        {"initial_capital", 1000000.0},
        {"reserve_capital_pct", 0.1}
    };
    nlohmann::json source = {
        {"initial_capital", 2000000.0}
    };

    ConfigLoader::merge_json(target, source);

    EXPECT_EQ(target.at("initial_capital"), 2000000.0);
    EXPECT_EQ(target.at("reserve_capital_pct"), 0.1);
}

// TEST 28: File config loads without DB errors
TEST_F(ConfigLoaderDBOverlayTest, FileConfigLoadsCorrectly) {
    auto result = ConfigLoader::load(base_, "test", nullptr);
    ASSERT_FALSE(result.is_error());

    AppConfig config = result.value();
    EXPECT_EQ(config.portfolio_id, "TEST_PORTFOLIO");
    EXPECT_EQ(config.initial_capital, 1'000'000.0);
    EXPECT_EQ(config.database.host, "default_host");
}
