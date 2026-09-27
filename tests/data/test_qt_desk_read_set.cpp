#include "trade_ngin/data/qt_desk_read_set.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace trade_ngin {
namespace {
using Json = nlohmann::json;

Json minimal_read_set() {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "contracts" / "qt-read-set-minimal-v1.json";
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) throw std::runtime_error("missing authoritative minimal read-set fixture");
    return Json::parse(file);
}

Json full_vectors() {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "contracts" / "qt-read-set-v1.json";
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) throw std::runtime_error("missing accepted API read-set vectors");
    return Json::parse(file);
}

TEST(QtDeskReadSetTest, MatchesApiPrivateMinimalVectorAndSortsExternalFacts) {
    auto document=minimal_read_set();
    auto encoded=canonical_qt_desk_read_set_bytes(document);
    ASSERT_TRUE(encoded.is_ok()) << encoded.error()->what();
    EXPECT_EQ(encoded.value().size(),2140u);
    auto digest=qt_desk_read_set_digest(document);
    ASSERT_TRUE(digest.is_ok()) << digest.error()->what();
    EXPECT_EQ(digest.value(),"f0e3ff844b5608514b2d5ded5665fb8ba4b6694c3f86381b8ed7d1a8c6bcf62d");
    std::reverse(document["external_sources"].begin(),document["external_sources"].end());
    EXPECT_EQ(canonical_qt_desk_read_set_bytes(document).value(),encoded.value());
}

TEST(QtDeskReadSetTest, RefusesUnknownIncompleteOrForeignFacts) {
    auto document=minimal_read_set();
    document["unknown"]="value";
    EXPECT_TRUE(canonical_qt_desk_read_set_bytes(document).is_error());
    document=minimal_read_set();
    document["external_sources"].erase(0);
    EXPECT_TRUE(canonical_qt_desk_read_set_bytes(document).is_error());
    document=minimal_read_set();
    document["external_sources"][0]["status"]="available";
    EXPECT_TRUE(canonical_qt_desk_read_set_bytes(document).is_error());
    document=minimal_read_set();
    document["saved_rows"].push_back({{"key",{{"portfolio_id","foreign"},{"strategy_id","s"},
        {"strategy_name","name"},{"date","2026-09-25"},{"symbol","X"},
        {"portfolio_type","qt"}}},{"quantity_exact","0"},{"average_price_exact","1"}});
    EXPECT_TRUE(canonical_qt_desk_read_set_bytes(document).is_error());
}

TEST(QtDeskReadSetTest, MatchesBothAcceptedApiFullVectorsByteForByte) {
    auto vectors = full_vectors();
    ASSERT_EQ(vectors.at("schema"), "qt-read-set-test-v1");
    ASSERT_EQ(vectors.at("cases").size(), 2u);
    for (const auto& item : vectors.at("cases")) {
        SCOPED_TRACE(item.at("name").get<std::string>());
        auto bytes = canonical_qt_desk_read_set_bytes(item.at("payload"));
        ASSERT_TRUE(bytes.is_ok()) << bytes.error()->what();
        EXPECT_EQ(bytes.value(), item.at("canonical_utf8").get<std::string>());
        EXPECT_EQ(bytes.value().size(), item.at("byte_count").get<std::size_t>());
        auto hash = qt_desk_read_set_digest(item.at("payload"));
        ASSERT_TRUE(hash.is_ok()) << hash.error()->what();
        EXPECT_EQ(hash.value(), item.at("sha256").get<std::string>());
    }
}

TEST(QtDeskReadSetTest, RejectsDuplicateAndNoncanonicalFullFacts) {
    auto document = full_vectors().at("cases").at(1).at("payload");
    auto duplicate = document;
    duplicate["grants"].push_back(duplicate["grants"][0]);
    EXPECT_TRUE(canonical_qt_desk_read_set_bytes(duplicate).is_error());
    auto foreign = document;
    foreign["source_rows"][0]["key"]["date"] = "2026-09-24";
    EXPECT_TRUE(canonical_qt_desk_read_set_bytes(foreign).is_error());
    auto malformed = document;
    malformed["external_sources"][0]["digest"] = "ABC";
    EXPECT_TRUE(canonical_qt_desk_read_set_bytes(malformed).is_error());
    auto wrong_presence = document;
    wrong_presence["capability"]["enabled"] = nullptr;
    wrong_presence["capability"]["version"] = nullptr;
    EXPECT_TRUE(canonical_qt_desk_read_set_bytes(wrong_presence).is_error());
}

TEST(QtDeskReadSetTest, SortsFullKeyTuplesRatherThanEncodedLengths) {
    auto document = full_vectors().at("cases").at(1).at("payload");
    auto row = document["source_rows"][0];
    document["source_rows"] = Json::array();
    row["key"]["strategy_id"] = "z";
    document["source_rows"].push_back(row);
    row["key"]["strategy_id"] = "aa";
    document["source_rows"].push_back(row);
    auto bytes = canonical_qt_desk_read_set_bytes(document);
    ASSERT_TRUE(bytes.is_ok()) << bytes.error()->what();
    auto normalized = Json::parse(bytes.value());
    EXPECT_EQ(normalized["source_rows"][0]["key"]["strategy_id"], "aa");
    EXPECT_EQ(normalized["source_rows"][1]["key"]["strategy_id"], "z");
}

TEST(QtDeskReadSetTest, RejectsMalformedProducerTimestampAndPythonBlankText) {
    auto document = full_vectors().at("cases").at(1).at("payload");
    document["registry"][0]["updated_at"] = "2026-09-25T10:00:00.Z";
    EXPECT_TRUE(canonical_qt_desk_read_set_bytes(document).is_error());
    document = minimal_read_set();
    document["book_id"] = std::string(1, '\x1C');
    EXPECT_TRUE(canonical_qt_desk_read_set_bytes(document).is_error());
}

TEST(QtDeskReadSetTest, UsesDistinctTuplesForMembershipIdentity) {
    auto document = minimal_read_set();
    document["memberships"] = Json::array({
        {{"strategy_id", std::string("a\0b",3)}, {"portfolio_id","c"}},
        {{"strategy_id","a"}, {"portfolio_id",std::string("b\0c",3)}}
    });
    auto bytes = canonical_qt_desk_read_set_bytes(document);
    ASSERT_TRUE(bytes.is_ok()) << bytes.error()->what();
    auto normalized = Json::parse(bytes.value());
    EXPECT_EQ(normalized["memberships"].size(), 2u);
    EXPECT_EQ(normalized["memberships"][0]["strategy_id"], "a");
}

TEST(QtDeskReadSetTest, AdmitsOnlyCompleteReadyAndFreshSelectedFacts) {
    auto document = full_vectors().at("cases").at(1).at("payload");
    auto capture = admit_qt_desk_read_set(document, "2026-09-25T12:00:00Z");
    ASSERT_TRUE(capture.is_ok()) << capture.error()->what();
    EXPECT_EQ(capture.value().source_day, document["source_day"].get<std::string>());
    EXPECT_EQ(capture.value().digest, qt_desk_read_set_digest(document).value());
    EXPECT_TRUE(admit_qt_desk_read_set(document, "2026-09-25T12:01:01Z").is_error());
    EXPECT_TRUE(admit_qt_desk_read_set(document, "2026-09-25T11:58:59Z").is_error());
    auto incomplete = document;
    incomplete["capability"]["enabled"] = nullptr;
    ASSERT_TRUE(canonical_qt_desk_read_set_bytes(incomplete).is_ok());
    EXPECT_TRUE(admit_qt_desk_read_set(incomplete, "2026-09-25T12:00:00Z").is_error());
    incomplete = document;
    incomplete["risk_limits"]["content_digest"] = nullptr;
    ASSERT_TRUE(canonical_qt_desk_read_set_bytes(incomplete).is_ok());
    EXPECT_TRUE(admit_qt_desk_read_set(incomplete, "2026-09-25T12:00:00Z").is_error());
    EXPECT_TRUE(admit_qt_desk_read_set(
        full_vectors().at("cases").at(0).at("payload"), "2026-09-25T12:00:00Z").is_error());
}

TEST(QtDeskReadSetTest, CanonicalizesExplicitMissingAccountingWithoutInventingIt) {
    auto document = minimal_read_set();
    document["saved_accounting"] = Json::array();
    auto bytes = canonical_qt_desk_read_set_bytes(document);
    ASSERT_TRUE(bytes.is_ok()) << bytes.error()->what();
    EXPECT_TRUE(admit_qt_desk_read_set(document, "2026-09-25T12:00:00Z").is_error());
}

TEST(QtDeskReadSetTest, AccountingIsRequiredBoundAndHashSensitive) {
    auto document = full_vectors().at("cases").at(1).at("payload");
    ASSERT_FALSE(document["saved_accounting"].empty());
    auto original = qt_desk_read_set_digest(document);
    ASSERT_TRUE(original.is_ok());
    auto changed = document;
    changed.erase("saved_accounting");
    EXPECT_TRUE(canonical_qt_desk_read_set_bytes(changed).is_error());
    changed = document;
    changed["saved_accounting"] = Json::array();
    EXPECT_TRUE(canonical_qt_desk_read_set_bytes(changed).is_ok());
    EXPECT_TRUE(admit_qt_desk_read_set(changed, "2026-09-25T12:00:00Z").is_error());
    changed = document;
    changed["saved_accounting"][0]["quantity_exact"] = "37";
    EXPECT_TRUE(canonical_qt_desk_read_set_bytes(changed).is_ok());
    EXPECT_TRUE(admit_qt_desk_read_set(changed, "2026-09-25T12:00:00Z").is_error());
    changed = document;
    changed["saved_accounting"][0]["daily_realized_pnl_exact"] = "0.00000001";
    ASSERT_TRUE(qt_desk_read_set_digest(changed).is_ok());
    EXPECT_NE(qt_desk_read_set_digest(changed).value(), original.value());
    changed = document;
    changed["saved_accounting"][0]["last_update"] = "2026-09-25T11:58:00Z";
    ASSERT_TRUE(qt_desk_read_set_digest(changed).is_ok());
    EXPECT_NE(qt_desk_read_set_digest(changed).value(), original.value());
    changed = document;
    changed["saved_accounting"][0]["last_update"] = nullptr;
    EXPECT_TRUE(canonical_qt_desk_read_set_bytes(changed).is_error());
}

}  // namespace
}  // namespace trade_ngin
