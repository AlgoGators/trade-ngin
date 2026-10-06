#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/portfolio/qt_evaluation.hpp"
#include "qt_test_build_identity.hpp"

#include <gtest/gtest.h>
#include <fstream>
#include <iterator>
#include <filesystem>

namespace trade_ngin {
namespace {

TEST(QtWireTest, MatchesAllAuthoritativeCanonicalVectors) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path() /
                         "contracts" / "qt-workflow-v1.json";
    std::ifstream file(fixture, std::ios::binary);
    ASSERT_TRUE(file.good());
    const auto data = nlohmann::json::parse(file);
    ASSERT_EQ(data.at("canonical_vectors").size(), 5);
    for (const auto& vector : data.at("canonical_vectors")) {
        auto bytes = canonical_qt_bytes(vector.at("input"));
        ASSERT_TRUE(bytes.is_ok()) << bytes.error()->what();
        EXPECT_EQ(bytes.value(), vector.at("canonical_json").get<std::string>());
        auto digest = qt_digest_v1(vector.at("input"));
        ASSERT_TRUE(digest.is_ok()) << digest.error()->what();
        EXPECT_EQ(digest.value(), vector.at("sha256").get<std::string>());
    }
}

TEST(QtWireTest, SortsFullKeysAndRejectsDuplicateMembers) {
    nlohmann::json one = {{"key", {{"portfolio_id", "book"}, {"strategy_id", "a"},
        {"strategy_name", "A"}, {"date", "2026-09-25"}, {"symbol", "SYN"},
        {"portfolio_type", "qt"}}}, {"quantity_exact", "5"}};
    nlohmann::json two = one;
    two["key"]["strategy_id"] = "b";
    auto forward = canonical_qt_bytes({{"selection_rows", nlohmann::json::array({one, two})}});
    auto reversed = canonical_qt_bytes({{"selection_rows", nlohmann::json::array({two, one})}});
    ASSERT_TRUE(forward.is_ok());
    ASSERT_TRUE(reversed.is_ok());
    EXPECT_EQ(forward.value(), reversed.value());
    EXPECT_TRUE(canonical_qt_bytes({{"selection_rows", nlohmann::json::array({one, one})}}).is_error());
}

TEST(QtWireTest, BindsDraftRationaleWithPythonCanonicalTextAndNullParity) {
    // Digests are the Python QT canonical contract; omitting or changing the
    // rationale must not leave a saved draft's signed identity unchanged.
    nlohmann::json draft = {{"selection_rows", nlohmann::json::array()},
                            {"rationale", "Keep settled quantities"}};
    auto digest = qt_digest_v1(draft);
    ASSERT_TRUE(digest.is_ok()) << digest.error()->what();
    EXPECT_EQ(digest.value(), "32df91d7fca4c62114bf62062a3ae2814a1337002c4104f2861833fa4946aab6");
    EXPECT_EQ(canonical_qt_bytes(draft).value(),
              R"({"rationale":"Keep settled quantities","selection_rows":[]})");
    draft["rationale"] = "Different choice";
    ASSERT_TRUE(qt_digest_v1(draft).is_ok());
    EXPECT_NE(qt_digest_v1(draft).value(), digest.value());
    draft["rationale"] = nullptr;
    auto nullable = qt_digest_v1(draft);
    ASSERT_TRUE(nullable.is_ok()) << nullable.error()->what();
    EXPECT_EQ(nullable.value(), "450a129f657011777368aba1c8be6bea179bd1154e4f68d6286019514e050eb6");
    draft.erase("rationale");
    EXPECT_EQ(qt_digest_v1(draft).value(), "4a3bc494577c19c2b2e75a4601089a9dad54972213487d1729d8d18f0ae0d771");
    draft["rationale"] = 1;
    EXPECT_TRUE(qt_digest_v1(draft).is_error());
    draft["rationale"] = std::string(4097, 'x');
    EXPECT_TRUE(qt_digest_v1(draft).is_error());
}

TEST(QtWireTest, DistinguishesAdjacentDecimal8AndRejectsPaddedForm) {
    const auto a = parse_qt_quantity_exact("92233720368.12345678");
    const auto b = parse_qt_quantity_exact("92233720368.12345677");
    ASSERT_TRUE(a.is_ok());
    ASSERT_TRUE(b.is_ok());
    EXPECT_NE(a.value().raw_value(), b.value().raw_value());
    EXPECT_TRUE(parse_qt_quantity_exact("1.25000000").is_error());
    EXPECT_EQ(parse_qt_quantity_exact("1.25").value().to_string(), "1.25");
    EXPECT_TRUE(parse_qt_quantity_exact("-0").is_error());
}

TEST(QtWireTest, RejectsUnknownAndNoncanonicalExactFields) {
    EXPECT_TRUE(canonical_qt_bytes({{"quantity_exact", "1.25000000"}}).is_error());
    EXPECT_TRUE(canonical_qt_bytes({{"quantity_exact", 1.25}}).is_error());
    EXPECT_TRUE(canonical_qt_bytes({{"unknown_field", "value"}}).is_error());
    EXPECT_TRUE(canonical_qt_bytes({{"quantity_exact", "1.25"}, {"symbol", "\xc3\xa9\n"}}).is_ok());
}

TEST(QtWireTest, RejectsDuplicateAndTrailingJsonBeforeCanonicalization) {
    EXPECT_TRUE(parse_qt_json("{\"quantity_exact\":\"1\",\"quantity_exact\":\"2\"}").is_error());
    EXPECT_TRUE(parse_qt_json("{\"quantity_exact\":\"1\"} {}").is_error());
    EXPECT_TRUE(parse_qt_json("{\"quantity_exact\":1.0}").is_error());
    EXPECT_TRUE(parse_qt_json(std::string("{\"symbol\":\"") + '\xff' + "\"}").is_error());
    auto good = parse_qt_json(" {\"symbol\":\"ES\",\"quantity_exact\":\"1.25\"} \n");
    ASSERT_TRUE(good.is_ok());
    EXPECT_EQ(qt_digest_v1(good.value()).value(), "ffb1f43d93c320c8dc8b82e62505c7c9e129335d008eb86360d2e58b45259087");
}

TEST(QtWireTest, HashUsesStandardSha256) {
    EXPECT_EQ(qt_sha256_hex("").value(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(qt_sha256_hex("abc").value(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(QtWireTest, ParsesCompleteSelectedAndDiagnosticSnapshots) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path() /
                         "contracts" / "qt-eval-v1.json";
    std::ifstream file(fixture, std::ios::binary);
    ASSERT_TRUE(file.good());
    const auto examples = trade_ngin::test::fixture_for_compiled_build(
        nlohmann::json::parse(file));
    auto selected = parse_qt_evaluation_request(examples.at("selected_book").dump());
    ASSERT_TRUE(selected.is_ok()) << selected.error()->what();
    EXPECT_EQ(selected.value().operation, QtEvaluationOperation::SelectedBook);
    EXPECT_EQ(selected.value().context.slots.size(), 2);
    EXPECT_EQ(selected.value().proposal.quantities.at(1).quantity.to_string(), "1");
    EXPECT_EQ(selected.value().component_cost_inputs.size(), 2);
    EXPECT_FALSE(selected.value().optimizer_inputs.has_value());
    auto diagnostic = parse_qt_evaluation_request(examples.at("draft_diagnostic").dump());
    ASSERT_TRUE(diagnostic.is_ok()) << diagnostic.error()->what();
    EXPECT_EQ(diagnostic.value().operation, QtEvaluationOperation::DraftDiagnostic);
    EXPECT_TRUE(diagnostic.value().optimizer_enabled);
    ASSERT_TRUE(diagnostic.value().optimizer_config.has_value());
    EXPECT_EQ(diagnostic.value().optimizer_config->max_iterations, 100);
}

TEST(QtWireTest, RejectsMissingAndMismatchedEvaluatorEvidence) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path() /
                         "contracts" / "qt-eval-v1.json";
    std::ifstream file(fixture, std::ios::binary);
    ASSERT_TRUE(file.good());
    const auto examples = trade_ngin::test::fixture_for_compiled_build(
        nlohmann::json::parse(file));
    auto selected = examples.at("selected_book");
    selected["evaluator_build"] = "wrong-build";
    EXPECT_TRUE(parse_qt_evaluation_request(selected.dump()).is_error());
    selected = examples.at("selected_book");
    selected["proposal"]["quantities"][0]["key"]["strategy_id"] = "other-component";
    EXPECT_TRUE(parse_qt_evaluation_request(selected.dump()).is_error());
    selected = examples.at("selected_book");
    selected["risk_inputs"]["expected_revision"] = "stale-risk-snapshot";
    EXPECT_TRUE(parse_qt_evaluation_request(selected.dump()).is_error());
    selected = examples.at("selected_book");
    selected["component_cost_inputs"][0]["cash_cost_per_increment_exact"] = 0.01;
    EXPECT_TRUE(parse_qt_evaluation_request(selected.dump()).is_error());
    selected = examples.at("selected_book");
    selected["risk_inputs"]["valuation_time"] = "2026-09-25T12:00:00.1234567891Z";
    EXPECT_TRUE(parse_qt_evaluation_request(selected.dump()).is_error());
    selected = examples.at("selected_book");
    selected["component_cost_inputs"] = nlohmann::json::array();
    auto missing_cost = parse_qt_evaluation_request(selected.dump());
    ASSERT_TRUE(missing_cost.is_ok()) << missing_cost.error()->what();
    EXPECT_TRUE(missing_cost.value().component_cost_inputs.empty());
    auto diagnostic = examples.at("draft_diagnostic");
    diagnostic.erase("optimizer_inputs");
    EXPECT_TRUE(parse_qt_evaluation_request(diagnostic.dump()).is_error());
    diagnostic = examples.at("draft_diagnostic");
    diagnostic["optimizer_policy"]["enabled"] = false;
    diagnostic.erase("optimizer_inputs");
    diagnostic.erase("optimizer_config");
    auto disabled = parse_qt_evaluation_request(diagnostic.dump());
    ASSERT_TRUE(disabled.is_ok()) << disabled.error()->what();
    EXPECT_FALSE(disabled.value().optimizer_enabled);
}

TEST(QtWireTest, CanonicalEvalRequestSortsCompleteComponentKeys) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path() /
                         "contracts" / "qt-eval-v1.json";
    std::ifstream file(fixture, std::ios::binary);
    ASSERT_TRUE(file.good());
    const auto selected = trade_ngin::test::fixture_for_compiled_build(
        nlohmann::json::parse(file).at("selected_book"));
    auto reversed = selected;
    std::reverse(reversed["context"]["slots"].begin(), reversed["context"]["slots"].end());
    std::reverse(reversed["proposal"]["quantities"].begin(), reversed["proposal"]["quantities"].end());
    std::reverse(reversed["component_cost_inputs"].begin(), reversed["component_cost_inputs"].end());
    auto first = canonical_qt_eval_bytes(selected);
    auto second = canonical_qt_eval_bytes(reversed);
    ASSERT_TRUE(first.is_ok());
    ASSERT_TRUE(second.is_ok());
    EXPECT_EQ(first.value(), second.value());
    EXPECT_NE(first.value().find("synthetic-book-A"), std::string::npos);
    EXPECT_TRUE(canonical_qt_eval_bytes({{"schema", "qt-eval/v1"}, {"unknown", true}}).is_error());
}

}  // namespace
}  // namespace trade_ngin
