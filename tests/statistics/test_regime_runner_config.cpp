// Tests for the regime runners' database connection lookup
// (trade_ngin::regime::load_runner_db_connection). The runners read the
// connection only from config/defaults.json through the ConfigLoader; these
// tests build a config directory from config_template/ in a temp dir.

#include <gtest/gtest.h>

#include "trade_ngin/regime_detection/runner_config.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
using trade_ngin::ErrorCode;
using trade_ngin::regime::load_runner_db_connection;

#ifndef TRADE_NGIN_CONFIG_TEMPLATE_DIR
#error "TRADE_NGIN_CONFIG_TEMPLATE_DIR must point at config_template/"
#endif

namespace {

class RegimeRunnerConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::mt19937 gen(std::random_device{}());
        root_ = fs::temp_directory_path() /
                ("regime_runner_cfg_" + std::to_string(gen()));
        fs::create_directories(root_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    // Copy config_template/ to <root>/<name> and optionally overwrite its
    // database block.
    fs::path make_config(const std::string& name, const nlohmann::json* database) {
        fs::path dir = root_ / name;
        fs::copy(TRADE_NGIN_CONFIG_TEMPLATE_DIR, dir, fs::copy_options::recursive);
        if (database) {
            nlohmann::json defaults;
            {
                std::ifstream in(dir / "defaults.json");
                in >> defaults;
            }
            defaults["database"] = *database;
            std::ofstream out(dir / "defaults.json");
            out << defaults.dump(2);
        }
        return dir;
    }

    static nlohmann::json filled_db(const std::string& host) {
        return {{"host", host},       {"port", "6543"},     {"username", "regime_user"},
                {"password", "s3cret"}, {"name", "scratch"}, {"num_connections", 2}};
    }

    fs::path root_;
};

}  // namespace

TEST_F(RegimeRunnerConfigTest, ReturnsConnectionStringFromDefaultsJson) {
    auto db = filled_db("db.internal");
    auto dir = make_config("config", &db);

    auto r = load_runner_db_connection({dir});
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(r.value(), "postgresql://regime_user:s3cret@db.internal:6543/scratch");
}

TEST_F(RegimeRunnerConfigTest, FirstDirectoryWithDefaultsWins) {
    // ./config missing, ../config present: the second search dir is used.
    auto db = filled_db("second");
    auto present = make_config("parent_config", &db);

    auto r = load_runner_db_connection({root_ / "no_such_dir", present});
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(r.value(), "postgresql://regime_user:s3cret@second:6543/scratch");

    // Both present: the first one wins.
    auto db_first = filled_db("first");
    auto first = make_config("config", &db_first);
    auto r2 = load_runner_db_connection({first, present});
    ASSERT_TRUE(r2.is_ok()) << r2.error()->what();
    EXPECT_EQ(r2.value(), "postgresql://regime_user:s3cret@first:6543/scratch");
}

TEST_F(RegimeRunnerConfigTest, NoConfigDirectoryIsFileNotFound) {
    auto r = load_runner_db_connection({root_ / "a", root_ / "b"});
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::FILE_NOT_FOUND);
}

TEST_F(RegimeRunnerConfigTest, TemplatePlaceholdersAreRefused) {
    // An unedited copy of config_template/ must not produce a connection
    // string pointing at host "YOUR_DB_HOST".
    auto dir = make_config("config", nullptr);

    auto r = load_runner_db_connection({dir});
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(RegimeRunnerConfigTest, EmptyHostIsRefused) {
    // ConfigLoader's own validation refuses it; the helper passes its code through.
    auto db = filled_db("");
    auto dir = make_config("config", &db);

    auto r = load_runner_db_connection({dir});
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_DATA);
}

TEST_F(RegimeRunnerConfigTest, BrokenConfigIsAnErrorNotAFallThrough) {
    // A defaults.json that exists but does not load must stop the search,
    // not silently fall through to the next directory.
    auto good_db = filled_db("fallback");
    auto good = make_config("parent_config", &good_db);
    auto broken = make_config("config", nullptr);
    fs::remove(broken / "portfolios" / "base" / "risk.json");

    auto r = load_runner_db_connection({broken, good});
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find(broken.string()), std::string::npos);
}
