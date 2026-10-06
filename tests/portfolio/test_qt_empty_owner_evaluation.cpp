#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/portfolio/qt_evaluation.hpp"
#include "trade_ngin/git_version.hpp"
#include <algorithm>
#include <gtest/gtest.h>

namespace trade_ngin { namespace {
using J=nlohmann::json;
J request(const char* operation="selected_book") {
    J input={
      {"schema","qt-eval-empty-owner/v2"},{"operation",operation},
      {"evaluator_build",TRADE_NGIN_GIT_SHA},{"context_fingerprint",std::string(64,'a')},
      {"risk_config_source_id","explicit-synthetic-governed-policy"},
      {"context",{{"portfolio_id","EQ_BOOK"},{"date","2026-09-27"},
          {"portfolio_type","qt_proposal"},{"revision","actual-empty-owner-revision"},{"slots",J::array()}}},
      {"proposal",{{"expected_portfolio_id","EQ_BOOK"},{"expected_date","2026-09-27"},
          {"expected_portfolio_type","qt_proposal"},{"expected_revision","actual-empty-owner-revision"},
          {"quantities",J::array()}}},
      {"risk_inputs",{{"expected_portfolio_id","EQ_BOOK"},{"expected_date","2026-09-27"},
          {"expected_portfolio_type","qt_proposal"},{"expected_revision","actual-empty-owner-revision"},
          {"market_snapshot_id","explicit-empty-market-source"},{"valuation_time","2026-09-27T00:00:00Z"},
          {"capital_currency","USD"},{"valuations",J::array()},
          {"expected_observation_times",J::array()},{"closes",J::array()}}},
      {"risk_config",{{"var_limit","0.2"},{"jump_risk_limit","0.2"},{"max_correlation","0.9"},
          {"corr_shock_threshold","0.65"},{"jump_shock_threshold","0.75"},
          {"max_gross_leverage","2"},{"max_net_leverage","1"},{"confidence_level","0.95"},
          {"lookback_period",252},{"capital_exact","100000"},{"version","actual-explicit-policy-version"}}},
      {"quantity_rules",J::array()},{"component_cost_inputs",J::array()}};
    if(std::string(operation)=="draft_diagnostic")
      input["optimizer_policy"]={{"enabled",false},{"config_source_id","explicit-disabled-policy"}};
    return input;
}
void evaluated(const J& input) {
    auto decoded=parse_qt_evaluation_request(input.dump());
    ASSERT_TRUE(decoded.is_ok())<<(decoded.error()?decoded.error()->what():"");
    auto out=evaluate_qt_request(decoded.value());
    ASSERT_TRUE(out.is_ok())<<(out.error()?out.error()->what():"");
    EXPECT_EQ(out.value().optimizer_status,QtEvidenceStatus::Disabled);
    EXPECT_FALSE(out.value().optimizer.has_value());
    EXPECT_EQ(out.value().risk_status,QtEvidenceStatus::Evaluated);
    EXPECT_EQ(out.value().cost_status,QtEvidenceStatus::Evaluated);
    ASSERT_TRUE(out.value().risk.has_value());
    ASSERT_TRUE(out.value().selected_costs.has_value());
    EXPECT_TRUE(out.value().risk->bindings.empty());
    EXPECT_TRUE(out.value().risk->evaluated_inputs.closes.empty());
    EXPECT_TRUE(out.value().risk->evaluated_inputs.expected_observation_times.empty());
    EXPECT_TRUE(out.value().risk->evaluated_book.components.empty());
    EXPECT_FALSE(out.value().risk->risk.risk_exceeded);
    EXPECT_EQ(out.value().risk->risk.gross_leverage,0.0);
    EXPECT_EQ(out.value().risk->risk.net_leverage,0.0);
    EXPECT_TRUE(out.value().selected_costs->by_component.empty());
    EXPECT_EQ(out.value().selected_costs->total_cash_cost.to_string(),"0");
    EXPECT_NE(std::find(out.value().diagnostics.begin(),out.value().diagnostics.end(),
        "risk:empty_owner_no_instrument_observations;correlation_history_not_applicable"),
        out.value().diagnostics.end());
    EXPECT_EQ(input.at("risk_config").at("capital_exact"),"100000");
}
void refused(const J& input) {
    auto decoded=parse_qt_evaluation_request(input.dump());
    if(decoded.is_error())return;
    auto out=evaluate_qt_request(decoded.value());
    if(out.is_error())return;
    EXPECT_NE(out.value().risk_status,QtEvidenceStatus::Evaluated);
    EXPECT_FALSE(out.value().risk.has_value());
}
TEST(QtEmptyOwnerEvaluation, SelectedBookUsesActualEmptyRiskAndCostKernels){evaluated(request());}
TEST(QtEmptyOwnerEvaluation, DraftDiagnosticPreservesDisabledOptimizer){evaluated(request("draft_diagnostic"));}
TEST(QtEmptyOwnerEvaluation, LegacyEmptyBookDoesNotAcquireV2Authority){auto in=request();in["schema"]="qt-eval/v1";refused(in);}
TEST(QtEmptyOwnerEvaluation, NonemptyHistoryIsOutsideEmptyInstrumentScope){auto in=request();in["risk_inputs"]["expected_observation_times"]={"2026-09-26T00:00:00Z"};refused(in);}
TEST(QtEmptyOwnerEvaluation, NonemptyMarksAreRefused){auto in=request();in["risk_inputs"]["valuations"]={{{"instrument",{{"instrument_type","EQUITY"},{"symbol","SYN"}}},{"mark_as_of","2026-09-27T00:00:00Z"},{"mark","10"},{"price_multiplier","1"},{"quote_currency","USD"}}};refused(in);}
TEST(QtEmptyOwnerEvaluation, UnexpectedFieldsAreClosed){auto in=request();in["trusted_empty_owner"]=true;refused(in);}
TEST(QtEmptyOwnerEvaluation, EnabledOptimizerCannotUseEmptyOwnerMode){auto in=request("draft_diagnostic");in["optimizer_policy"]["enabled"]=true;refused(in);}
TEST(QtEmptyOwnerEvaluation, ZeroCapitalIsRefused){auto in=request();in["risk_config"]["capital_exact"]="0";refused(in);}
TEST(QtEmptyOwnerEvaluation, NegativeCapitalIsRefused){auto in=request();in["risk_config"]["capital_exact"]="-1";refused(in);}
TEST(QtEmptyOwnerEvaluation, ZeroConfidenceIsRefused){auto in=request();in["risk_config"]["confidence_level"]="0";refused(in);}
TEST(QtEmptyOwnerEvaluation, UnitConfidenceIsRefused){auto in=request();in["risk_config"]["confidence_level"]="1";refused(in);}
TEST(QtEmptyOwnerEvaluation, NegativeLimitIsRefused){auto in=request();in["risk_config"]["var_limit"]="-0.2";refused(in);}
TEST(QtEmptyOwnerEvaluation, MissingShockThresholdIsRefused){auto in=request();in["risk_config"].erase("corr_shock_threshold");refused(in);}
TEST(QtEmptyOwnerEvaluation, ZeroShockThresholdIsRefused){auto in=request();in["risk_config"]["jump_shock_threshold"]="0";refused(in);}
TEST(QtEmptyOwnerEvaluation, InvalidLookbackIsRefused){auto in=request();in["risk_config"]["lookback_period"]=0;refused(in);}
TEST(QtEmptyOwnerEvaluation, MissingPolicyIdentityIsUnavailable){auto in=request();in["risk_config_source_id"]="";refused(in);}
TEST(QtEmptyOwnerEvaluation, MissingMarketIdentityIsUnavailable){auto in=request();in["risk_inputs"]["market_snapshot_id"]="";refused(in);}
TEST(QtEmptyOwnerEvaluation, MissingCurrencyIsUnavailable){auto in=request();in["risk_inputs"]["capital_currency"]="";refused(in);}
TEST(QtEmptyOwnerEvaluation, SuccessfulSerializedRiskExplainsNoInstrumentHistory){
    auto parsed=parse_qt_evaluation_request(request().dump());ASSERT_TRUE(parsed.is_ok());
    auto result=evaluate_qt_request(parsed.value());ASSERT_TRUE(result.is_ok());
    auto encoded=serialize_qt_evaluation(result.value());ASSERT_TRUE(encoded.is_ok());
    auto doc=J::parse(encoded.value());
    EXPECT_EQ(doc.at("selected_risk").at("status"),"evaluated");
    EXPECT_EQ(doc.at("selected_risk").at("diagnostics"),J::array({
        "This selection has no instruments; price and correlation history are not applicable."}));
    EXPECT_NE(std::find(result.value().diagnostics.begin(),result.value().diagnostics.end(),
        "risk:empty_owner_no_instrument_observations;correlation_history_not_applicable"),result.value().diagnostics.end());
}
TEST(QtEmptyOwnerEvaluation, LegacyUnavailableRiskDoesNotAcquireEmptyOwnerExplanation){
    auto input=request();input["schema"]="qt-eval/v1";
    auto parsed=parse_qt_evaluation_request(input.dump());ASSERT_TRUE(parsed.is_ok());
    auto result=evaluate_qt_request(parsed.value());ASSERT_TRUE(result.is_ok());
    auto encoded=serialize_qt_evaluation(result.value());ASSERT_TRUE(encoded.is_ok());
    auto doc=J::parse(encoded.value());EXPECT_EQ(doc.at("schema"),"qt-eval/v1");EXPECT_NE(doc.at("selected_risk").at("status"),"evaluated");
    EXPECT_TRUE(doc.at("selected_risk").at("diagnostics").empty());
}
TEST(QtEmptyOwnerEvaluation, SerializedSelectedBookMatchesExplicitEmptyRequestVersion){
    auto input=request();auto parsed=parse_qt_evaluation_request(input.dump());ASSERT_TRUE(parsed.is_ok());
    auto result=evaluate_qt_request(parsed.value());ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().input_mode,QtEvaluationInputMode::EmptyOwnerV2);
    auto encoded=serialize_qt_evaluation(result.value());ASSERT_TRUE(encoded.is_ok());
    EXPECT_EQ(J::parse(encoded.value()).at("schema"),input.at("schema"));
}
TEST(QtEmptyOwnerEvaluation, SerializedDraftMatchesExplicitEmptyRequestVersion){
    auto input=request("draft_diagnostic");auto parsed=parse_qt_evaluation_request(input.dump());ASSERT_TRUE(parsed.is_ok());
    auto result=evaluate_qt_request(parsed.value());ASSERT_TRUE(result.is_ok());
    auto encoded=serialize_qt_evaluation(result.value());ASSERT_TRUE(encoded.is_ok());
    EXPECT_EQ(J::parse(encoded.value()).at("schema"),input.at("schema"));
}
TEST(QtEmptyOwnerEvaluation, InvalidTypedInputModeIsNotSilentlySerializedAsLegacy){
    auto parsed=parse_qt_evaluation_request(request().dump());ASSERT_TRUE(parsed.is_ok());
    auto result=evaluate_qt_request(parsed.value());ASSERT_TRUE(result.is_ok());
    auto altered=result.value();altered.input_mode=static_cast<QtEvaluationInputMode>(99);
    EXPECT_TRUE(serialize_qt_evaluation(altered).is_error());
}
} }
