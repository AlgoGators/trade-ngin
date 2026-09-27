#include <gtest/gtest.h>
#include <stdexcept>
#include "trade_ngin/apps/qt_equity_prior_finalization.hpp"
#include "trade_ngin/apps/qt_equity_accounting_output.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
using namespace trade_ngin;
namespace {
using J=nlohmann::json;
J fixture(){return J::parse(R"fixture({"accounting_input":{"accounting_input_id":"20000000-0000-0000-0000-000000000001","accounting_source_id":"owned-synthetic-equity-accounting","actions":[],"book_id":"BOOK","calculation_version":"qt-equity-main08b15c/v1","cost_config":{"explicit_fee_per_contract":"1.5","max_participation":"1","min_adv":"10000","min_participation":"0"},"currency":"USD","day_mode":"open","decision_id":"10000000-0000-0000-0000-000000000001","instruments":[{"asset_type":"EQUITY","cost_evidence":{"adv_model_number":"1000000","date":"2026-09-24","source_digest":"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","source_id":"owned-synthetic-cost-history","volatility_multiplier_model_number":"1"},"cost_parameters":{"apply_regulatory_fees":false,"baseline_spread_ticks":"1","commission_per_unit":"0.005","finra_taf_cap_per_trade":"9.79","finra_taf_per_share":"0.000195","max_commission_pct":"0.01","max_commission_per_order":"100","max_impact_bps":"100","max_spread_ticks":"10","max_total_implicit_bps":"0","min_commission_per_order":"1","min_spread_ticks":"1","point_value":"1","sec_fee_per_million":"20.6","spread_cost_multiplier":"0.5","tick_constrained":false,"tick_size":"0.01"},"mark":{"date":"2026-09-24","price_frame_id":"adjusted-1","price_model_number":"14","source_digest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","source_id":"owned-synthetic-close"},"reference":{"date":"2026-09-24","price_frame_id":"adjusted-1","price_model_number":"14","source_digest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","source_id":"owned-synthetic-close"},"symbol":"SYN"}],"market_source_digest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","market_source_id":"30000000-0000-0000-0000-000000000001","previous_day":"2026-09-24","previous_positions":[{"average_price_exact":"10","basis_evidence":{"formed_day":"2026-09-20","price_frame_id":"adjusted-1","source_digest":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","source_id":"owned-prior-position"},"daily_realized_pnl_exact":"9","daily_unrealized_pnl_exact":"6","key":{"date":"2026-09-24","portfolio_id":"BOOK","portfolio_type":"qt","strategy_id":"ENGINE","strategy_name":"owner-a","symbol":"SYN"},"last_update":"2026-09-24T00:00:00Z","quantity_exact":"1.5"}],"previous_totals":[{"equity_exact":"1026","initial_capital_exact":"1000","strategy_id":"ENGINE","total_pnl_exact":"26","total_realized_pnl_exact":"30","total_transaction_costs_exact":"10","total_unrealized_pnl_exact":"6"}],"prior_finalization_digest":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","prior_finalization_source_id":"qt-finalization/40000000-0000-0000-0000-000000000001","schema_version":"qt-equity-accounting-input/v1","source_day":"2026-09-25","timestamp":"2026-09-25T00:00:00Z"},"decision":{"book_id":"BOOK","decision_id":"10000000-0000-0000-0000-000000000001","model_publication_id":"50000000-0000-0000-0000-000000000001","source_day":"2026-09-25"},"producer_authority":{"allowed_override_codes":[],"as_of":"2026-09-25T00:00:00Z","book_id":"BOOK","content_digest":"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee","evaluator_build":"local-qt-controlled","evaluator_bundle_sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","evaluator_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","model_publication_id":"50000000-0000-0000-0000-000000000001","policy_revision":1,"policy_updated_at":"2026-09-20T00:00:00Z","policy_version":"owned-synthetic-evaluation-policy/v1","producer_id":"owned-synthetic-evaluator","schema_version":"qt-input-authority/v1","snapshot_id":1,"source_day":"2026-09-25","source_version":"owned-synthetic-evaluation/v1","valid_until":"2026-09-26T00:00:00Z"},"selection":[{"asset_type":"EQUITY","average_price_exact":"10","basis_status":"preserved_source","editable":true,"key":{"date":"2026-09-25","portfolio_id":"BOOK","portfolio_type":"qt_proposal","strategy_id":"ENGINE","strategy_name":"owner-a","symbol":"SYN"},"origin":"qt_draft","quantity_exact":"0.5"}]})fixture");}
std::string hash(const J& j,bool output=false){auto b=output?canonical_qt_desk_source_json(j):canonical_qt_desk_input_json(j);if(b.is_error())throw std::runtime_error("fixture canonicalization");auto h=qt_sha256_hex(b.value());if(h.is_error())throw std::runtime_error("fixture digest");return h.value();}
struct Case {J d,in,out,market,actions,before,provenance;};
Case make(std::string mode="executed") {
 auto f=fixture();auto in=f.at("accounting_input"),s=f.at("selection");
 if(mode=="quiet"||mode=="noneditable") {s[0]["quantity_exact"]="1.5";s[0]["average_price_exact"]="10";if(mode=="noneditable")s[0]["editable"]=false;}
 if(mode=="flat")s[0]["quantity_exact"]="0";
 if(mode=="short")s[0]["quantity_exact"]="-0.5";
 if(mode=="split"||mode=="dividend") {
  auto& prior=in["previous_positions"][0];
  s[0]["quantity_exact"]=mode=="split"?"3":"1.5";
  J event={{"key",s[0]["key"]},{"source_id","owned-original-action"},{"source_digest",std::string(64,'e')},
   {"type",mode=="split"?"SPLIT":"DIVIDEND"},{"ex_date",in["previous_day"]},{"value_model_number",mode=="split"?"2":"1"},
   {"basis_provenance","formed_on_or_before_ex_date"},{"basis_provenance_evidence",prior["basis_evidence"]["source_id"]},
   {"frame_before","adjusted-1"},{"frame_after","adjusted-2"},{"raw_close_model_number",nullptr},{"eligible_quantity_exact",nullptr}};
  event["key"]["portfolio_type"]="qt";
  if(mode=="dividend"){event["raw_close_model_number"]="10";event["eligible_quantity_exact"]="1.5";}
  in["actions"]=J::array({event});
  for(auto name:{"reference","mark"}) {in["instruments"][0][name]["price_frame_id"]="adjusted-2";in["instruments"][0][name]["price_model_number"]=mode=="split"?"7":"10";}
 }
 if(mode=="two_owners") {
  auto p=in["previous_positions"][0],choice=s[0];p["key"]["strategy_name"]="owner-b";p["quantity_exact"]="2";p["daily_unrealized_pnl_exact"]="8";
  p["basis_evidence"]["source_id"]="owned-owner-b-basis";choice["key"]["strategy_name"]="owner-b";choice["quantity_exact"]="2";choice["average_price_exact"]="10";
  in["previous_positions"].push_back(p);s.push_back(choice);in["previous_totals"][0]["total_unrealized_pnl_exact"]="14";in["previous_totals"][0]["total_pnl_exact"]="34";in["previous_totals"][0]["equity_exact"]="1034";
 }
 auto original=recompute_qt_equity_accounting_output(f["decision"],s,in,f["producer_authority"]);
 if(original.is_error())throw std::runtime_error("actual original producer must succeed before RED");
 auto out=original.value();const auto day="2026-09-26";auto instruments=in["instruments"];
 for(auto& item:instruments)for(auto name:{"reference","mark"}){item[name]["date"]="2026-09-25";item[name]["source_id"]="owned-S-close";item[name]["source_digest"]=std::string(64,'f');item[name]["price_model_number"]=mode=="split"?"9":"16";}
 for(auto& item:instruments)item["cost_evidence"]["date"]="2026-09-25";
 J actions={{"schema_version","qt-equity-actions-source/v1"},{"book_id","BOOK"},{"source_day",day},{"previous_day","2026-09-25"},{"valuation_time","2026-09-26T00:00:00Z"},{"events",J::array()}};
 J market={{"schema_version","qt-equity-accounting-market/v1"},{"calculation_version","qt-equity-main08b15c/v1"},{"book_id","BOOK"},{"source_day",day},
 {"model_publication_id","a0000000-0000-4000-8000-000000000001"},{"previous_day","2026-09-25"},{"valuation_time","2026-09-26T00:00:00Z"},{"day_mode","open"},{"currency","USD"},
 {"cost_config",in["cost_config"]},{"instruments",instruments},{"actions_source_id","qt-actions/a0000000-0000-4000-8000-000000000002"},{"actions_source_digest",hash(actions)}};
 J positions=J::array();for(const auto& fill:out["observation"]["fills"]){J p={{"key",fill["key"]},{"quantity_exact",fill["selected_quantity_exact"]}};for(auto name:{"average_price_exact","daily_realized_pnl_exact","daily_unrealized_pnl_exact","last_update"})p[name]=fill[name];positions.push_back(p);}
 J before={{"positions",positions},{"live_results",out["live_results"]},{"equity_curve",out["equity_curve"]}};
 J provenance={{"finalization_id","b0000000-0000-4000-8000-000000000001"},{"original_accounting_input_id",in["accounting_input_id"]},
 {"original_run_result_digest",hash(out,true)},{"original_observation_digest",hash(out["observation"])},
 {"predecessor_finalization_source_id",in["prior_finalization_source_id"]},{"predecessor_finalization_digest",in["prior_finalization_digest"]},
 {"market_source_id","a0000000-0000-4000-8000-000000000003"},{"market_source_digest",hash(market)},
 {"actions_source_id",market["actions_source_id"]},{"actions_source_digest",market["actions_source_digest"]},
 {"unchanged_execution_digest",hash(out["executions"])},{"finalizer_authority",out["producer_authority"]},{"policy_identity",{{"book_id","BOOK"},{"purpose","execution"},{"version",1},{"producer_id","owned-execution"},{"policy_version","owned-policy"}}}};
 return {f["decision"],in,out,market,actions,before,provenance};
}
auto invoke(const Case& c){return produce_qt_equity_prior_finalization(c.d,c.in,c.out,c.market,c.actions,c.before,c.provenance);}
J new_action(const Case& c,const char* ex_date) {
 auto key=c.out["observation"]["fills"][0]["key"];key["date"]=c.market["source_day"];
 return {{"key",key},{"type","SPLIT"},{"ex_date",ex_date},{"value_model_number","2"},
 {"basis_provenance","formed_on_or_before_ex_date"},{"basis_provenance_evidence","actual-original-position"},
 {"frame_before",c.market["instruments"][0]["mark"]["price_frame_id"]},{"frame_after","unadmitted-new-frame"},
 {"raw_close_model_number",nullptr},{"eligible_quantity_exact",nullptr}};
}
void bind_changed_sources(Case& c) {
 if(!c.actions.is_null()){c.market["actions_source_digest"]=hash(c.actions);c.provenance["actions_source_digest"]=hash(c.actions);}
 c.provenance["market_source_digest"]=hash(c.market);
}
void refused(const Case& c){const auto old=J::array({c.d,c.in,c.out,c.market,c.actions,c.before,c.provenance});EXPECT_TRUE(invoke(c).is_error());EXPECT_EQ(old,J::array({c.d,c.in,c.out,c.market,c.actions,c.before,c.provenance}));}
}
TEST(QtEquityPriorFinalization, FractionalActualTradeMarksUnrealizedWithoutRealizingOrChargingAgain) {
 auto c=make();const auto frozen=c.out;auto r=invoke(c);ASSERT_TRUE(r.is_ok());const auto& after=r.value().at("after_financial");
 EXPECT_EQ(r.value()["schema_version"],"qt-equity-desk-finalization/v1");EXPECT_EQ(r.value()["calculation_version"],"equity-prior-close-mark/v1");
 EXPECT_EQ(after["positions"][0]["quantity_exact"],"0.5");EXPECT_EQ(after["positions"][0]["average_price_exact"],"10");
 EXPECT_EQ(after["positions"][0]["daily_realized_pnl_exact"],"4");EXPECT_EQ(after["positions"][0]["daily_unrealized_pnl_exact"],"3");
 EXPECT_EQ(after["positions"][0]["key"]["date"],"2026-09-25");EXPECT_EQ(after["positions"][0]["last_update"],"2026-09-26T00:00:00Z");
 const auto& live=after["live_results"][0];EXPECT_EQ(live["total_realized_pnl_exact"],"34");EXPECT_EQ(live["total_transaction_costs_exact"],"10.14");
 EXPECT_EQ(live["daily_realized_pnl_exact"],"4");EXPECT_EQ(live["daily_transaction_costs_exact"],"0.14");EXPECT_EQ(live["daily_unrealized_pnl_exact"],"-3");
 EXPECT_EQ(live["daily_pnl_exact"],"0.86");EXPECT_EQ(live["current_portfolio_value_exact"],"1026.86");EXPECT_EQ(c.out,frozen);
 EXPECT_EQ(r.value()["unchanged_execution_digest"],hash(c.out["executions"]));
}
TEST(QtEquityPriorFinalization, QuietDayMarksWithoutInventingExecutionsOrCost) {auto c=make("quiet");ASSERT_TRUE(c.out["executions"].empty());auto r=invoke(c);ASSERT_TRUE(r.is_ok());auto live=r.value()["after_financial"]["live_results"][0];EXPECT_EQ(live["total_realized_pnl_exact"],"30");EXPECT_EQ(live["total_transaction_costs_exact"],"10");EXPECT_EQ(live["current_portfolio_value_exact"],"1029");}
TEST(QtEquityPriorFinalization, NoneditableQuantityAndBasisStayConfirmed) {auto c=make("noneditable");auto r=invoke(c);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value()["after_financial"]["positions"][0]["quantity_exact"],"1.5");EXPECT_EQ(r.value()["after_financial"]["positions"][0]["average_price_exact"],"10");}
TEST(QtEquityPriorFinalization, FlatOwnerRemainsCompleteWithZeroMark) {auto c=make("flat");auto r=invoke(c);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value()["after_financial"]["positions"][0]["quantity_exact"],"0");EXPECT_EQ(r.value()["after_financial"]["positions"][0]["daily_unrealized_pnl_exact"],"0");}
TEST(QtEquityPriorFinalization, CompleteOwnersRemainSeparate) {auto c=make("two_owners");auto r=invoke(c);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value()["after_financial"]["positions"].size(),2);}
TEST(QtEquityPriorFinalization, AlreadyAppliedOriginalSplitIsNotAppliedTwice) {auto c=make("split");ASSERT_EQ(c.out["corporate_action_adjustments"].size(),1);auto r=invoke(c);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value()["after_financial"]["positions"][0]["quantity_exact"],"3");EXPECT_EQ(r.value()["after_financial"]["positions"][0]["average_price_exact"],"5");EXPECT_EQ(r.value()["after_financial"]["live_results"][0]["current_portfolio_value_exact"],"1032");}
TEST(QtEquityPriorFinalization, AlreadyAppliedOriginalDividendBasisAndCostsAreRetained) {auto c=make("dividend");auto r=invoke(c);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value()["after_financial"]["positions"][0]["average_price_exact"],c.out["observation"]["fills"][0]["average_price_exact"]);EXPECT_EQ(r.value()["after_financial"]["positions"][0]["quantity_exact"],"1.5");EXPECT_EQ(r.value()["after_financial"]["live_results"][0]["total_transaction_costs_exact"],"10");}
TEST(QtEquityPriorFinalization, IntradayFractionalShortCannotBecomeUnchargedOvernightHolding) {refused(make("short"));}
TEST(QtEquityPriorFinalization, MissingActualSCloseRefuses) {auto c=make();c.market["instruments"][0]["mark"].erase("price_model_number");bind_changed_sources(c);refused(c);}
TEST(QtEquityPriorFinalization, FutureCloseRefuses) {auto c=make();c.market["instruments"][0]["mark"]["date"]="2026-09-26";bind_changed_sources(c);refused(c);}
TEST(QtEquityPriorFinalization, NonfiniteCloseRefuses) {auto c=make();c.market["instruments"][0]["mark"]["price_model_number"]="inf";bind_changed_sources(c);refused(c);}
TEST(QtEquityPriorFinalization, MissingGovernedNoActionEvidenceRefuses) {auto c=make();c.actions=J(nullptr);refused(c);}
TEST(QtEquityPriorFinalization, NewSActionCannotResizeConfirmedHolding) {auto c=make();c.actions["events"].push_back(new_action(c,"2026-09-25"));bind_changed_sources(c);refused(c);}
TEST(QtEquityPriorFinalization, DActionCannotBeMovedIntoSFinalization) {auto c=make();c.actions["events"].push_back(new_action(c,"2026-09-26"));bind_changed_sources(c);refused(c);}
TEST(QtEquityPriorFinalization, PostActionFrameCannotBePretendedWithoutEvent) {auto c=make();c.market["instruments"][0]["mark"]["price_frame_id"]="unproved-new-frame";bind_changed_sources(c);refused(c);}
TEST(QtEquityPriorFinalization, MissingOwnerRefusesCompleteScope) {auto c=make("two_owners");c.before["positions"].erase(1);refused(c);}
TEST(QtEquityPriorFinalization, ForeignOwnerCannotBeNettedBySymbol) {auto c=make();auto p=c.before["positions"][0];p["key"]["strategy_name"]="foreign";c.before["positions"].push_back(p);refused(c);}
TEST(QtEquityPriorFinalization, ChangedBeforeCostsRefuse) {auto c=make();c.before["live_results"][0]["total_transaction_costs_exact"]="0";refused(c);}
TEST(QtEquityPriorFinalization, ChangedOriginalExecutionDigestRefuses) {auto c=make();c.provenance["unchanged_execution_digest"]=std::string(64,'0');refused(c);}
TEST(QtEquityPriorFinalization, SameDayOrBackwardValuationRefuses) {auto c=make();c.market["source_day"]="2026-09-25";c.market["valuation_time"]="2026-09-25T00:00:00Z";refused(c);}
TEST(QtEquityPriorFinalization, FinalizerCannotClaimAnotherBuildAgainstOriginalCapturedAuthority) {auto c=make();c.provenance["finalizer_authority"]["evaluator_build"]="unadmitted-build";refused(c);}
TEST(QtEquityPriorFinalization, FinalizerCannotClaimAnotherBundleAgainstOriginalCapturedAuthority) {auto c=make();c.provenance["finalizer_authority"]["evaluator_bundle_sha256"]=std::string(64,'0');refused(c);}
