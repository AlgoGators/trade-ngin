#include <gtest/gtest.h>
#include "trade_ngin/apps/qt_equity_desk_cycle.hpp"
#include "trade_ngin/apps/qt_equity_cost_consumption.hpp"

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
void refused(J in,J selection=J::array()){
 auto d=decision();auto saved=J::array({d,selection,in});std::vector<QtEquityCostTrace> trace(1);
 trace[0].key.strategy_name="sentinel";
 auto r=produce_qt_equity_accounting(d,selection,in,&trace);EXPECT_TRUE(r.is_error());
 EXPECT_EQ(J::array({d,selection,in}),saved);ASSERT_EQ(trace.size(),1U);EXPECT_EQ(trace[0].key.strategy_name,"sentinel");
}
TEST(QtEmptyOwnerAccounting, ActualSumsPreserveCapitalWithoutPositionsExecutionsOrCharges){
 auto d=decision(),in=input();auto saved=J::array({d,in});std::vector<QtEquityCostTrace> trace(1);
 auto r=produce_qt_equity_accounting(d,J::array(),in,&trace);ASSERT_TRUE(r.is_ok());const auto& o=r.value();
 EXPECT_EQ(o.at("schema_version"),"qt-equity-accounting-empty-owner/v2");
 for(auto f:{"fills"})EXPECT_TRUE(o.at("observation").at(f).empty());
 for(auto f:{"executions","distance","corporate_action_adjustments","layers_applied"})EXPECT_TRUE(o.at(f).empty());
 EXPECT_TRUE(trace.empty());ASSERT_EQ(o.at("live_results").size(),1U);ASSERT_EQ(o.at("equity_curve").size(),1U);
 EXPECT_EQ(o.at("live_results")[0].at("strategy_id"),in.at("previous_totals")[0].at("strategy_id"));
 EXPECT_EQ(o.at("live_results")[0].at("current_portfolio_value_exact"),"100000");
 for(auto f:{"daily_realized_pnl_exact","daily_unrealized_pnl_exact","daily_pnl_exact","daily_transaction_costs_exact","total_pnl_exact"})
   EXPECT_EQ(o.at("live_results")[0].at(f),"0");
 EXPECT_EQ(o.at("equity_curve")[0].at("equity_exact"),"100000");
 EXPECT_EQ(o.at("observation").at("results").at("position_count"),0);EXPECT_EQ(J::array({d,in}),saved);
 auto child=project_qt_equity_cost_consumption(d,in,o,&trace);ASSERT_TRUE(child.is_ok());
 EXPECT_TRUE(child.value().at("charges").empty());EXPECT_EQ(child.value().at("coverage").at("status"),"complete");
}
TEST(QtEmptyOwnerAccounting, ActualPriorCumulativeRealizedAndCostsAreNotReset){
 auto in=input();auto& a=in["previous_totals"][0];a["total_realized_pnl_exact"]="12";
 a["total_transaction_costs_exact"]="2";a["total_pnl_exact"]="10";a["equity_exact"]="100010";
 auto r=produce_qt_equity_accounting(decision(),J::array(),in);ASSERT_TRUE(r.is_ok());
 const auto& live=r.value().at("live_results")[0];EXPECT_EQ(live.at("total_realized_pnl_exact"),"12");
 EXPECT_EQ(live.at("total_transaction_costs_exact"),"2");EXPECT_EQ(live.at("current_portfolio_value_exact"),"100010");
 EXPECT_EQ(live.at("daily_pnl_exact"),"0");
}
TEST(QtEmptyOwnerAccounting, V1EmptyChoiceStillRefuses){auto in=input();in["schema_version"]="qt-equity-accounting-input/v1";refused(in);}
TEST(QtEmptyOwnerAccounting, NonemptySelectionCannotUseEmptyAuthority){refused(input(),J::array({J::object()}));}
TEST(QtEmptyOwnerAccounting, NonemptyPriorCannotUseEmptyAuthority){auto in=input();in["previous_positions"].push_back(J::object());refused(in);}
TEST(QtEmptyOwnerAccounting, NonemptyInstrumentCannotUseEmptyAuthority){auto in=input();in["instruments"].push_back(J::object());refused(in);}
TEST(QtEmptyOwnerAccounting, CorporateActionCannotUseEmptyAuthority){auto in=input();in["actions"].push_back(J::object());refused(in);}
TEST(QtEmptyOwnerAccounting, MissingEngineAnchorRefuses){auto in=input();in["previous_totals"]=J::array();refused(in);}
TEST(QtEmptyOwnerAccounting, ExtraEngineAnchorRefuses){auto in=input();auto extra=in["previous_totals"][0];extra["strategy_id"]="FOREIGN";in["previous_totals"].push_back(extra);refused(in);}
TEST(QtEmptyOwnerAccounting, UnrealizedAnchorNeedsActualPositions){auto in=input();in["previous_totals"][0]["total_unrealized_pnl_exact"]="1";refused(in);}
TEST(QtEmptyOwnerAccounting, ZeroCapitalCannotBecomeReady){auto in=input();in["previous_totals"][0]["initial_capital_exact"]="0";refused(in);}
TEST(QtEmptyOwnerAccounting, ContradictoryEquityAnchorRefuses){auto in=input();in["previous_totals"][0]["equity_exact"]="100001";refused(in);}
TEST(QtEmptyOwnerAccounting, UnknownReadyFlagDoesNotAdmitInput){auto in=input();in["ready"]=true;refused(in);}
TEST(QtEmptyOwnerAccounting, MissingActualTraceNeverMeansNoCharges){auto in=input();auto out=produce_qt_equity_accounting(decision(),J::array(),in);ASSERT_TRUE(out.is_ok());
 EXPECT_TRUE(project_qt_equity_cost_consumption(decision(),in,out.value(),nullptr).is_error());}
}
