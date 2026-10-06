#include "trade_ngin/portfolio/qt_wire.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <limits>

namespace trade_ngin {
namespace {

nlohmann::json workflow_examples() {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "contracts" / "qt-workflow-v1.json";
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) throw std::runtime_error("missing workflow authority fixture");
    return nlohmann::json::parse(file);
}

TEST(QtWorkflowParityTest, FullPreviewsMatchAuthoritativePayloadDigests) {
    const auto examples = workflow_examples();
    for (const auto* name : {"preview_clean", "preview_breach"}) {
        auto preview = examples.at(name);
        const auto expected = preview.at("payload_digest").get<std::string>();
        preview.erase("payload_digest");
        auto digest = qt_digest_v1(preview);
        ASSERT_TRUE(digest.is_ok()) << name << ": " << digest.error()->what();
        EXPECT_EQ(digest.value(), expected);
    }
}

TEST(QtWorkflowParityTest, DiagnosticNumbersRemainFiniteAndNeverBecomeExactMoney) {
    for (const auto* field : {"value_diagnostic", "limit_diagnostic", "actual_diagnostic", "weight_diagnostic"}) {
        EXPECT_TRUE(canonical_qt_bytes({{field, "0.12345678912345678"}}).is_ok());
        EXPECT_TRUE(canonical_qt_bytes({{field, "5e-324"}}).is_ok());
        EXPECT_TRUE(canonical_qt_bytes({{field, "0e-9999"}}).is_ok());
        EXPECT_TRUE(canonical_qt_bytes({{field, "1e309"}}).is_error());
        EXPECT_TRUE(canonical_qt_bytes({{field, "1e-9999"}}).is_error());
        EXPECT_TRUE(canonical_qt_bytes({{field, "NaN"}}).is_error());
    }
    for (const auto* obsolete : {"value_exact", "limit_exact", "actual_exact"})
        EXPECT_TRUE(canonical_qt_bytes({{obsolete, "1"}}).is_error());
    EXPECT_TRUE(canonical_qt_bytes({{"quantity_exact", "1e-8"}}).is_error());
}

TEST(QtWorkflowParityTest, RejectsIncompleteNestedEvidenceAndOutOfRangeIdentity) {
    const auto fixture = workflow_examples();
    auto preview = fixture.at("preview_clean");
    preview["evaluation"]["selected_risk"]["metrics"][0].erase("unit");
    EXPECT_TRUE(canonical_qt_bytes(preview).is_error());
    preview = fixture.at("preview_clean");
    preview["evaluation"]["selected_costs"]["passed"] = true;
    EXPECT_TRUE(canonical_qt_bytes(preview).is_error());
    EXPECT_TRUE(canonical_qt_bytes({{"version", std::numeric_limits<uint64_t>::max()}}).is_error());
    EXPECT_TRUE(canonical_qt_bytes({{"user_id", "-1"}}).is_error());
    EXPECT_TRUE(canonical_qt_bytes({{"user_id", "01"}}).is_error());
    EXPECT_TRUE(canonical_qt_bytes({{"user_id", "9223372036854775808"}}).is_error());
    EXPECT_TRUE(canonical_qt_bytes({{"user_id", "9223372036854775807"}}).is_ok());
}

TEST(QtWorkflowParityTest, AggregateBindingsSortAndRejectDuplicateTypedInstruments) {
    const nlohmann::json equity = {{"instrument_type", "EQUITY"}, {"symbol", "SYN"},
        {"component_keys", nlohmann::json::array()},
        {"previous_net_quantity_exact", "0"}, {"proposed_net_quantity_exact", "0"}};
    auto future = equity;
    future["instrument_type"] = "FUTURE";
    auto first = canonical_qt_bytes({{"aggregate_bindings", nlohmann::json::array({equity, future})}});
    auto reverse = canonical_qt_bytes({{"aggregate_bindings", nlohmann::json::array({future, equity})}});
    ASSERT_TRUE(first.is_ok());
    ASSERT_TRUE(reverse.is_ok());
    EXPECT_EQ(first.value(), reverse.value());
    EXPECT_TRUE(canonical_qt_bytes({{"aggregate_bindings", nlohmann::json::array({equity, equity})}}).is_error());
}

TEST(QtWorkflowParityTest, UnicodeLimitsCountCodepointsLikeTheApi) {
    for (const std::string codepoint : {std::string("\xC3\xA9"),
                                       std::string("\xF0\x9F\x98\x80")}) {
        std::string maximum;
        for (size_t i=0;i<4096;++i) maximum += codepoint;
        auto accepted=canonical_qt_bytes({{"symbol",maximum}});
        ASSERT_TRUE(accepted.is_ok());
        EXPECT_EQ(accepted.value(), "{\"symbol\":\""+maximum+"\"}");
        EXPECT_TRUE(canonical_qt_bytes({{"symbol",maximum+codepoint}}).is_error());
        EXPECT_TRUE(canonical_qt_bytes({{"diagnostics",nlohmann::json::array({maximum})}}).is_ok());
        EXPECT_TRUE(canonical_qt_bytes({{"diagnostics",nlohmann::json::array({maximum+codepoint})}}).is_error());
    }
    EXPECT_TRUE(canonical_qt_bytes({{"symbol",std::string("\xC0\xAF")}}).is_error());
    EXPECT_TRUE(canonical_qt_bytes({{"symbol",std::string("\xED\xA0\x80")}}).is_error());
}

}  // namespace
}  // namespace trade_ngin

