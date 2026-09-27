#include "trade_ngin/apps/qt_prior_finalization.hpp"
#include "trade_ngin/apps/qt_desk_cycle.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
namespace trade_ngin { namespace {
using J=nlohmann::json;
J component_key(const char* day,const char* stream){return {{"portfolio_id","BOOK"},{"strategy_id","ENGINE"},{"strategy_name","alpha"},{"date",day},{"symbol","SYN"},{"portfolio_type",stream}};}
J original_decision(){return {{"decision_id","10000000-0000-4000-8000-000000000001"},{"book_id","BOOK"},{"source_day","2026-09-25"}};}
J original_input(){return {{"schema_version","qt-futures-accounting-input/v1"},
 {"decision_id","10000000-0000-4000-8000-000000000001"},{"book_id","BOOK"},{"source_day","2026-09-25"},
 {"previous_day","2026-09-24"},{"accounting_source_id","original-accounting"},{"prior_finalization_source_id","prior-final"},
 {"timestamp","2026-09-25T00:00:00Z"},{"currency","USD"},
 {"previous_positions",J::array({{{"key",component_key("2026-09-24","qt")},{"quantity_exact","4"},{"average_price_exact","90"}}})},
 {"previous_totals",J::array({{{"strategy_id","ENGINE"},{"equity_exact","1000"},{"total_pnl_exact","20"}}})},
 {"cost_config",{{"explicit_fee_per_contract","2"},{"min_adv","100"},{"min_participation","0"},{"max_participation","0.1"}}},
 {"instruments",J::array({{{"symbol","SYN"},{"price_exact","100"},{"adv_exact","100000"},{"volatility_multiplier_exact","1"},
 {"source_id","original-reference-close"},{"baseline_spread_ticks","0"},{"min_spread_ticks","0"},{"max_spread_ticks","0"},
 {"spread_cost_multiplier","0"},{"max_impact_bps","0"},{"tick_size","0.01"},{"point_value","50"},{"max_total_implicit_bps","0"}}})}};}
J original_selection(){return J::array({{{"key",component_key("2026-09-25","qt_proposal")},{"asset_type","FUTURE"},{"editable",true},{"quantity_exact","5"},{"average_price_exact","90"}}});}
J market(){return {{"schema_version","qt-accounting-market/v1"},{"book_id","BOOK"},{"source_day","2026-09-26"},
 {"previous_day","2026-09-25"},{"valuation_time","2026-09-26T00:00:00Z"},{"currency","USD"},
 {"instruments",J::array({{{"symbol","SYN"},{"instrument_type","FUTURE"},{"price_model_number","101"},
 {"price_time","2026-09-25T00:00:00Z"},{"source_id","settlement-close"},{"point_value","50"}}})}};}
J before(const J& output){
 J positions=J::array();for(const auto& fill:output.at("observation").at("fills"))positions.push_back({
  {"key",fill.at("key")},{"quantity_exact",fill.at("selected_quantity_exact")},{"average_price_exact",fill.at("average_price_exact")},
  {"daily_realized_pnl_exact",fill.at("daily_realized_pnl_exact")},{"daily_unrealized_pnl_exact",fill.at("daily_unrealized_pnl_exact")},{"last_update",fill.at("last_update")}});
 auto live=output.at("live_results");J equity=J::array();for(auto& row:live){row["daily_realized_pnl_exact"]="0";row["daily_unrealized_pnl_exact"]="0";
  equity.push_back({{"portfolio_id",row.at("portfolio_id")},{"strategy_id",row.at("strategy_id")},{"timestamp",row.at("date").get<std::string>()+"T00:00:00Z"},{"portfolio_type","qt"},{"equity_exact",row.at("current_portfolio_value_exact")}});}
 return {{"positions",positions},{"live_results",live},{"equity_curve",equity}};
}
J provenance(){return {{"finalization_id","f0000000-0000-4000-8000-000000000001"},
 {"original_accounting_input_id","90000000-0000-4000-8000-000000000001"},{"original_run_result_digest",std::string(64,'a')},
 {"original_observation_digest",std::string(64,'b')},{"predecessor_finalization_source_id","prior-final"},
 {"predecessor_finalization_digest",std::string(64,'c')},{"market_source_id","a0000000-0000-4000-8000-000000000001"},
 {"market_source_digest",std::string(64,'d')},{"unchanged_execution_digest",std::string(64,'e')},
 {"policy_identity",{{"book_id","BOOK"},{"purpose","execution"},{"version",1},{"producer_id","synthetic-execution"},{"policy_version","policy-v1"}}}};}
TEST(QtPriorFinalizationTest, UsesActualOriginalAccountingAndSeparatesMarkFromBasis){
 auto output=produce_qt_futures_accounting(original_decision(),original_selection(),original_input());ASSERT_TRUE(output.is_ok());
 ASSERT_EQ(output.value()["live_results"][0]["daily_transaction_costs_exact"],"2");
 auto result=produce_qt_prior_finalization(original_decision(),original_input(),output.value(),market(),before(output.value()),provenance());
 ASSERT_TRUE(result.is_ok());const auto& finalized=result.value();
 EXPECT_EQ(finalized["components"][0]["gross_pnl_exact"],"250");
 EXPECT_EQ(finalized["engine_totals"][0]["net_pnl_exact"],"248");
 EXPECT_EQ(finalized["engine_totals"][0]["equity_exact"],"1248");
 EXPECT_EQ(finalized["engine_totals"][0]["total_pnl_exact"],"268");
 EXPECT_EQ(finalized["after_financial"]["positions"][0]["average_price_exact"],"90");
 EXPECT_EQ(finalized["after_financial"]["positions"][0]["quantity_exact"],"5");
 EXPECT_EQ(finalized["after_financial"]["positions"][0]["daily_realized_pnl_exact"],"250");
 EXPECT_EQ(finalized["after_financial"]["positions"][0]["daily_unrealized_pnl_exact"],"0");
}
TEST(QtPriorFinalizationTest, ZeroPnlIsFinalizedAndUnavailableMarkNeverBecomesZero){
 auto output=produce_qt_futures_accounting(original_decision(),original_selection(),original_input());ASSERT_TRUE(output.is_ok());
 auto marks=market();marks["instruments"][0]["price_model_number"]="100";
 auto zero=produce_qt_prior_finalization(original_decision(),original_input(),output.value(),marks,before(output.value()),provenance());
 ASSERT_TRUE(zero.is_ok());EXPECT_EQ(zero.value()["engine_totals"][0]["equity_exact"],"998");
 marks["instruments"][0].erase("price_model_number");
 EXPECT_TRUE(produce_qt_prior_finalization(original_decision(),original_input(),output.value(),marks,before(output.value()),provenance()).is_error());
}
TEST(QtPriorFinalizationTest, RejectsChangedQuantityBasisCostsAndReferenceIdentity){
 auto output=produce_qt_futures_accounting(original_decision(),original_selection(),original_input());ASSERT_TRUE(output.is_ok());
 for(const auto* field:{"quantity_exact","average_price_exact","daily_realized_pnl_exact"}){
  auto rows=before(output.value());rows["positions"][0][field]="999";
  EXPECT_TRUE(produce_qt_prior_finalization(original_decision(),original_input(),output.value(),market(),rows,provenance()).is_error());
 }
 auto rows=before(output.value());rows["live_results"][0]["daily_transaction_costs_exact"]="1";
 EXPECT_TRUE(produce_qt_prior_finalization(original_decision(),original_input(),output.value(),market(),rows,provenance()).is_error());
 auto marks=market();marks["instruments"][0]["point_value"]="5";
 EXPECT_TRUE(produce_qt_prior_finalization(original_decision(),original_input(),output.value(),marks,before(output.value()),provenance()).is_error());
 marks=market();marks["instruments"][0]["price_model_number"]="1e300";
 EXPECT_TRUE(produce_qt_prior_finalization(original_decision(),original_input(),output.value(),marks,before(output.value()),provenance()).is_error());
}
TEST(QtPriorFinalizationTest, MarketMayAlsoContainTheNextDaysNewSymbol){
 auto output=produce_qt_futures_accounting(original_decision(),original_selection(),original_input());ASSERT_TRUE(output.is_ok());
 auto marks=market();auto extra=marks["instruments"][0];extra["symbol"]="NEW";marks["instruments"].push_back(extra);
 auto result=produce_qt_prior_finalization(original_decision(),original_input(),output.value(),marks,before(output.value()),provenance());
 ASSERT_TRUE(result.is_ok());EXPECT_EQ(result.value()["components"].size(),1U);
}
TEST(QtPriorFinalizationTest, OppositeOwnersFinalizeIndividuallyEvenWhenAggregateGrossIsZero){
 auto input=original_input(),selection=original_selection();
 auto prior=input["previous_positions"][0];prior["key"]["strategy_name"]="beta";prior["quantity_exact"]="-4";input["previous_positions"].push_back(prior);
 auto chosen=selection[0];chosen["key"]["strategy_name"]="beta";chosen["quantity_exact"]="-5";selection.push_back(chosen);
 auto output=produce_qt_futures_accounting(original_decision(),selection,input);ASSERT_TRUE(output.is_ok());
 auto result=produce_qt_prior_finalization(original_decision(),input,output.value(),market(),before(output.value()),provenance());ASSERT_TRUE(result.is_ok());
 EXPECT_EQ(result.value()["engine_totals"][0]["gross_pnl_exact"],"0");
 EXPECT_EQ(result.value()["engine_totals"][0]["actual_cash_cost_exact"],"4");
 EXPECT_EQ(result.value()["engine_totals"][0]["equity_exact"],"996");
 EXPECT_EQ(result.value()["after_financial"]["positions"][0]["daily_realized_pnl_exact"],"250");
 EXPECT_EQ(result.value()["after_financial"]["positions"][1]["daily_realized_pnl_exact"],"-250");
}
}}
