#include "trade_ngin/portfolio/qt_evaluation.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "qt_test_build_identity.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace trade_ngin {
namespace {
TEST(QtEvalSerializeTest, PerformedIterationsObserveBodiesWithoutChangingLegacyResults) {
    for(bool buffering:{false,true}){
        DynamicOptConfig config;config.max_iterations=1;config.use_buffering=buffering;
        config.cost_penalty_scalar=0;config.convergence_threshold=0;
        DynamicOptimizer optimizer(config);OptimizationTrace trace;
        const auto plain=optimizer.optimize({0},{1},{0},{0.1},{{1}});
        const auto observed=optimizer.optimize({0},{1},{0},{0.1},{{1}},&trace);
        ASSERT_TRUE(plain.is_ok());ASSERT_TRUE(observed.is_ok());
        ASSERT_TRUE(trace.solver_iterations);EXPECT_EQ(*trace.solver_iterations,1);
        EXPECT_EQ(plain.value().positions,observed.value().positions);
        EXPECT_EQ(plain.value().iterations,observed.value().iterations);
        EXPECT_EQ(plain.value().tracking_error,observed.value().tracking_error);
        EXPECT_EQ(plain.value().cost_penalty,observed.value().cost_penalty);
        EXPECT_EQ(plain.value().converged,observed.value().converged);
        if(!buffering)EXPECT_EQ(observed.value().iterations,2); // Preserve legacy result semantics.
    }
}

TEST(QtEvalSerializeTest, UnavailableEvidenceNeverLooksEvaluated) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path() /
                         "contracts" / "qt-eval-v1.json";
    std::ifstream file(fixture, std::ios::binary);
    ASSERT_TRUE(file.good());
    const auto request_json = trade_ngin::test::fixture_for_compiled_build(
        nlohmann::json::parse(file).at("selected_book"));
    const auto request = parse_qt_evaluation_request(request_json.dump());
    ASSERT_TRUE(request.is_ok());
    const auto overlay = overlay_component_book(request.value().context, request.value().proposal);
    ASSERT_TRUE(overlay.is_ok());

    QtEvaluation evaluation;
    evaluation.operation = QtEvaluationOperation::SelectedBook;
    evaluation.evaluator_build = request.value().evaluator_build;
    evaluation.context_fingerprint = request.value().context_fingerprint;
    evaluation.evaluated_book = overlay.value();
    evaluation.evaluated_book_digest = "170528e9e3a72c56773ceeba05ed1762eb4a35ff1a35d0af8633e35039ee99df";
    evaluation.optimizer_status = QtEvidenceStatus::Disabled;
    evaluation.risk_status = QtEvidenceStatus::Unavailable;
    evaluation.cost_status = QtEvidenceStatus::Unavailable;
    evaluation.risk_config_source_id = request.value().risk_config_source_id;
    evaluation.diagnostics = {"risk:missing_market_source", "cost:missing_cost_source"};

    auto encoded = serialize_qt_evaluation(evaluation);
    ASSERT_TRUE(encoded.is_ok()) << encoded.error()->what();
    const auto output = nlohmann::json::parse(encoded.value());
    EXPECT_EQ(output.at("schema"), "qt-eval/v1");
    EXPECT_EQ(output.at("evaluated_book_digest"), evaluation.evaluated_book_digest);
    EXPECT_EQ(output.at("evaluated_book").size(), 2);
    EXPECT_EQ(output.at("completeness"), "unavailable");
    EXPECT_EQ(output.at("optimizer").at("status"), "disabled");
    EXPECT_TRUE(output.at("optimizer").at("actual_iterations").is_null());
    EXPECT_EQ(output.at("selected_risk").at("status"), "unavailable");
    EXPECT_TRUE(output.at("selected_risk").at("passed").is_null());
    EXPECT_TRUE(output.at("selected_costs").at("total_exact").is_null());
    evaluation.operation=QtEvaluationOperation::DraftDiagnostic;
    evaluation.optimizer_status=QtEvidenceStatus::Evaluated;
    evaluation.optimizer_config_source_id="synthetic-optimizer";
    ComponentOptimizerEvaluation optimizer{};
    optimizer.evaluated_book=overlay.value();
    optimizer.optimization.iterations=17;
    optimizer.trace.solver_iterations=17;
    optimizer.optimization.tracking_error=0;
    optimizer.optimization.cost_penalty=0;
    evaluation.optimizer=optimizer;
    auto counted=serialize_qt_evaluation(evaluation);ASSERT_TRUE(counted.is_ok());
    EXPECT_EQ(nlohmann::json::parse(counted.value()).at("optimizer").at("actual_iterations"),17);
    evaluation.optimizer->trace.solver_iterations=-1;
    EXPECT_TRUE(serialize_qt_evaluation(evaluation).is_error());
}

TEST(QtEvalSerializeTest, RejectsMismatchedSelectedDigestAndFabricatedStage) {
    QtEvaluation evaluation;
    evaluation.operation = QtEvaluationOperation::SelectedBook;
    evaluation.evaluator_build = TRADE_NGIN_GIT_SHA;
    evaluation.context_fingerprint = std::string(64, '1');
    evaluation.evaluated_book_digest = std::string(64, '0');
    evaluation.optimizer_status = QtEvidenceStatus::Disabled;
    evaluation.risk_status = QtEvidenceStatus::Evaluated;
    evaluation.cost_status = QtEvidenceStatus::Unavailable;
    EXPECT_TRUE(serialize_qt_evaluation(evaluation).is_error());
}

}  // namespace
}  // namespace trade_ngin
