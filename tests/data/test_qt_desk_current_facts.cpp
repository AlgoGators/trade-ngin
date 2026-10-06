#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <limits>

namespace trade_ngin {
namespace {
using Json = nlohmann::json;

TEST(QtDeskCurrentFactsTest, MatchesAllActualPythonRawJsonVectors) {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                "contracts" / "qt-raw-json-digest-v1.json";
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.good());
    const auto cases = Json::parse(file).at("cases");
    ASSERT_EQ(cases.size(), 6u);
    for (const auto& item : cases) {
        SCOPED_TRACE(item.at("name").get<std::string>());
        auto bytes = canonical_qt_desk_source_json(item.at("payload"));
        ASSERT_TRUE(bytes.is_ok()) << bytes.error()->what();
        EXPECT_EQ(bytes.value(), item.at("canonical_utf8").get<std::string>());
        auto hash = qt_sha256_hex(bytes.value());
        ASSERT_TRUE(hash.is_ok());
        EXPECT_EQ(hash.value(), item.at("sha256").get<std::string>());
    }
}

TEST(QtDeskCurrentFactsTest, RefusesNonfiniteAndOversizedSourceJson) {
    EXPECT_TRUE(canonical_qt_desk_source_json(
        Json{{"x",std::numeric_limits<double>::infinity()}}).is_error());
    EXPECT_TRUE(canonical_qt_desk_source_json(
        Json{{"x",std::string(1'048'577,'x')}}).is_error());
}
}  // namespace
}  // namespace trade_ngin
