#include "trade_ngin/data/qt_desk_publication.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "qt_test_build_identity.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace trade_ngin {
namespace {
using Json = nlohmann::json;

Json fixture_preview() {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "contracts" / "qt-workflow-v1.json";
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) throw std::runtime_error("missing authoritative QT workflow fixture");
    return Json::parse(file).at("preview_clean");
}

Json preview_row(const Json& payload) {
    return {{"preview_id",payload.at("preview_id")}, {"book_id",payload.at("book_id")},
            {"source_day",payload.at("source_day")}, {"draft_id",payload.at("draft_id")},
            {"draft_revision",payload.at("draft_revision")},
            {"source_digest",payload.at("source_digest")},
            {"provenance_digest",payload.at("provenance_digest")},
            {"read_set_digest",payload.at("read_set_digest")},
            {"selected_book_digest",payload.at("selected_book_digest")},
            {"payload_digest",payload.at("payload_digest")},
            {"policy_version","synthetic-policy-v1"},
            {"evaluator_build",TRADE_NGIN_GIT_SHA},
            {"availability","ready"}, {"state","confirmed_decision"},
            {"payload",payload}, {"read_set_payload",Json::object()}};
}

Json decision_row(const Json& preview) {
    constexpr const char* kDecisionId = "40000000-0000-4000-8000-000000000001";
    Json envelope = {{"schema_version","qt-desk-decision/v1"},
        {"decision_id",kDecisionId}, {"preview_id",preview.at("preview_id")},
        {"book_id",preview.at("book_id")}, {"source_day",preview.at("source_day")},
        {"preview_payload_digest",preview.at("payload_digest")},
        {"selected_book_digest",preview.at("selected_book_digest")},
        {"read_set_digest",preview.at("read_set_digest")}};
    return {{"decision_id",kDecisionId}, {"preview_id",preview.at("preview_id")},
            {"book_id",preview.at("book_id")}, {"source_day",preview.at("source_day")},
            {"status","confirmed_decision"}, {"draft_id",preview.at("draft_id")},
            {"draft_revision",preview.at("draft_revision")},
            {"provenance_digest",preview.at("provenance_digest")},
            {"selected_book_digest",preview.at("selected_book_digest")},
            {"read_set_digest",preview.at("read_set_digest")},
            {"workflow_capability_version",1}, {"submitter_grant_version",1},
            {"policy_version",preview.at("policy_version")},
            {"model_publication_id",nullptr}, {"created_by",1},
            {"payload",std::move(envelope)}};
}

void rehash_preview(Json& preview) {
    Json without_digest = preview.at("payload");
    without_digest.erase("payload_digest");
    auto digest = qt_digest_v1(without_digest);
    ASSERT_TRUE(digest.is_ok());
    preview["payload"]["payload_digest"] = digest.value();
    preview["payload_digest"] = digest.value();
}

TEST(QtDeskDecisionTest, DerivesCompleteTargetBookFromImmutablePreview) {
    auto preview = preview_row(fixture_preview());
    auto decision = decision_row(preview);
    auto result = validate_qt_desk_decision_snapshot(decision, preview);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    ASSERT_EQ(result.value().selected_rows.size(), 2u);
    EXPECT_EQ(result.value().selected_rows[0].key.portfolio_type, "qt");
    EXPECT_EQ(result.value().selected_rows[0].quantity.to_string(), "5");
    EXPECT_EQ(result.value().selected_rows[1].quantity.to_string(), "1");
    EXPECT_EQ(result.value().selected_book_digest,
              "170528e9e3a72c56773ceeba05ed1762eb4a35ff1a35d0af8633e35039ee99df");
}

TEST(QtDeskDecisionTest, AdmitsReceiptProvenHumanOriginWithoutChangingSelectedQuantity) {
    auto preview = preview_row(fixture_preview());
    preview["payload"]["selection_rows"][0]["origin"] = "verified_qt_decision";
    rehash_preview(preview);
    auto result = validate_qt_desk_decision_snapshot(decision_row(preview), preview);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    EXPECT_EQ(result.value().selected_rows[0].quantity.to_string(), "5");
    EXPECT_EQ(result.value().selected_rows[1].quantity.to_string(), "1");
}

TEST(QtDeskDecisionTest, RefusesMismatchedReferenceOrDecisionState) {
    auto preview = preview_row(fixture_preview());
    auto decision = decision_row(preview);
    decision["payload"]["preview_payload_digest"] = std::string(64,'0');
    EXPECT_TRUE(validate_qt_desk_decision_snapshot(decision,preview).is_error());
    decision = decision_row(preview);
    decision["status"] = "pending_override";
    EXPECT_TRUE(validate_qt_desk_decision_snapshot(decision,preview).is_error());
    decision = decision_row(preview);
    preview["state"] = "pending_override";
    EXPECT_TRUE(validate_qt_desk_decision_snapshot(decision,preview).is_error());
    preview = preview_row(fixture_preview());
    preview["policy_version"] = "changed-policy";
    EXPECT_TRUE(validate_qt_desk_decision_snapshot(decision,preview).is_error());
}

TEST(QtDeskDecisionTest, RefusesTamperedPreviewAndNonselectedEvidence) {
    auto preview = preview_row(fixture_preview());
    auto decision = decision_row(preview);
    preview["payload"]["selection_rows"][0]["quantity_exact"] = "99";
    EXPECT_TRUE(validate_qt_desk_decision_snapshot(decision,preview).is_error());
    preview = preview_row(fixture_preview());
    preview["payload"]["evaluation"]["selected_costs"]["by_component"][0]["cash_cost_exact"] = "0.99";
    rehash_preview(preview);
    decision = decision_row(preview);
    EXPECT_TRUE(validate_qt_desk_decision_snapshot(decision,preview).is_error());
    preview = preview_row(fixture_preview());
    preview["payload"]["evaluation"]["selected_risk"]["evaluated_book_digest"] = std::string(64,'0');
    rehash_preview(preview);
    decision = decision_row(preview);
    EXPECT_TRUE(validate_qt_desk_decision_snapshot(decision,preview).is_error());
}

TEST(QtDeskDecisionTest, RefusesIncompleteCostCoverageAndWrongStream) {
    auto preview = preview_row(fixture_preview());
    preview["payload"]["evaluation"]["selected_costs"]["by_component"].erase(1);
    rehash_preview(preview);
    auto decision = decision_row(preview);
    EXPECT_TRUE(validate_qt_desk_decision_snapshot(decision,preview).is_error());
    preview = preview_row(fixture_preview());
    preview["payload"]["selection_rows"][0]["key"]["portfolio_type"] = "system";
    rehash_preview(preview);
    decision = decision_row(preview);
    EXPECT_TRUE(validate_qt_desk_decision_snapshot(decision,preview).is_error());
}

}  // namespace
}  // namespace trade_ngin
