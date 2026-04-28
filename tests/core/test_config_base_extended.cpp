// Extended branch coverage for config_base.cpp. Targets the open-failure
// paths in save_to_file and load_from_file, plus the JSON-parse failure
// path that wraps the exception.

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "trade_ngin/core/config_base.hpp"

using namespace trade_ngin;

namespace {

class ToyConfig : public ConfigBase {
public:
    int value = 0;
    nlohmann::json to_json() const override { return nlohmann::json{{"value", value}}; }
    void from_json(const nlohmann::json& j) override {
        if (j.contains("value")) value = j["value"].get<int>();
    }
};

}  // namespace

class ConfigBaseExtendedTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / "config_base_ext";
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }
    std::filesystem::path dir_;
};

TEST_F(ConfigBaseExtendedTest, SaveToUnwritablePathReturnsError) {
    ToyConfig c;
    auto r = c.save_to_file("/this/path/does/not/exist/cannot.json");
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConfigBaseExtendedTest, LoadFromMissingFileReturnsError) {
    ToyConfig c;
    auto missing = (dir_ / "missing.json").string();
    auto r = c.load_from_file(missing);
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConfigBaseExtendedTest, LoadFromMalformedJsonReturnsError) {
    auto p = dir_ / "bad.json";
    std::ofstream(p) << "{not valid json";
    ToyConfig c;
    auto r = c.load_from_file(p.string());
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConfigBaseExtendedTest, SaveAndReloadRoundTripPreservesValue) {
    ToyConfig c;
    c.value = 7;
    auto p = dir_ / "rt.json";
    ASSERT_TRUE(c.save_to_file(p.string()).is_ok());

    ToyConfig loaded;
    ASSERT_TRUE(loaded.load_from_file(p.string()).is_ok());
    EXPECT_EQ(loaded.value, 7);
}
