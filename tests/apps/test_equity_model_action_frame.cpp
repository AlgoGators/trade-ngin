#include <gtest/gtest.h>
#include <stdexcept>
#include "trade_ngin/apps/qt_equity_prior_finalization.hpp"
#include "trade_ngin/apps/qt_equity_accounting_output.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/apps/equity_model_action_frame.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"
#include "qt_test_build_identity.hpp"
using namespace trade_ngin;
namespace {
using J=nlohmann::json;
#define fixture unbound_fixture
J fixture(){return J::parse(R"fixture({"accounting_input":{"accounting_input_id":"20000000-0000-0000-0000-000000000001","accounting_source_id":"owned-synthetic-equity-accounting","actions":[],"book_id":"BOOK","calculation_version":"qt-equity-main08b15c/v1","cost_config":{"explicit_fee_per_contract":"1.5","max_participation":"1","min_adv":"10000","min_participation":"0"},"currency":"USD","day_mode":"open","decision_id":"10000000-0000-0000-0000-000000000001","instruments":[{"asset_type":"EQUITY","cost_evidence":{"adv_model_number":"1000000","date":"2026-09-24","source_digest":"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","source_id":"owned-synthetic-cost-history","volatility_multiplier_model_number":"1"},"cost_parameters":{"apply_regulatory_fees":false,"baseline_spread_ticks":"1","commission_per_unit":"0.005","finra_taf_cap_per_trade":"9.79","finra_taf_per_share":"0.000195","max_commission_pct":"0.01","max_commission_per_order":"100","max_impact_bps":"100","max_spread_ticks":"10","max_total_implicit_bps":"0","min_commission_per_order":"1","min_spread_ticks":"1","point_value":"1","sec_fee_per_million":"20.6","spread_cost_multiplier":"0.5","tick_constrained":false,"tick_size":"0.01"},"mark":{"date":"2026-09-24","price_frame_id":"adjusted-1","price_model_number":"14","source_digest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","source_id":"owned-synthetic-close"},"reference":{"date":"2026-09-24","price_frame_id":"adjusted-1","price_model_number":"14","source_digest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","source_id":"owned-synthetic-close"},"symbol":"SYN"}],"market_source_digest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","market_source_id":"30000000-0000-0000-0000-000000000001","previous_day":"2026-09-24","previous_positions":[{"average_price_exact":"10","basis_evidence":{"formed_day":"2026-09-20","price_frame_id":"adjusted-1","source_digest":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","source_id":"owned-prior-position"},"daily_realized_pnl_exact":"9","daily_unrealized_pnl_exact":"6","key":{"date":"2026-09-24","portfolio_id":"BOOK","portfolio_type":"qt","strategy_id":"LIVE_EQUITY_MEAN_REVERSION","strategy_name":"EQUITY_MEAN_REVERSION","symbol":"SYN"},"last_update":"2026-09-24T00:00:00Z","quantity_exact":"1.5"}],"previous_totals":[{"equity_exact":"1026","initial_capital_exact":"1000","strategy_id":"LIVE_EQUITY_MEAN_REVERSION","total_pnl_exact":"26","total_realized_pnl_exact":"30","total_transaction_costs_exact":"10","total_unrealized_pnl_exact":"6"}],"prior_finalization_digest":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","prior_finalization_source_id":"qt-finalization/40000000-0000-0000-0000-000000000001","schema_version":"qt-equity-accounting-input/v1","source_day":"2026-09-25","timestamp":"2026-09-25T00:00:00Z"},"decision":{"book_id":"BOOK","decision_id":"10000000-0000-0000-0000-000000000001","model_publication_id":"50000000-0000-0000-0000-000000000001","source_day":"2026-09-25"},"producer_authority":{"allowed_override_codes":[],"as_of":"2026-09-25T00:00:00Z","book_id":"BOOK","content_digest":"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee","evaluator_build":"local-qt-controlled","evaluator_bundle_sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","evaluator_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","model_publication_id":"50000000-0000-0000-0000-000000000001","policy_revision":1,"policy_updated_at":"2026-09-20T00:00:00Z","policy_version":"owned-synthetic-evaluation-policy/v1","producer_id":"owned-synthetic-evaluator","schema_version":"qt-input-authority/v1","snapshot_id":1,"source_day":"2026-09-25","source_version":"owned-synthetic-evaluation/v1","valid_until":"2026-09-26T00:00:00Z"},"selection":[{"asset_type":"EQUITY","average_price_exact":"10","basis_status":"preserved_source","editable":true,"key":{"date":"2026-09-25","portfolio_id":"BOOK","portfolio_type":"qt_proposal","strategy_id":"LIVE_EQUITY_MEAN_REVERSION","strategy_name":"EQUITY_MEAN_REVERSION","symbol":"SYN"},"origin":"qt_draft","quantity_exact":"0.5"}]})fixture");}
#undef fixture
J fixture(){return trade_ngin::test::fixture_for_compiled_build(unbound_fixture());}
std::string hash(const J& j,bool output=false){auto b=output?canonical_qt_desk_source_json(j):canonical_qt_desk_input_json(j);if(b.is_error())throw std::runtime_error("fixture canonicalization");auto h=qt_sha256_hex(b.value());if(h.is_error())throw std::runtime_error("fixture digest");return h.value();}
struct Case {J d,in,out,market,actions,before,provenance;};
Case make(std::string mode="executed") {
 auto f=fixture();auto in=f.at("accounting_input"),s=f.at("selection");
 if(mode=="quiet"||mode=="noneditable") {s[0]["quantity_exact"]="1.5";s[0]["average_price_exact"]="10";if(mode=="noneditable")s[0]["editable"]=false;}
 if(mode=="flat")s[0]["quantity_exact"]="0";
 if(mode=="short")s[0]["quantity_exact"]="-0.5";
 if(mode=="split"||mode=="dividend"||mode=="split_flat") {
  auto& prior=in["previous_positions"][0];
  s[0]["quantity_exact"]=mode=="split_flat"?"0":mode=="split"?"3":"1.5";
  J event={{"key",s[0]["key"]},{"source_id","owned-original-action"},{"source_digest",std::string(64,'e')},
   {"type",mode!="dividend"?"SPLIT":"DIVIDEND"},{"ex_date",in["previous_day"]},{"value_model_number",mode!="dividend"?"2":"1"},
   {"basis_provenance","formed_on_or_before_ex_date"},{"basis_provenance_evidence",prior["basis_evidence"]["source_id"]},
   {"frame_before","adjusted-1"},{"frame_after","adjusted-2"},{"raw_close_model_number",nullptr},{"eligible_quantity_exact",nullptr}};
  event["key"]["portfolio_type"]="qt";
  if(mode=="dividend"){event["raw_close_model_number"]="10";event["eligible_quantity_exact"]="1.5";}
  in["actions"]=J::array({event});
  for(auto name:{"reference","mark"}) {in["instruments"][0][name]["price_frame_id"]="adjusted-2";in["instruments"][0][name]["price_model_number"]=mode!="dividend"?"7":"10";}
 }
 if(mode=="two_owners") {
  auto p=in["previous_positions"][0],choice=s[0];p["key"]["strategy_name"]="owner-b";p["quantity_exact"]="2";p["daily_unrealized_pnl_exact"]="8";
  p["basis_evidence"]["source_id"]="owned-owner-b-basis";choice["key"]["strategy_name"]="owner-b";choice["quantity_exact"]="2";choice["average_price_exact"]="10";
  in["previous_positions"].push_back(p);s.push_back(choice);in["previous_totals"][0]["total_unrealized_pnl_exact"]="14";in["previous_totals"][0]["total_pnl_exact"]="34";in["previous_totals"][0]["equity_exact"]="1034";
 }
 auto original=recompute_qt_equity_accounting_output(f["decision"],s,in,f["producer_authority"]);
 if(original.is_error())throw std::runtime_error("actual original producer must succeed before RED");
 auto out=original.value();const auto day="2026-09-26";auto instruments=in["instruments"];
 for(auto& item:instruments)for(auto name:{"reference","mark"}){item[name]["date"]="2026-09-25";item[name]["source_id"]="owned-S-close";item[name]["source_digest"]=std::string(64,'f');item[name]["price_model_number"]=mode!="dividend"?"9":"16";}
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

namespace {
struct FrameCase { Case original; VerifiedEquityModelPrior prior; EquityModelPriorOwner owner; J source,policy,raw; };
FrameCase frame_case(std::string original_mode="quiet",std::string mode="none") {
 auto c=make(original_mode);auto finalized=invoke(c);if(finalized.is_error())throw std::runtime_error("real S finalizer refused");
 VerifiedEquityModelPrior p;p.financial=finalized.value().at("after_financial");p.basis_positions=J::array();
 for(auto row:p.financial.at("positions")){
  const auto symbol=row["key"]["symbol"].get<std::string>();
  auto qty=parse_qt_quantity_exact(row["quantity_exact"].get<std::string>()),basis=parse_qt_quantity_exact(row["average_price_exact"].get<std::string>()),
   unreal=parse_qt_quantity_exact(row["daily_unrealized_pnl_exact"].get<std::string>()),real=parse_qt_quantity_exact(row["daily_realized_pnl_exact"].get<std::string>());
  if(qty.is_error()||basis.is_error()||unreal.is_error()||real.is_error())throw std::runtime_error("fixture precision");
  p.positions.emplace(symbol,Position{symbol,qty.value(),basis.value(),unreal.value(),real.value(),std::chrono::sys_days{std::chrono::year{2026}/9/26}});
  row["basis_evidence"]={{"source_id","qt-basis/owned-finalized-S"},{"source_digest",hash(row)},{"price_frame_id",c.market["instruments"][0]["mark"]["price_frame_id"]},{"formed_day","2026-09-20"}};
  p.basis_positions.push_back(row);
 }
 p.replay_reference={{"schema_version","qt-equity-model-prior/v1"},{"mode","verified_desk_prior"},{"book_id","BOOK"},{"source_day","2026-09-25"},{"valuation_day","2026-09-26"},{"basis_positions",p.basis_positions},{"action_admission","action_free_only"}};
 EquityModelPriorOwner owner{"BOOK","LIVE_EQUITY_MEAN_REVERSION","EQUITY_MEAN_REVERSION","2026-09-25","2026-09-26"};
 J events=J::array();std::string before=p.basis_positions[0]["basis_evidence"]["price_frame_id"];
 auto add_event=[&](const char* type){const auto after=before+"/D-"+type;auto key=p.basis_positions[0]["key"];key["date"]=owner.valuation_day;
  J event={{"key",key},{"type",type},{"ex_date",owner.valuation_day},{"value_model_number",std::string(type)=="DIVIDEND"?"1":"2"},
   {"basis_provenance","formed_on_or_before_ex_date"},{"basis_provenance_evidence",p.basis_positions[0]["basis_evidence"]["source_id"]},
   {"frame_before",before},{"frame_after",after},{"raw_close_model_number",nullptr},{"eligible_quantity_exact",nullptr}};
  if(std::string(type)=="DIVIDEND"){event["raw_close_model_number"]="10";event["eligible_quantity_exact"]=p.basis_positions[0]["quantity_exact"];}
  events.push_back(event);before=after;};
 if(mode=="split"||mode=="mixed")add_event("SPLIT");if(mode=="dividend"||mode=="mixed")add_event("DIVIDEND");
 J payload={{"schema_version","qt-equity-actions-source/v1"},{"book_id","BOOK"},{"source_day",owner.valuation_day},{"previous_day",owner.source_day},{"valuation_time",owner.valuation_day+"T00:00:00Z"},{"events",events}};
 J source={{"source_id","qt-actions/owned-D-model-frame"},{"book_id","BOOK"},{"source_day",owner.valuation_day},{"purpose","actions"},{"producer_id","owned-source"},{"policy_version","owned-policy/v1"},{"policy_revision",1},{"source_version","qt-actions/owned-D-model-frame"},{"created_at",owner.valuation_day+"T00:00:00Z"},{"content_digest",hash(payload)},{"payload",payload}};
 J policy={{"book_id","BOOK"},{"purpose","execution"},{"enabled",true},{"version",1},{"producer_id","owned-source"},{"policy_version","owned-policy/v1"}};
 J raw={{"bars",J::array()},{"aliases",J::array()},{"terminations",J::array()},{"restating_metadata",J::array()}};
 if(mode!="none")raw["bars"].push_back({{"symbol","SYN"},{"ex_date",owner.valuation_day},{"raw_close_model_number","10"},{"split_factor_model_number",mode=="split"||mode=="mixed"?"2":"1"},{"dividend_cash_model_number",mode=="dividend"||mode=="mixed"?"1":"0"}});
 return {c,p,owner,source,policy,raw};
}
auto apply_frame(const FrameCase& f){return derive_equity_model_action_frame(f.prior,f.owner,f.original.out["corporate_action_adjustments"],f.source,f.policy,f.raw);}
void rehash(FrameCase& f){f.source["content_digest"]=hash(f.source["payload"]);}
void frame_refused(const FrameCase& f){const auto before=J::array({f.original.out,f.prior.financial,f.prior.basis_positions,f.prior.replay_reference,f.source,f.policy,f.raw});EXPECT_TRUE(apply_frame(f).is_error());EXPECT_EQ(before,J::array({f.original.out,f.prior.financial,f.prior.basis_positions,f.prior.replay_reference,f.source,f.policy,f.raw}));}
void unchanged_S(const FrameCase& f,const EquityModelActionFrame& r){EXPECT_EQ(f.prior.positions.at("SYN").realized_pnl,r.positions.at("SYN").realized_pnl);EXPECT_EQ(f.prior.positions.at("SYN").unrealized_pnl,r.positions.at("SYN").unrealized_pnl);EXPECT_EQ(f.prior.positions.at("SYN").last_update,r.positions.at("SYN").last_update);EXPECT_EQ(r.document["original_basis_positions"],f.prior.basis_positions);}
}
TEST(EquityModelActionFrame, ZeroEventsPreserveExactOldReference) {auto f=frame_case();auto r=apply_frame(f);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value().replay_reference,f.prior.replay_reference);EXPECT_TRUE(r.value().document.is_null());EXPECT_TRUE(r.value().digest.empty());}
TEST(EquityModelActionFrame, ProvedOriginalSplitSeedsActualFinalizedBasisWithoutReapplying) {auto f=frame_case("split");const auto frozen=f.original.out;auto r=apply_frame(f);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value().positions.at("SYN").quantity,Quantity(3));EXPECT_EQ(r.value().positions.at("SYN").average_price,Decimal(5));EXPECT_EQ(r.value().original_action_count,1);EXPECT_EQ(r.value().successor_action_count,0);EXPECT_EQ(f.original.out,frozen);unchanged_S(f,r.value());}
TEST(EquityModelActionFrame, ProvedOriginalDividendSeedsFinalizedBasisWithoutSecondCashCredit) {auto f=frame_case("dividend");auto r=apply_frame(f);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value().positions.at("SYN").average_price,f.prior.positions.at("SYN").average_price);EXPECT_EQ(r.value().positions.at("SYN").quantity,Quantity(1.5));EXPECT_EQ(r.value().original_action_count,1);unchanged_S(f,r.value());}
TEST(EquityModelActionFrame, NewSplitRestatesOnlyModelCopy) {auto f=frame_case("quiet","split");auto r=apply_frame(f);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value().positions.at("SYN").quantity,Quantity(3));EXPECT_EQ(r.value().positions.at("SYN").average_price,Decimal(5));EXPECT_EQ(f.prior.positions.at("SYN").quantity,Quantity(1.5));EXPECT_EQ(r.value().successor_action_count,1);unchanged_S(f,r.value());}
TEST(EquityModelActionFrame, NewDividendMatchesExistingApplierAndAdjustedPricePnL) {auto f=frame_case("quiet","dividend");auto r=apply_frame(f);ASSERT_TRUE(r.is_ok());auto oracle=f.prior.positions;CorpActionEvent e;e.symbol="SYN";e.ex_date=f.owner.valuation_day;e.type=CorpActionType::DIVIDEND;e.value=1;e.close_at_ex_date=10;e.qty_at_ex_date=1.5;e.basis_provenance=CorpActionEvent::BasisProvenance::FORMED_ON_OR_BEFORE_EX_DATE;e.basis_provenance_evidence="qt-basis/owned-finalized-S";auto adj=CorporateActionsApplier::apply(oracle,{e});ASSERT_EQ(adj.size(),1);const auto& p=r.value().positions.at("SYN");EXPECT_EQ(p.average_price,oracle.at("SYN").average_price);EXPECT_EQ(p.quantity,Quantity(1.5));EXPECT_DOUBLE_EQ(LivePnLManager::unrealized_from_cost_basis(p.quantity.as_double(),p.average_price.as_double(),10),LivePnLManager::unrealized_from_cost_basis(oracle.at("SYN").quantity.as_double(),oracle.at("SYN").average_price.as_double(),10));EXPECT_FALSE(r.value().document.contains("dividend_cash_pnl_credit"));unchanged_S(f,r.value());}
TEST(EquityModelActionFrame, MixedSplitThenDividendUsesRoundedNativeBasisOnce) {auto f=frame_case("quiet","mixed");auto r=apply_frame(f);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value().positions.at("SYN").quantity,Quantity(3));EXPECT_EQ(r.value().positions.at("SYN").average_price,Decimal(5/1.1));EXPECT_EQ(r.value().successor_action_count,2);EXPECT_EQ(r.value().document["adjustments"].size(),2);unchanged_S(f,r.value());}
TEST(EquityModelActionFrame, DerivedFrameCannotBeFedBackAsOriginalAnchor) {auto f=frame_case("quiet","split");auto r=apply_frame(f);ASSERT_TRUE(r.is_ok());f.prior.replay_reference=r.value().replay_reference;frame_refused(f);}
TEST(EquityModelActionFrame, WrongBookRefuses){auto f=frame_case("quiet","split");f.owner.portfolio_id="FOREIGN";frame_refused(f);}
TEST(EquityModelActionFrame, WrongDayRefuses){auto f=frame_case("quiet","split");f.source["payload"]["source_day"]="2026-09-27";rehash(f);frame_refused(f);}
TEST(EquityModelActionFrame, WrongFrameRefuses){auto f=frame_case("quiet","split");f.source["payload"]["events"][0]["frame_before"]="foreign-frame";rehash(f);frame_refused(f);}
TEST(EquityModelActionFrame, MissingPolicyRefuses){auto f=frame_case("quiet","split");f.policy=J(nullptr);frame_refused(f);}
TEST(EquityModelActionFrame, DisabledPolicyRefuses){auto f=frame_case("quiet","split");f.policy["enabled"]=false;frame_refused(f);}
TEST(EquityModelActionFrame, ChangedSourceDigestRefuses){auto f=frame_case("quiet","split");f.source["payload"]["events"][0]["value_model_number"]="3";frame_refused(f);}
TEST(EquityModelActionFrame, RehashedSourceDoesNotAuthorizeForeignRawOperands){auto f=frame_case("quiet","split");f.source["payload"]["events"][0]["value_model_number"]="3";rehash(f);frame_refused(f);}
TEST(EquityModelActionFrame, DividendEligibilityMustEqualProvedSQuantity){auto f=frame_case("quiet","dividend");f.source["payload"]["events"][0]["eligible_quantity_exact"]="99";rehash(f);frame_refused(f);}
TEST(EquityModelActionFrame, DividendRawExDateCloseRequired){auto f=frame_case("quiet","dividend");f.source["payload"]["events"][0]["raw_close_model_number"]=nullptr;rehash(f);frame_refused(f);}
TEST(EquityModelActionFrame, UnknownProvenanceRefuses){auto f=frame_case("quiet","split");f.source["payload"]["events"][0]["basis_provenance"]="unknown";rehash(f);frame_refused(f);}
TEST(EquityModelActionFrame, UnsupportedSpinoffRefuses){auto f=frame_case("quiet","split");f.source["payload"]["events"][0]["type"]="SPINOFF";rehash(f);frame_refused(f);}
TEST(EquityModelActionFrame, AliasesAndTerminationsRefuse){for(const auto key:{"aliases","terminations"}){auto f=frame_case("quiet","split");f.raw[key].push_back({{"symbol","SYN"}});frame_refused(f);}}
TEST(EquityModelActionFrame, DuplicateEventsRefuse){auto f=frame_case("quiet","split");f.source["payload"]["events"].push_back(f.source["payload"]["events"][0]);rehash(f);frame_refused(f);}
TEST(EquityModelActionFrame, MissingEventForRawBarRefuses){auto f=frame_case("quiet","split");f.source["payload"]["events"].clear();rehash(f);frame_refused(f);}
TEST(EquityModelActionFrame, ForeignOwnerEventRefuses){auto f=frame_case("quiet","split");f.source["payload"]["events"][0]["key"]["strategy_name"]="FOREIGN";rehash(f);frame_refused(f);}

TEST(EquityModelActionFrame, ExtraSourceRootFieldRefusesClosedEvidence) {auto f=frame_case("quiet","split");f.source["unreviewed_extra"]=true;frame_refused(f);}
TEST(EquityModelActionFrame, ExistingSFinalizerCannotProveOvernightShortFrame) {auto c=make("short");EXPECT_TRUE(invoke(c).is_error());auto f=frame_case("quiet","split");f.prior.positions.at("SYN").quantity=Quantity(-1.5);f.prior.basis_positions[0]["quantity_exact"]="-1.5";f.prior.replay_reference["basis_positions"]=f.prior.basis_positions;frame_refused(f);}

namespace {
J known_metadata(const char* label="split"){return {{"date","2026-09-26"},{"ticker","SYN"},{"action",label},{"value",nullptr},{"contraticker",nullptr},{"contraname",nullptr},{"name",nullptr}};}
}
TEST(EquityModelActionFrame, KnownRestatingMetadataWithoutRawBarRefuses){auto f=frame_case();f.raw["restating_metadata"].push_back(known_metadata());frame_refused(f);}
TEST(EquityModelActionFrame, KnownDividendMetadataWithoutRawCashRefuses){auto f=frame_case("quiet","split");f.raw["restating_metadata"].push_back(known_metadata("dividend"));frame_refused(f);}
TEST(EquityModelActionFrame, KnownSplitMetadataMatchesDatedNativeOperands){auto f=frame_case("quiet","split");f.raw["restating_metadata"].push_back(known_metadata());auto result=apply_frame(f);ASSERT_TRUE(result.is_ok());EXPECT_EQ(result.value().positions.at("SYN").quantity,Quantity(3));EXPECT_EQ(result.value().document["raw_capture"]["restating_metadata"],f.raw["restating_metadata"]);}
TEST(EquityModelActionFrame, ForeignMetadataCannotAuthorizeHeldRawBar){auto f=frame_case("quiet","split");auto m=known_metadata();m["ticker"]="FOREIGN";f.raw["restating_metadata"].push_back(m);frame_refused(f);}
TEST(EquityModelActionFrame, MissingMetadataCollectionRefusesCompleteCapture){auto f=frame_case("quiet","split");f.raw.erase("restating_metadata");frame_refused(f);}

TEST(EquityModelActionFrame, KnownADRMetadataUsesActualADRKernel){auto f=frame_case("quiet","split");f.source["payload"]["events"][0]["type"]="ADR_SPLIT";rehash(f);f.raw["restating_metadata"].push_back(known_metadata("adrratiosplit"));auto result=apply_frame(f);ASSERT_TRUE(result.is_ok());EXPECT_EQ(result.value().positions.at("SYN").quantity,Quantity(3));EXPECT_EQ(result.value().positions.at("SYN").average_price,Decimal(5));}
TEST(EquityModelActionFrame, KnownADRMetadataCannotBeRelabelledPlainSplit){auto f=frame_case("quiet","split");f.raw["restating_metadata"].push_back(known_metadata("adrratiosplit"));frame_refused(f);}

TEST(EquityModelActionFrame, ActualClosedSPositionRemainsAZeroPriorRow){auto f=frame_case("flat");ASSERT_EQ(f.prior.positions.size(),1);EXPECT_TRUE(f.prior.positions.at("SYN").quantity.is_zero());ASSERT_EQ(f.prior.basis_positions.size(),1);EXPECT_EQ(f.prior.basis_positions[0]["quantity_exact"],"0");auto r=apply_frame(f);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value().positions.at("SYN").quantity,f.prior.positions.at("SYN").quantity);EXPECT_EQ(r.value().replay_reference,f.prior.replay_reference);}
TEST(EquityModelActionFrame, ClosedPositionDoesNotRequestNextSplitOrDividendEvidence){for(const auto mode:{"split","dividend"}){auto f=frame_case("flat");const auto before=f.prior.basis_positions;auto held=equity_model_action_held_symbols(f.prior.positions);EXPECT_TRUE(held.empty())<<mode;auto r=apply_frame(f);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value().positions.at("SYN").quantity,Quantity(0));EXPECT_EQ(f.prior.basis_positions,before);}}
TEST(EquityModelActionFrame, MixedNonzeroAndClosedHoldingsUseOnlyActualHeldEvidence){for(const auto mode:{"split","dividend"}){auto f=frame_case("quiet",mode),closed=frame_case("flat");auto zero=closed.prior.positions.at("SYN");zero.symbol="ZERO";f.prior.positions.emplace("ZERO",zero);auto row=closed.prior.basis_positions[0];row["key"]["symbol"]="ZERO";f.prior.basis_positions.push_back(row);f.prior.replay_reference["basis_positions"]=f.prior.basis_positions;EXPECT_EQ(equity_model_action_held_symbols(f.prior.positions),(std::vector<std::string>{"SYN"}));auto r=apply_frame(f);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value().positions.at("ZERO").quantity,zero.quantity);EXPECT_EQ(r.value().positions.at("ZERO").average_price,zero.average_price);EXPECT_EQ(r.value().positions.at("ZERO").last_update,zero.last_update);EXPECT_EQ(r.value().document["original_basis_positions"],f.prior.basis_positions);}}
TEST(EquityModelActionFrame, ClosingAfterOriginalSplitRetainsOriginalActionAndZeroBasis){auto f=frame_case("split_flat");ASSERT_TRUE(f.prior.positions.at("SYN").quantity.is_zero());ASSERT_EQ(f.original.out["corporate_action_adjustments"].size(),1);EXPECT_TRUE(equity_model_action_held_symbols(f.prior.positions).empty());auto r=apply_frame(f);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value().original_action_count,1);EXPECT_EQ(r.value().successor_action_count,0);EXPECT_TRUE(r.value().positions.at("SYN").quantity.is_zero());EXPECT_EQ(r.value().positions.at("SYN").average_price,f.prior.positions.at("SYN").average_price);EXPECT_EQ(r.value().document["original_basis_positions"],f.prior.basis_positions);}
TEST(EquityModelActionFrame, DealTermsValueIsRetainedWithoutBecomingFinancialOperand){auto f=frame_case("quiet","split");auto m=known_metadata();m["value"]="vendor-ratio-text-unconsumed";f.raw["restating_metadata"].push_back(m);auto r=apply_frame(f);ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value().positions.at("SYN").quantity,Quantity(3));EXPECT_EQ(r.value().document["raw_capture"]["restating_metadata"][0]["value"],m["value"]);}
