#include <gtest/gtest.h>
#include "trade_ngin/apps/qt_equity_desk_cycle.hpp"
#include "trade_ngin/apps/qt_equity_cost_consumption.hpp"
#include "trade_ngin/apps/qt_equity_accounting_output.hpp"
#include "trade_ngin/apps/qt_equity_prior_finalization.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/git_version.hpp"
#include <stdexcept>
#include "trade_ngin/portfolio/qt_equity_proof.hpp"

namespace {using namespace trade_ngin;using J=nlohmann::json;
J decision(){return {{"decision_id","10000000-0000-0000-0000-000000000001"},{"book_id","BOOK"},{"source_day","2026-09-27"}};}
J input(){return {
 {"schema_version","qt-equity-accounting-input-empty-owner/v2"},{"calculation_version","qt-equity-main08b15c/v1"},
 {"decision_id",decision().at("decision_id")},{"book_id","BOOK"},{"source_day","2026-09-27"},
 {"accounting_input_id","20000000-0000-0000-0000-000000000001"},
 {"market_source_id","30000000-0000-0000-0000-000000000001"},{"market_source_digest",std::string(64,'a')},
 {"accounting_source_id","explicit-owned-governed-source"},
 {"prior_finalization_source_id","qt-finalization/40000000-0000-0000-0000-000000000001"},
 {"prior_finalization_digest",std::string(64,'b')},{"previous_day","2026-09-26"},
 {"timestamp","2026-09-27T00:00:00Z"},{"day_mode","open"},{"currency","USD"},
 {"previous_positions",J::array()},{"instruments",J::array()},{"actions",J::array()},
 {"previous_totals",J::array({{{"strategy_id","LIVE_EQUITY_MEAN_REVERSION"},{"initial_capital_exact","100000"},
   {"equity_exact","100000"},{"total_pnl_exact","0"},{"total_realized_pnl_exact","0"},
   {"total_transaction_costs_exact","0"},{"total_unrealized_pnl_exact","0"}}})},
 {"cost_config",{{"explicit_fee_per_contract","1.5"},{"min_adv","10000"},{"min_participation","0"},{"max_participation","1"}}}};}

std::string hash(const J& j,bool output=false){
 auto b=output?canonical_qt_desk_source_json(j):canonical_qt_desk_input_json(j);
 if(b.is_error())throw std::runtime_error("fixture_canonical_failed");
 auto h=qt_sha256_hex(b.value());if(h.is_error())throw std::runtime_error("fixture_hash_failed");return h.value();
}
J fixture(bool cumulative=false){
 auto d=decision(),in=input();d["model_publication_id"]="50000000-0000-0000-0000-000000000001";
 if(cumulative){auto& p=in["previous_totals"][0];p["total_realized_pnl_exact"]="12";p["total_transaction_costs_exact"]="2";p["total_pnl_exact"]="10";p["equity_exact"]="100010";}
 J a={{"schema_version","qt-input-authority/v1"},{"book_id",d.at("book_id")},{"source_day",d.at("source_day")},{"model_publication_id",d.at("model_publication_id")},
   {"snapshot_id",1},{"source_version","explicit-synthetic-snapshot/v1"},{"as_of","2026-09-27T00:00:00Z"},{"valid_until","2026-09-27T23:59:59Z"},
   {"content_digest",std::string(64,'c')},{"producer_id","owned-synthetic-producer"},{"policy_version","owned-synthetic-policy/v1"},{"policy_revision",1},
   {"policy_updated_at","2026-09-26T00:00:00Z"},{"evaluator_build",TRADE_NGIN_GIT_SHA},{"evaluator_sha256",std::string(64,'d')},
   {"evaluator_bundle_sha256",std::string(64,'e')},{"allowed_override_codes",J::array()}};
 auto produced=recompute_qt_equity_accounting_output(d,J::array(),in,a);
 if(produced.is_error())throw std::runtime_error("actual_empty_original_producer_failed");
 auto out=produced.value();
 J actions={{"schema_version","qt-equity-actions-source/v1"},{"book_id",d.at("book_id")},{"source_day","2026-09-28"},
   {"previous_day",d.at("source_day")},{"valuation_time","2026-09-28T00:00:00Z"},{"events",J::array()}};
 J market={{"schema_version","qt-equity-accounting-market-empty-owner/v2"},{"calculation_version","qt-equity-main08b15c/v1"},{"book_id",d.at("book_id")},
   {"source_day","2026-09-28"},{"model_publication_id","60000000-0000-0000-0000-000000000001"},{"previous_day",d.at("source_day")},
   {"valuation_time","2026-09-28T00:00:00Z"},{"day_mode","open"},{"currency","USD"},{"cost_config",in.at("cost_config")},
   {"instruments",J::array()},{"actions_source_id","synthetic-governed-empty-actions"},{"actions_source_digest",hash(actions)}};
 J before={{"positions",J::array()},{"live_results",out.at("live_results")},{"equity_curve",out.at("equity_curve")}};
 J provenance={{"finalization_id","70000000-0000-0000-0000-000000000001"},{"original_accounting_input_id",in.at("accounting_input_id")},
   {"original_run_result_digest",hash(out,true)},{"original_observation_digest",hash(out.at("observation"))},
   {"predecessor_finalization_source_id",in.at("prior_finalization_source_id")},{"predecessor_finalization_digest",in.at("prior_finalization_digest")},
   {"market_source_id","80000000-0000-0000-0000-000000000001"},{"market_source_digest",hash(market)},
   {"actions_source_id",market.at("actions_source_id")},{"actions_source_digest",market.at("actions_source_digest")},
   {"unchanged_execution_digest",hash(out.at("executions"))},
   {"policy_identity",{{"book_id",d.at("book_id")},{"purpose","execution"},{"version",1},{"producer_id","synthetic-execution-producer"},{"policy_version","synthetic-execution-policy/v1"}}},
   {"finalizer_authority",a}};
 return {{"d",d},{"in",in},{"out",out},{"market",market},{"actions",actions},{"before",before},{"provenance",provenance}};
}
Result<J> finalize(const J& f){return produce_qt_equity_prior_finalization(f.at("d"),f.at("in"),f.at("out"),f.at("market"),f.at("actions"),f.at("before"),f.at("provenance"));}
TEST(QtEmptyOwnerFinalization, ActualEmptyOriginalProducesExactMarkSuccessorWithoutRows){auto f=fixture();auto saved=f;auto r=finalize(f);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value().at("schema_version"),"qt-equity-desk-finalization-empty-owner/v2");EXPECT_TRUE(r.value().at("components").empty());EXPECT_EQ(r.value().at("before_financial"),f.at("before"));EXPECT_EQ(r.value().at("after_financial"),f.at("before"));EXPECT_EQ(r.value().at("engine_totals"),f.at("in").at("previous_totals"));EXPECT_EQ(f,saved);}
TEST(QtEmptyOwnerFinalization, ActualCumulativeRealizedAndCostsRemainRecorded){auto f=fixture(true);auto r=finalize(f);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value().at("engine_totals"),f.at("in").at("previous_totals"));EXPECT_EQ(r.value().at("after_financial"),f.at("before"));}
TEST(QtEmptyOwnerFinalization, NonemptyPriorPositionCannotDisappear){auto f=fixture();f["in"]["previous_positions"].push_back(J::object());EXPECT_TRUE(finalize(f).is_error());}
TEST(QtEmptyOwnerFinalization, NonemptyPriorInstrumentCannotBecomeEmpty){auto f=fixture();f["in"]["instruments"].push_back(J::object());EXPECT_TRUE(finalize(f).is_error());}
TEST(QtEmptyOwnerFinalization, PhysicalZeroRowCannotMasqueradeAsAbsent){auto f=fixture();f["before"]["positions"].push_back({{"quantity_exact","0"},{"average_price_exact","0"}});EXPECT_TRUE(finalize(f).is_error());}
TEST(QtEmptyOwnerFinalization, MissingCapitalAnchorRefuses){auto f=fixture();f["in"]["previous_totals"]=J::array();EXPECT_TRUE(finalize(f).is_error());}
TEST(QtEmptyOwnerFinalization, MissingActualCurveRefuses){auto f=fixture();f["before"]["equity_curve"]=J::array();EXPECT_TRUE(finalize(f).is_error());}
TEST(QtEmptyOwnerFinalization, ExtraFinancialOwnerRefuses){auto f=fixture();auto row=f["before"]["live_results"][0];row["strategy_id"]="FOREIGN";f["before"]["live_results"].push_back(row);EXPECT_TRUE(finalize(f).is_error());}
TEST(QtEmptyOwnerFinalization, WrongPriorDayRefuses){auto f=fixture();f["market"]["previous_day"]="2026-09-26";f["provenance"]["market_source_digest"]=hash(f.at("market"));EXPECT_TRUE(finalize(f).is_error());}
TEST(QtEmptyOwnerFinalization, ChangedAuthorityRefuses){auto f=fixture();f["provenance"]["finalizer_authority"]["evaluator_bundle_sha256"]=std::string(64,'f');EXPECT_TRUE(finalize(f).is_error());}
TEST(QtEmptyOwnerFinalization, NewActionCannotInventIncome){auto f=fixture();f["actions"]["events"].push_back(J::object());f["market"]["actions_source_digest"]=hash(f.at("actions"));f["provenance"]["actions_source_digest"]=hash(f.at("actions"));f["provenance"]["market_source_digest"]=hash(f.at("market"));EXPECT_TRUE(finalize(f).is_error());}
TEST(QtEmptyOwnerFinalization, ChangedFinancialRowsEvenAfterRehashRefuse){auto f=fixture();f["out"]["live_results"][0]["current_portfolio_value_exact"]="100001";f["before"]["live_results"]=f.at("out").at("live_results");f["provenance"]["original_run_result_digest"]=hash(f.at("out"),true);EXPECT_TRUE(finalize(f).is_error());}
TEST(QtEmptyOwnerFinalization, UntaggedEmptyOriginalRemainsUnsupported){auto f=fixture();f["in"]["schema_version"]="qt-equity-accounting-input/v1";f["out"]["schema_version"]="qt-equity-accounting/v1";f["market"]["schema_version"]="qt-equity-accounting-market/v1";EXPECT_TRUE(finalize(f).is_error());}
TEST(QtEmptyOwnerFinalization, FinalizationOnlyMarketIsNotEnabledForEmptyOwnerBooks){auto f=fixture();f["market"]["schema_version"]="qt-equity-finalization-market/v1";f["market"]["model_publication_id"]=nullptr;f["provenance"]["market_source_digest"]=hash(f.at("market"));EXPECT_TRUE(finalize(f).is_error());}

J accounting_wire(const J& f){
 auto d=f.at("d");d.erase("model_publication_id");
 J r={{"schema","qt-equity-proof/v1"},{"operation","recompute_equity_accounting"},
  {"evaluator_build",TRADE_NGIN_GIT_SHA},{"decision",d},{"selection_rows",J::array()},
  {"accounting_input",f.at("in")},{"producer_authority",f.at("out").at("producer_authority")}};
 r["context_fingerprint"]=hash(r);return r;
}
J finalization_wire(const J& f){
 auto bytes=canonical_qt_desk_source_json(f.at("out"));if(bytes.is_error())throw std::runtime_error("source_output_fixture_failed");
 J r={{"schema","qt-equity-finalization-proof/v1"},{"operation","recompute_equity_finalization"},
  {"evaluator_build",TRADE_NGIN_GIT_SHA},{"decision",f.at("d")},{"original_input",f.at("in")},
  {"original_output_json",bytes.value()},{"market_payload",f.at("market")},{"actions_payload",f.at("actions")},
  {"before_financial",f.at("before")},{"provenance",f.at("provenance")},
  {"finalizer_authority",f.at("out").at("producer_authority")}};
 r["context_fingerprint"]=hash(r,true);return r;
}
TEST(QtEmptyOwnerFinalization, OfflineAccountingRecomputesActualEmptyOutputWithoutSuppliedExpectedDigest){
 auto f=fixture(),r=accounting_wire(f),saved=r;auto proof=recompute_qt_equity_proof(r);
 ASSERT_TRUE(proof.is_ok());EXPECT_EQ(proof.value().at("output_digest"),hash(f.at("out"),true));
 EXPECT_EQ(proof.value().at("selection_digest"),hash(J::array()));EXPECT_EQ(r,saved);
}
TEST(QtEmptyOwnerFinalization, OfflineLegacyAccountingStillRejectsEmptySelection){
 auto r=accounting_wire(fixture());r["accounting_input"]["schema_version"]="qt-equity-accounting-input/v1";
 auto operands=r;operands.erase("context_fingerprint");r["context_fingerprint"]=hash(operands);
 EXPECT_TRUE(recompute_qt_equity_proof(r).is_error());
}
TEST(QtEmptyOwnerFinalization, OfflineExplicitEmptyAccountingRejectsNonemptySelection){
 auto r=accounting_wire(fixture());r["selection_rows"].push_back(J::object());
 auto operands=r;operands.erase("context_fingerprint");r["context_fingerprint"]=hash(operands);
 EXPECT_TRUE(recompute_qt_equity_proof(r).is_error());
}
TEST(QtEmptyOwnerFinalization, OfflineFinalizerRecomputesSameActualEmptySuccessor){
 auto f=fixture(),r=finalization_wire(f),saved=r;auto actual=finalize(f);ASSERT_TRUE(actual.is_ok());
 auto proof=recompute_qt_equity_finalization_proof(r);ASSERT_TRUE(proof.is_ok());
 EXPECT_EQ(proof.value().at("successor_digest"),hash(actual.value()));EXPECT_EQ(r,saved);
}
TEST(QtEmptyOwnerFinalization, OfflineFinalizerRejectsRehashedMixedOriginalSchema){
 auto f=fixture();f["out"]["schema_version"]="qt-equity-accounting/v1";
 f["provenance"]["original_run_result_digest"]=hash(f.at("out"),true);
 EXPECT_TRUE(recompute_qt_equity_finalization_proof(finalization_wire(f)).is_error());
}
TEST(QtEmptyOwnerFinalization, OfflineEmptyAccountingRejectsRehashedWrongCapital){
 auto r=accounting_wire(fixture());r["accounting_input"]["previous_totals"][0]["initial_capital_exact"]="100001";
 auto operands=r;operands.erase("context_fingerprint");r["context_fingerprint"]=hash(operands);
 EXPECT_TRUE(recompute_qt_equity_proof(r).is_error());
}
}
