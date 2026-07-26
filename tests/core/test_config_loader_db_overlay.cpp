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
    auto result = ConfigLoader::load(base_ / "config", "test", nullptr);
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
    EXPECT_THAT(result.error()->what(), testing::HasSubstr("database"));
}

// TEST 3: Validate override rejects database.password.
TEST_F(ConfigLoaderDBOverlayTest, ValidateRejectsDatabasePassword) {
    nlohmann::json bad_override = {
        {"database", {{"password", "hacked"}}}
    };

    auto result = ConfigLoader::validate_override_no_credentials(bad_override);
    EXPECT_TRUE(result.is_error()) << "Should reject database.password";
    EXPECT_THAT(result.error()->what(), testing::HasSubstr("database"));
}

// TEST 4: Validate override rejects email.password.
TEST_F(ConfigLoaderDBOverlayTest, ValidateRejectsEmailPassword) {
    nlohmann::json bad_override = {
        {"email", {{"password", "hacked_email"}}}
    };

    auto result = ConfigLoader::validate_override_no_credentials(bad_override);
    EXPECT_TRUE(result.is_error()) << "Should reject email.password";
    EXPECT_THAT(result.error()->what(), testing::HasSubstr("email.password"));
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
    auto file_result = ConfigLoader::load(base_ / "config", "test", nullptr);
    ASSERT_FALSE(file_result.is_error());
    AppConfig config = file_result.value();

    nlohmann::json manifest = ConfigLoader::strip_credentials_for_manifest(config);

    EXPECT_FALSE(manifest.contains("database")) << "Manifest still contains database section";
}

// TEST 7: strip_credentials_for_manifest removes email.password.
TEST_F(ConfigLoaderDBOverlayTest, StripCredentialsRemovesEmailPassword) {
    auto file_result = ConfigLoader::load(base_ / "config", "test", nullptr);
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
    auto file_result = ConfigLoader::load(base_ / "config", "test", nullptr);
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
    auto result = ConfigLoader::load(base_ / "config", "test", nullptr);
    EXPECT_FALSE(result.is_error()) << "Load failed with nullptr db: " << result.error()->what();
}
