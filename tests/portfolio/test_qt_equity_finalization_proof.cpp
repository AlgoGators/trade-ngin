#include "trade_ngin/portfolio/qt_equity_proof.hpp"
#include "trade_ngin/apps/qt_equity_prior_finalization.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
using namespace trade_ngin;
namespace {
using J=nlohmann::json;
std::string bytes(const J& j){auto r=canonical_qt_desk_source_json(j);if(r.is_error())throw std::runtime_error("fixture source bytes");return r.value();}
std::string digest(const J& j){auto r=qt_sha256_hex(bytes(j));if(r.is_error())throw std::runtime_error("fixture hash");return r.value();}
J fixture(){std::ifstream f(std::filesystem::path(__FILE__).parent_path().parent_path()/"contracts/qt-equity-finalization-wire.json");if(!f.good())throw std::runtime_error("missing fixture");return J::parse(f);}
void fingerprint(J& r){auto copy=r;copy.erase("context_fingerprint");r["context_fingerprint"]=digest(copy);}
J calculated(const J& r){auto out=J::parse(r.at("original_output_json").get<std::string>());auto x=produce_qt_equity_prior_finalization(r.at("decision"),r.at("original_input"),out,r.at("market_payload"),r.at("actions_payload"),r.at("before_financial"),r.at("provenance"));if(x.is_error())throw std::runtime_error("actual finalizer fixture unavailable");return x.value();}
}
TEST(QtEquityFinalizationProof, ActualMarkArithmeticAndOriginalAuthorityAreBoundWithoutExpectedSuccessor) {
 auto r=fixture();const auto frozen=r;ASSERT_GT(r["original_output_json"].get<std::string>().size(),4096u);
 auto actual=calculated(r);ASSERT_EQ(actual["after_financial"]["positions"][0]["quantity_exact"],"0.5");
 ASSERT_EQ(actual["after_financial"]["live_results"][0]["current_portfolio_value_exact"],"1026.86");
 RecordProperty("actual_finalization_request_json",bytes(r));
 RecordProperty("actual_finalization_successor_json",bytes(actual));
 auto proof=recompute_qt_equity_finalization_proof(r);ASSERT_TRUE(proof.is_ok());const auto& p=proof.value();
 EXPECT_EQ(p.size(),11u);EXPECT_EQ(p["schema"],"qt-equity-finalization-proof/v1");
 EXPECT_EQ(p["operation"],"recompute_equity_finalization");EXPECT_EQ(p["context_fingerprint"],r["context_fingerprint"]);
 EXPECT_EQ(p["successor_digest"],digest(actual));EXPECT_EQ(p["market_digest"],digest(r["market_payload"]));
 EXPECT_EQ(p["original_output_digest"],digest(J::parse(r["original_output_json"].get<std::string>())));
 EXPECT_EQ(p["actions_digest"],digest(r["actions_payload"]));EXPECT_EQ(p["provenance_digest"],digest(r["provenance"]));
 EXPECT_EQ(p["before_financial_digest"],digest(r["before_financial"]));EXPECT_EQ(r,frozen);
}
TEST(QtEquityFinalizationProof, ChangedGovernedMarkComputesAnotherSuccessorWithoutResizing) {
 auto r=fixture();auto first=recompute_qt_equity_finalization_proof(r);ASSERT_TRUE(first.is_ok());
 for(auto name:{"reference","mark"})r["market_payload"]["instruments"][0][name]["price_model_number"]="17";
 r["provenance"]["market_source_digest"]=digest(r["market_payload"]);fingerprint(r);
 auto actual=calculated(r);EXPECT_EQ(actual["after_financial"]["live_results"][0]["current_portfolio_value_exact"],"1027.36");
 EXPECT_EQ(actual["after_financial"]["positions"][0]["quantity_exact"],"0.5");
 auto result=recompute_qt_equity_finalization_proof(r);ASSERT_TRUE(result.is_ok());
 EXPECT_EQ(result.value()["successor_digest"],digest(actual));EXPECT_NE(result.value()["successor_digest"],first.value()["successor_digest"]);
}
TEST(QtEquityFinalizationProof, ExpectedSuccessorAndUnknownFieldsAreRejected) {auto r=fixture();r["expected_successor"]=J::object();fingerprint(r);EXPECT_TRUE(recompute_qt_equity_finalization_proof(r).is_error());}
TEST(QtEquityFinalizationProof, DetachedContextFingerprintIsRejected) {auto r=fixture();r["context_fingerprint"]=std::string(64,'0');EXPECT_TRUE(recompute_qt_equity_finalization_proof(r).is_error());}
TEST(QtEquityFinalizationProof, AnotherFinalizerBundleCannotReplaceOriginalAuthority) {auto r=fixture();r["finalizer_authority"]["evaluator_bundle_sha256"]=std::string(64,'0');fingerprint(r);EXPECT_TRUE(recompute_qt_equity_finalization_proof(r).is_error());}
TEST(QtEquityFinalizationProof, OuterNumericFloatIsRejectedEvenWhenHashed) {auto r=fixture();r["finalizer_authority"]["snapshot_id"]=1.0;fingerprint(r);EXPECT_TRUE(recompute_qt_equity_finalization_proof(r).is_error());}
TEST(QtEquityFinalizationProof, DuplicateKeysInOriginalOutputCannotDisappearDuringParsing) {auto r=fixture();auto s=r["original_output_json"].get<std::string>();s.insert(1,"\"schema_version\":\"forged\",");r["original_output_json"]=s;fingerprint(r);EXPECT_TRUE(recompute_qt_equity_finalization_proof(r).is_error());}
TEST(QtEquityFinalizationProof, NoncanonicalOriginalOutputIsRejected) {auto r=fixture();r["original_output_json"]=" "+r["original_output_json"].get<std::string>();fingerprint(r);EXPECT_TRUE(recompute_qt_equity_finalization_proof(r).is_error());}
TEST(QtEquityFinalizationProof, OversizedOriginalOutputIsRejectedBeforeCanonicalFingerprinting) {auto r=fixture();r["original_output_json"]=std::string(1048577,' ');EXPECT_TRUE(recompute_qt_equity_finalization_proof(r).is_error());}
TEST(QtEquityFinalizationProof, OriginalOutputCannotChangeBehindItsRecordedDigest) {auto r=fixture();auto out=J::parse(r["original_output_json"].get<std::string>());out["live_results"][0]["total_transaction_costs_exact"]="0";r["original_output_json"]=bytes(out);fingerprint(r);EXPECT_TRUE(recompute_qt_equity_finalization_proof(r).is_error());}
TEST(QtEquityFinalizationProof, MissingGovernedActionsCannotBecomeNoActions) {auto r=fixture();r["actions_payload"]=nullptr;fingerprint(r);EXPECT_TRUE(recompute_qt_equity_finalization_proof(r).is_error());}
TEST(QtEquityFinalizationProof, FutureMarkCannotBeUsedForOriginalDay) {auto r=fixture();r["market_payload"]["instruments"][0]["mark"]["date"]="2026-09-26";r["provenance"]["market_source_digest"]=digest(r["market_payload"]);fingerprint(r);EXPECT_TRUE(recompute_qt_equity_finalization_proof(r).is_error());}
TEST(QtEquityFinalizationProof, RehashedFinancialFloatOutsideConsumptionIsRejected) {
 auto r=fixture();auto out=J::parse(r["original_output_json"].get<std::string>());
 out["distance"][0]["selected_quantity_exact"]=1.0;
 r["original_output_json"]=bytes(out);r["provenance"]["original_run_result_digest"]=digest(out);fingerprint(r);
 EXPECT_TRUE(recompute_qt_equity_finalization_proof(r).is_error());
}
TEST(QtEquityFinalizationProof, RehashedUnknownOriginalOutputFieldIsRejected) {
 auto r=fixture();auto out=J::parse(r["original_output_json"].get<std::string>());out["unrecognized"]=true;
 r["original_output_json"]=bytes(out);r["provenance"]["original_run_result_digest"]=digest(out);fingerprint(r);
 EXPECT_TRUE(recompute_qt_equity_finalization_proof(r).is_error());
}
TEST(QtEquityFinalizationProof, RehashedMissingOriginalOutputFieldIsRejected) {
 auto r=fixture();auto out=J::parse(r["original_output_json"].get<std::string>());out.erase("distance");
 r["original_output_json"]=bytes(out);r["provenance"]["original_run_result_digest"]=digest(out);fingerprint(r);
 EXPECT_TRUE(recompute_qt_equity_finalization_proof(r).is_error());
}
