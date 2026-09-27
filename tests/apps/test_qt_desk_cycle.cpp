#include "trade_ngin/apps/qt_desk_cycle.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"
#include <gtest/gtest.h>

namespace trade_ngin { namespace {
using J=nlohmann::json;
J key(const char* owner="alpha",const char* day="2026-09-26",const char* stream="qt") {
    return {{"portfolio_id","BOOK"},{"strategy_id","ENGINE"},{"strategy_name",owner},
        {"date",day},{"symbol","SYN"},{"portfolio_type",stream}};
}
J decision(){return {{"decision_id","decision"},{"book_id","BOOK"},{"source_day","2026-09-26"}};}
J choices(){return J::array({{{"key",key("alpha","2026-09-26","qt_proposal")},
    {"asset_type","FUTURE"},{"editable",true},{"quantity_exact","5"},{"average_price_exact","100"}}});}
J inputs(){return {{"schema_version","qt-futures-accounting-input/v1"},
    {"decision_id","decision"},{"book_id","BOOK"},{"source_day","2026-09-26"},
    {"previous_day","2026-09-25"},{"accounting_source_id","synthetic-ledger"},
    {"prior_finalization_source_id","synthetic-finalization"},
    {"timestamp","2026-09-26T00:00:00Z"},{"currency","USD"},
    {"previous_positions",J::array({{{"key",key("alpha","2026-09-25")},
        {"quantity_exact","4"},{"average_price_exact","100"}}})},
    {"previous_totals",J::array({{{"strategy_id","ENGINE"},{"equity_exact","1000"},{"total_pnl_exact","20"}}})},
    {"cost_config",{{"explicit_fee_per_contract","1.5"},{"min_adv","100"},
        {"min_participation","0"},{"max_participation","0.1"}}},
    {"instruments",J::array({{{"symbol","SYN"},{"price_exact","101"},{"adv_exact","100000"},
        {"volatility_multiplier_exact","1"},{"source_id","synthetic-prior-close"},
        {"baseline_spread_ticks","1"},{"min_spread_ticks","1"},{"max_spread_ticks","10"},
        {"spread_cost_multiplier","0.5"},{"max_impact_bps","100"},{"tick_size","0.01"},
        {"point_value","5"},{"max_total_implicit_bps","200"}}})}};}
TEST(QtDeskCycleTest, ActualSharedCostModelProducesPriorCloseExecutionAndEquity) {
    auto r=produce_qt_futures_accounting(decision(),choices(),inputs());
    ASSERT_TRUE(r.is_ok());const auto& o=r.value();
    EXPECT_EQ(o["observation"]["fills"][0]["selected_quantity_exact"],"5");
    EXPECT_EQ(o["observation"]["fills"][0]["average_price_exact"],"100");
    ASSERT_EQ(o["executions"].size(),1U);
    EXPECT_EQ(o["executions"][0]["quantity_exact"],"1");
    EXPECT_EQ(o["executions"][0]["price_exact"],"101");
    transaction_cost::TransactionCostManager cost;
    transaction_cost::AssetCostConfig asset;asset.symbol="SYN";asset.point_value=5;
    cost.register_asset_config(asset);
    auto expected=cost.calculate_costs("SYN",1,101,100000,1);
    auto charge=Decimal(expected.total_transaction_costs);
    EXPECT_EQ(o["executions"][0]["total_transaction_costs_exact"],charge.to_string());
    EXPECT_EQ(o["live_results"][0]["current_portfolio_value_exact"],(Decimal(1000)-charge).to_string());
    EXPECT_EQ(o["live_results"][0]["daily_pnl_exact"],(-charge).to_string());
}
TEST(QtDeskCycleTest, GovernedV2PreservesBinary64CostOperandsBeyondDecimal8){
    auto input=inputs();input["schema_version"]="qt-futures-accounting-input/v2";
    input["market_source_id"]="market";input["market_source_digest"]=std::string(64,'a');
    input["prior_finalization_digest"]=std::string(64,'b');
    auto& m=input["instruments"][0];m["price_model_number"]=m["price_exact"];m.erase("price_exact");
    m["adv_model_number"]="100000.12345678901";m.erase("adv_exact");
    m["volatility_multiplier_model_number"]="1.1234567890123457";m.erase("volatility_multiplier_exact");
    auto result=produce_qt_futures_accounting(decision(),choices(),input);ASSERT_TRUE(result.is_ok());
    transaction_cost::TransactionCostManager manager;transaction_cost::AssetCostConfig asset;asset.symbol="SYN";asset.point_value=5;manager.register_asset_config(asset);
    auto expected=manager.calculate_costs("SYN",1,101,100000.12345678901,1.1234567890123457);
    EXPECT_EQ(result.value()["executions"][0]["total_transaction_costs_exact"],Decimal(expected.total_transaction_costs).to_string());
    EXPECT_EQ(result.value()["observation"]["fills"][0]["selected_quantity_exact"],"5");
}
TEST(QtDeskCycleTest, QuietDayProducesEquityWithoutFakeTrade) {
    auto s=choices();s[0]["quantity_exact"]="4";
    auto r=produce_qt_futures_accounting(decision(),s,inputs());ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value()["executions"].empty());
    EXPECT_EQ(r.value()["live_results"][0]["current_portfolio_value_exact"],"1000");
    EXPECT_EQ(r.value()["observation"]["fills"][0]["observation_kind"],"carried");
    s[0]["average_price_exact"]="101";
    EXPECT_TRUE(produce_qt_futures_accounting(decision(),s,inputs()).is_error());
}
TEST(QtDeskCycleTest, SharedTotalIsRoundedOnceAtFractionalCostBoundary) {
    auto input=inputs();
    input["cost_config"]["explicit_fee_per_contract"]="0.00000001";
    input["instruments"][0]["tick_size"]="0.00000003";
    input["instruments"][0]["point_value"]="1";
    input["instruments"][0]["max_impact_bps"]="0";
    transaction_cost::TransactionCostManager::Config config;
    config.explicit_fee_per_contract=0.00000001;
    transaction_cost::TransactionCostManager costs(config);
    transaction_cost::AssetCostConfig asset;asset.symbol="SYN";
    asset.tick_size=0.00000003;asset.point_value=1;asset.max_impact_bps=0;
    costs.register_asset_config(asset);
    const auto expected=costs.calculate_costs("SYN",1,101,100000,1);
    const auto total=Decimal(expected.total_transaction_costs);
    ASSERT_NE(total,Decimal(expected.commissions_fees)+Decimal(expected.slippage_market_impact));
    const auto result=produce_qt_futures_accounting(decision(),choices(),input);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value()["executions"][0]["total_transaction_costs_exact"],total.to_string());
    EXPECT_EQ(result.value()["observation"]["results"]["currency_totals"][0]["actual_cash_cost_exact"],total.to_string());
    EXPECT_EQ(result.value()["live_results"][0]["daily_pnl_exact"],(-total).to_string());
}
TEST(QtDeskCycleTest, SplitOwnersDoNotNetAwayAndResultsAreOrderIndependent) {
    auto s=choices(),i=inputs();auto other=s[0];other["key"]=key("beta","2026-09-26","qt_proposal");
    other["quantity_exact"]="3";s.push_back(other);
    auto previous=i["previous_positions"][0];previous["key"]=key("beta","2026-09-25");
    i["previous_positions"].push_back(previous);
    auto a=produce_qt_futures_accounting(decision(),s,i);ASSERT_TRUE(a.is_ok());
    ASSERT_EQ(a.value()["executions"].size(),2U);
    EXPECT_NE(a.value()["executions"][0]["exec_id"],a.value()["executions"][1]["exec_id"]);
    std::reverse(s.begin(),s.end());std::reverse(i["previous_positions"].begin(),i["previous_positions"].end());
    auto b=produce_qt_futures_accounting(decision(),s,i);ASSERT_TRUE(b.is_ok());EXPECT_EQ(a.value(),b.value());
}
TEST(QtDeskCycleTest, MissingOrInvalidDataNeverFallsBack) {
    auto boundary=inputs();boundary["cost_config"]["explicit_fee_per_contract"]="92233720368.54775807";
    EXPECT_TRUE(produce_qt_futures_accounting(decision(),choices(),boundary).is_error());
    auto missing=inputs();missing.erase("prior_finalization_source_id");
    EXPECT_TRUE(produce_qt_futures_accounting(decision(),choices(),missing).is_error());
    for(auto field:{"price_exact","adv_exact","point_value","source_id"}) {
        auto i=inputs();i["instruments"][0].erase(field);
        EXPECT_TRUE(produce_qt_futures_accounting(decision(),choices(),i).is_error()) << field;
    }
    auto i=inputs();i["previous_totals"]=J::array();
    EXPECT_TRUE(produce_qt_futures_accounting(decision(),choices(),i).is_error());
    i=inputs();i["previous_day"]="2026-09-27";
    EXPECT_TRUE(produce_qt_futures_accounting(decision(),choices(),i).is_error());
    i=inputs();i["instruments"][0]["adv_exact"]="0";
    EXPECT_TRUE(produce_qt_futures_accounting(decision(),choices(),i).is_error());
}
TEST(QtDeskCycleTest, EquityFractionalFutureDuplicateAndOmittedPriorOwnerRefused) {
    auto s=choices();s[0]["asset_type"]="EQUITY";
    EXPECT_TRUE(produce_qt_futures_accounting(decision(),s,inputs()).is_error());
    s=choices();s[0]["quantity_exact"]="5.5";
    EXPECT_TRUE(produce_qt_futures_accounting(decision(),s,inputs()).is_error());
    s=choices();s.push_back(s[0]);
    EXPECT_TRUE(produce_qt_futures_accounting(decision(),s,inputs()).is_error());
    auto i=inputs();i["previous_positions"][0]["key"]["strategy_name"]="unselected";
    EXPECT_TRUE(produce_qt_futures_accounting(decision(),choices(),i).is_error());
}
TEST(QtDeskCycleTest, NewSymbolUsesExecutionBasisAndCloseRemainsExplicit) {
    auto i=inputs();i["previous_positions"]=J::array();auto s=choices();s[0]["average_price_exact"]=nullptr;
    auto r=produce_qt_futures_accounting(decision(),s,i);ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value()["observation"]["fills"][0]["average_price_exact"],"101");
    s=choices();s[0]["quantity_exact"]="0";r=produce_qt_futures_accounting(decision(),s,inputs());
    ASSERT_TRUE(r.is_ok());EXPECT_EQ(r.value()["executions"][0]["side"],"SELL");
    EXPECT_EQ(r.value()["executions"][0]["quantity_exact"],"4");
}
TEST(QtDeskCycleTest, ModelChoicePriorExecutionAndSolverDiagnosticsRemainDistinct) {
    auto d=decision();d["model_publication_id"]="model";d["preview_id"]="preview";
    auto selected=choices();selected[0]["quantity_exact"]="7";
    auto in=inputs();in["previous_positions"][0]["quantity_exact"]="5";
    auto source=choices();source[0]["quantity_exact"]="4";
    J evaluation={{"optimizer",{{"status","evaluated"},{"diagnostics",J::array()},
        {"aggregate_bindings",J::array({{{"instrument_type","FUTURE"},{"symbol","SYN"},{"component_keys",J::array({selected[0]["key"]})},{"previous_net_quantity_exact","5"},{"proposed_net_quantity_exact","7"}}})},
        {"current_weights",J::array({{{"instrument_type","FUTURE"},{"symbol","SYN"},{"weight_diagnostic","0.5"}}})},
        {"target_weights",J::array({{{"instrument_type","FUTURE"},{"symbol","SYN"},{"weight_diagnostic","0.7"}}})},
        {"solved_weights",J::array({{{"instrument_type","FUTURE"},{"symbol","SYN"},{"weight_diagnostic","0.6"}}})},
        {"cost_penalty","0.01"},{"trace",J::array({"actual_iterations=2","tracking_error=0.2",R"({"buffer_branch":"applied","solver_positions":["0.65"],"continuous_buffered_positions":["0.62"],"rounded_buffered_positions":["0.6"]})"})}}},
        {"selected_risk",{{"status","evaluated"},{"diagnostics",J::array()},{"metrics",J::array()}}},
        {"selected_costs",{{"status","evaluated"},{"diagnostics",J::array()}}}};
    for(auto code:{"portfolio_multiplier","jump_multiplier","correlation_multiplier","leverage_multiplier"})
        evaluation["selected_risk"]["metrics"].push_back({{"code",code},{"value_diagnostic",std::string(code)=="leverage_multiplier"?"1":"0.5"},{"unit","ratio"},{"source_id","risk"}});
    J preview={{"selection_rows",selected},{"evaluation",evaluation}},facts={{"source_rows",source}};
    for(auto prior:{"5","7"}){
        in["previous_positions"][0]["quantity_exact"]=prior;
        auto accounting=produce_qt_futures_accounting(d,selected,in);ASSERT_TRUE(accounting.is_ok());
        auto result=build_qt_desk_diagnostics(d,preview,facts,in,accounting.value());ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value()["model_to_choice"][0]["quantity_delta_exact"],"3");
        EXPECT_EQ(result.value()["model_to_choice"][0]["notional_delta_exact"],"1515");
        EXPECT_EQ(result.value()["execution_distance"][0]["execution_delta_exact"],std::string(prior)=="5"?"2":"0");
        EXPECT_EQ(result.value()["optimizer"]["actual_iterations"],2);
        EXPECT_EQ(result.value()["optimizer"]["aggregate_recommendation"][0]["moved_by"],J::array({"solver","buffer","rounding"}));
        EXPECT_EQ(result.value()["risk"]["binding_multipliers"].size(),3U);
        EXPECT_TRUE(result.value()["controls_applied"].empty());
    }
    auto accounting=produce_qt_futures_accounting(d,selected,in);ASSERT_TRUE(accounting.is_ok());
    auto returned=preview;
    returned["evaluation"]["optimizer"]["solved_weights"][0]["weight_diagnostic"]="0.5";
    returned["evaluation"]["optimizer"]["trace"]=J::array({"actual_iterations=2","tracking_error=0.2",R"({"buffer_branch":"returned_prior","solver_positions":["0.65"],"continuous_buffered_positions":null,"rounded_buffered_positions":null})"});
    auto retained=build_qt_desk_diagnostics(d,returned,facts,in,accounting.value());ASSERT_TRUE(retained.is_ok());
    EXPECT_EQ(retained.value()["optimizer"]["aggregate_recommendation"][0]["moved_by"],J::array({"solver","buffer"}));
    // Distinct futures symbols use canonical binding order independently of the
    // incoming selected/source/market/prior row order.
    auto second=selected[0];second["key"]["symbol"]="ZYN";selected.push_back(second);
    second=source[0];second["key"]["symbol"]="ZYN";source.push_back(second);
    second=in["previous_positions"][0];second["key"]["symbol"]="ZYN";in["previous_positions"].push_back(second);
    second=in["instruments"][0];second["symbol"]="ZYN";in["instruments"].push_back(second);
    auto& opt=evaluation["optimizer"];
    second=opt["aggregate_bindings"][0];second["symbol"]="ZYN";second["component_keys"][0]["symbol"]="ZYN";opt["aggregate_bindings"].push_back(second);
    for(auto field:{"current_weights","target_weights","solved_weights"}){
        second=opt[field][0];second["symbol"]="ZYN";second["weight_diagnostic"]="0.8";opt[field].push_back(second);
    }
    opt["trace"]=J::array({"actual_iterations=2","tracking_error=0.2",R"({"buffer_branch":"applied","solver_positions":["0.65","0.8"],"continuous_buffered_positions":["0.62","0.8"],"rounded_buffered_positions":["0.6","0.8"]})"});
    preview={{"selection_rows",selected},{"evaluation",evaluation}};facts["source_rows"]=source;
    accounting=produce_qt_futures_accounting(d,selected,in);ASSERT_TRUE(accounting.is_ok());
    auto ordered=build_qt_desk_diagnostics(d,preview,facts,in,accounting.value());ASSERT_TRUE(ordered.is_ok());
    EXPECT_EQ(ordered.value()["optimizer"]["aggregate_recommendation"][0]["moved_by"],J::array({"solver","buffer","rounding"}));
    EXPECT_TRUE(ordered.value()["optimizer"]["aggregate_recommendation"][1]["moved_by"].empty());
    std::reverse(preview["selection_rows"].begin(),preview["selection_rows"].end());
    std::reverse(facts["source_rows"].begin(),facts["source_rows"].end());
    std::reverse(in["instruments"].begin(),in["instruments"].end());
    std::reverse(in["previous_positions"].begin(),in["previous_positions"].end());
    auto reversed=build_qt_desk_diagnostics(d,preview,facts,in,accounting.value());ASSERT_TRUE(reversed.is_ok());
    EXPECT_EQ(ordered.value(),reversed.value());
    auto governed=in;governed["schema_version"]="qt-futures-accounting-input/v2";
    for(auto& instrument:governed["instruments"]){instrument["price_model_number"]="101.000000001";instrument.erase("price_exact");}
    auto precise=build_qt_desk_diagnostics(d,preview,facts,governed,accounting.value());ASSERT_TRUE(precise.is_ok());
    EXPECT_EQ(precise.value()["model_to_choice"][0]["notional_delta_exact"],"1515.000000015");
    EXPECT_EQ(precise.value()["model_to_choice"][0]["price_model_number"],"101.000000001");
    facts["source_rows"]=J::array();
    auto absent=build_qt_desk_diagnostics(d,preview,facts,in,accounting.value());ASSERT_TRUE(absent.is_ok());
    EXPECT_EQ(absent.value()["model_to_choice"][0]["status"],"unavailable");
    EXPECT_TRUE(absent.value()["model_to_choice"][0]["notional_delta_exact"].is_null());
}
}}
