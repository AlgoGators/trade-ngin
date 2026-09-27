#include <gtest/gtest.h>
#include "trade_ngin/apps/qt_equity_desk_cycle.hpp"

using namespace trade_ngin;
namespace {
using J=nlohmann::json;
constexpr const char* day="2026-09-25";
constexpr const char* prior_day="2026-09-24";
J key(std::string owner="owner-a",std::string date=day,std::string engine="ENGINE") {
    return {{"portfolio_id","BOOK"},{"strategy_id",engine},{"strategy_name",owner},
            {"date",date},{"symbol","SYN"},{"portfolio_type","qt"}};
}
J decision() {
    return {{"decision_id","10000000-0000-0000-0000-000000000001"},
            {"book_id","BOOK"},{"source_day",day}};
}
J selection() {
    return J::array({{{"key",key()},{"asset_type","EQUITY"},{"editable",true},
                     {"quantity_exact","0.5"},{"average_price_exact","999"}}});
}
J price(std::string value="14",std::string frame="adjusted-1") {
    return {{"source_id","owned-synthetic-close"},{"source_digest",std::string(64,'a')},
            {"date",prior_day},{"price_frame_id",frame},{"price_model_number",value}};
}
J input() {
    J costs={{"tick_constrained",false},{"commission_per_unit","0.005"},
        {"min_commission_per_order","1"},{"max_commission_per_order","100"},
        {"max_commission_pct","0.01"},{"sec_fee_per_million","20.6"},
        {"finra_taf_per_share","0.000195"},{"finra_taf_cap_per_trade","9.79"},
        {"apply_regulatory_fees",false},{"baseline_spread_ticks","1"},
        {"min_spread_ticks","1"},{"max_spread_ticks","10"},
        {"spread_cost_multiplier","0.5"},{"max_impact_bps","100"},
        {"tick_size","0.01"},{"point_value","1"},{"max_total_implicit_bps","0"}};
    return {{"schema_version","qt-equity-accounting-input/v1"},
        {"calculation_version","qt-equity-main08b15c/v1"},
        {"decision_id",decision().at("decision_id")},{"book_id","BOOK"},{"source_day",day},
        {"accounting_input_id","20000000-0000-0000-0000-000000000001"},
        {"market_source_id","30000000-0000-0000-0000-000000000001"},
        {"market_source_digest",std::string(64,'a')},
        {"accounting_source_id","owned-synthetic-equity-accounting"},
        {"prior_finalization_source_id","qt-finalization/40000000-0000-0000-0000-000000000001"},
        {"prior_finalization_digest",std::string(64,'b')},
        {"previous_day",prior_day},{"timestamp",std::string(day)+"T00:00:00Z"},
        {"day_mode","open"},{"currency","USD"},
        {"previous_positions",J::array({{{"key",key("owner-a",prior_day)},
            {"quantity_exact","1.5"},{"average_price_exact","10"},
            {"daily_realized_pnl_exact","9"},{"daily_unrealized_pnl_exact","6"},
            {"last_update",std::string(prior_day)+"T00:00:00Z"},
            {"basis_evidence",{{"source_id","owned-prior-position"},
                {"source_digest",std::string(64,'c')},{"price_frame_id","adjusted-1"},
                {"formed_day","2026-09-20"}}}}})},
        {"previous_totals",J::array({{{"strategy_id","ENGINE"},{"initial_capital_exact","1000"},
            {"equity_exact","1026"},{"total_pnl_exact","26"},
            {"total_realized_pnl_exact","30"},{"total_transaction_costs_exact","10"},
            {"total_unrealized_pnl_exact","6"}}})},
        {"instruments",J::array({{{"symbol","SYN"},{"asset_type","EQUITY"},
            {"reference",price()},{"mark",price()},
            {"cost_evidence",{{"source_id","owned-synthetic-cost-history"},
                {"source_digest",std::string(64,'d')},{"date",prior_day},
                {"adv_model_number","1000000"},{"volatility_multiplier_model_number","1"}}},
            {"cost_parameters",costs}}})},
        {"cost_config",{{"explicit_fee_per_contract","1.5"},{"min_adv","10000"},
            {"min_participation","0"},{"max_participation","1"}}},
        {"actions",J::array()}};
}
void anchors(J& in,std::string unreal,std::string pnl,std::string equity) {
    auto& t=in["previous_totals"][0];t["total_unrealized_pnl_exact"]=unreal;
    t["total_pnl_exact"]=pnl;t["equity_exact"]=equity;
}
J action(std::string type="SPLIT",std::string value="2") {
    return {{"key",key()},{"source_id","owned-action"},{"source_digest",std::string(64,'e')},
        {"type",type},{"ex_date",prior_day},{"value_model_number",value},
        {"basis_provenance","formed_on_or_before_ex_date"},
        {"basis_provenance_evidence","owned immediate prior basis evidence"},
        {"frame_before","adjusted-1"},{"frame_after","adjusted-2"},
        {"raw_close_model_number",nullptr},{"eligible_quantity_exact",nullptr}};
}
void split_prices(J& in) {
    for(auto p:{"reference","mark"}) {
        in["instruments"][0][p]["price_frame_id"]="adjusted-2";
        in["instruments"][0][p]["price_model_number"]="7";
    }
}
// The confirmed noneditable quantity is already the exact post-event holding;
// its saved advisory basis is the actual pre-event source basis, not a choice.
void noneditable_action(J& s,J& in,bool dividend) {
    s[0]["editable"]=false;s[0]["quantity_exact"]="8";s[0]["average_price_exact"]="25";
    auto& prior=in["previous_positions"][0];prior["quantity_exact"]=dividend?"8":"4";
    prior["average_price_exact"]="25";prior["daily_unrealized_pnl_exact"]="0";
    anchors(in,"0","20","1020");
    auto a=action(dividend?"DIVIDEND":"SPLIT",dividend?"1":"2");
    a["basis_provenance_evidence"]=prior["basis_evidence"]["source_id"];
    if(dividend){a["raw_close_model_number"]="100";a["eligible_quantity_exact"]="8";}
    in["actions"].push_back(a);
    for(auto p:{"reference","mark"}){in["instruments"][0][p]["price_frame_id"]="adjusted-2";
        in["instruments"][0][p]["price_model_number"]=dividend?"24.75247525":"12.5";}
}
void refused(const J& d,const J& s,const J& in) {
    const auto before=J::array({d,s,in});auto result=produce_qt_equity_accounting(d,s,in);
    EXPECT_TRUE(result.is_error());EXPECT_EQ(J::array({d,s,in}),before);
}
}

TEST(QtEquityDeskCycle, FractionalReductionChargesOnceAndUsesUnrealizedFlow) {
    const auto d=decision(),s=selection(),in=input();const auto before=J::array({d,s,in});
    auto r=produce_qt_equity_accounting(d,s,in);ASSERT_TRUE(r.is_ok());const auto& out=r.value();
    EXPECT_EQ(out.at("schema_version"),"qt-equity-accounting/v1");
    EXPECT_EQ(out.at("calculation_version"),"qt-equity-main08b15c/v1");
    const auto& obs=out.at("observation");EXPECT_EQ(obs.at("schema_version"),"qt-execution/v2");
    EXPECT_EQ(obs.at("accounting_input_id"),in.at("accounting_input_id"));
    const auto& fill=obs.at("fills").at(0);
    EXPECT_EQ(fill.at("key"),key());EXPECT_EQ(fill.at("selected_quantity_exact"),"0.5");
    EXPECT_EQ(fill.at("average_price_exact"),"10"); // proposed 999 never changes actual basis
    EXPECT_EQ(fill.at("actual_cash_cost_exact"),"0.14");
    EXPECT_EQ(fill.at("daily_realized_pnl_exact"),"4");
    EXPECT_EQ(fill.at("daily_unrealized_pnl_exact"),"2"); // current snapshot
    const auto& live=out.at("live_results").at(0);
    EXPECT_EQ(live.at("daily_realized_pnl_exact"),"4");
    EXPECT_EQ(live.at("daily_unrealized_pnl_exact"),"-4"); // daily flow
    EXPECT_EQ(live.at("daily_pnl_exact"),"-0.14");
    EXPECT_EQ(live.at("total_realized_pnl_exact"),"34");
    EXPECT_EQ(live.at("total_transaction_costs_exact"),"10.14");
    EXPECT_EQ(live.at("total_unrealized_pnl_exact"),"2");
    EXPECT_EQ(live.at("total_pnl_exact"),"25.86");
    EXPECT_EQ(live.at("current_portfolio_value_exact"),"1025.86");
    EXPECT_EQ(out.at("equity_curve").at(0).at("equity_exact"),"1025.86");
    const auto& exec=out.at("executions").at(0);
    EXPECT_EQ(exec.at("key"),key());EXPECT_EQ(exec.at("side"),"SELL");
    EXPECT_EQ(exec.at("quantity_exact"),"1");EXPECT_EQ(exec.at("price_exact"),"14");
    EXPECT_EQ(exec.at("commissions_fees_exact"),"0.14");
    EXPECT_EQ(out.at("distance").at(0).at("execution_delta_exact"),"-1");
    EXPECT_EQ(J::array({d,s,in}),before);
}

TEST(QtEquityDeskCycle, QuietOpenDayDoesNotInventTradeOrCost) {
    auto s=selection();s[0]["quantity_exact"]="1.5";auto r=produce_qt_equity_accounting(decision(),s,input());
    ASSERT_TRUE(r.is_ok());const auto& o=r.value();EXPECT_TRUE(o.at("executions").empty());
    EXPECT_EQ(o.at("observation").at("fills").at(0).at("observation_kind"),"carried");
    EXPECT_EQ(o.at("observation").at("fills").at(0).at("actual_cash_cost_exact"),"0");
    EXPECT_EQ(o.at("live_results").at(0).at("daily_pnl_exact"),"0");
    EXPECT_EQ(o.at("live_results").at(0).at("current_portfolio_value_exact"),"1026");
}

TEST(QtEquityDeskCycle, AuthenticPriorIntradayTimestampIsPreservedAsInputEvidence) {
    auto in=input();in["previous_positions"][0]["last_update"]="2026-09-24T15:31:42Z";
    const auto frozen=in;auto r=produce_qt_equity_accounting(decision(),selection(),in);
    ASSERT_TRUE(r.is_ok());EXPECT_EQ(in,frozen);
    EXPECT_EQ(r.value().at("observation").at("fills")[0].at("last_update"),"2026-09-25T00:00:00Z");
}

TEST(QtEquityDeskCycle, ExplicitPriorFinalizationAtCurrentValuationTimeIsAdmitted) {
    auto in=input();in["previous_positions"][0]["last_update"]="2026-09-25T00:00:00Z";
    const auto frozen=in;auto r=produce_qt_equity_accounting(decision(),selection(),in);
    ASSERT_TRUE(r.is_ok());EXPECT_EQ(in,frozen);
}

TEST(QtEquityDeskCycle, FlipRealizesOnlyClosedHoldingAndRetainsFractionalShort) {
    auto s=selection();s[0]["quantity_exact"]="-0.5";
    auto r=produce_qt_equity_accounting(decision(),s,input());ASSERT_TRUE(r.is_ok());const auto& o=r.value();
    const auto& f=o.at("observation").at("fills").at(0);
    EXPECT_EQ(f.at("selected_quantity_exact"),"-0.5");EXPECT_EQ(f.at("average_price_exact"),"14");
    EXPECT_EQ(f.at("daily_realized_pnl_exact"),"6");EXPECT_EQ(f.at("daily_unrealized_pnl_exact"),"0");
    EXPECT_EQ(f.at("actual_cash_cost_exact"),"0.28");
    EXPECT_EQ(o.at("live_results").at(0).at("daily_pnl_exact"),"-0.28");
}

TEST(QtEquityDeskCycle, ExplicitZeroCloseProducesACompleteSuccessor) {
    auto s=selection();s[0]["quantity_exact"]="0";
    auto r=produce_qt_equity_accounting(decision(),s,input());ASSERT_TRUE(r.is_ok());
    const auto& f=r.value().at("observation").at("fills").at(0);
    EXPECT_EQ(f.at("selected_quantity_exact"),"0");EXPECT_EQ(f.at("daily_realized_pnl_exact"),"6");
    EXPECT_EQ(f.at("daily_unrealized_pnl_exact"),"0");
    EXPECT_EQ(r.value().at("executions").at(0).at("quantity_exact"),"1.5");
}

TEST(QtEquityDeskCycle, ExactTinyQuantityIsNeverRoundedToContractsOrSuppressed) {
    auto s=selection(),in=input();s[0]["quantity_exact"]="1.50000001";
    auto r=produce_qt_equity_accounting(decision(),s,in);ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().at("observation").at("fills").at(0).at("selected_quantity_exact"),"1.50000001");
    EXPECT_EQ(r.value().at("executions").at(0).at("quantity_exact"),"0.00000001");
}

TEST(QtEquityDeskCycle, NewHoldingUsesActualFillBasisAndExplicitPriorCapital) {
    auto s=selection(),in=input();s[0]["quantity_exact"]="0.25";s[0]["average_price_exact"]=nullptr;
    in["previous_positions"]=J::array();anchors(in,"0","20","1020");
    in["instruments"][0]["mark"]["price_model_number"]="18";
    auto r=produce_qt_equity_accounting(decision(),s,in);ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().at("observation").at("fills").at(0).at("average_price_exact"),"14");
    EXPECT_EQ(r.value().at("observation").at("fills").at(0).at("daily_unrealized_pnl_exact"),"1");
}

TEST(QtEquityDeskCycle, OppositeOwnersRemainSeparateAndBothActualCostsAreCharged) {
    auto s=selection(),in=input();auto other=s[0];other["key"]=key("owner-b");other["quantity_exact"]="-0.5";s.push_back(other);
    auto p=in["previous_positions"][0];p["key"]=key("owner-b",prior_day);p["quantity_exact"]="-1.5";
    p["daily_unrealized_pnl_exact"]="-6";in["previous_positions"].push_back(p);anchors(in,"0","20","1020");
    auto r=produce_qt_equity_accounting(decision(),s,in);ASSERT_TRUE(r.is_ok());const auto& o=r.value();
    ASSERT_EQ(o.at("executions").size(),2U);ASSERT_EQ(o.at("observation").at("fills").size(),2U);
    EXPECT_NE(o.at("executions")[0].at("exec_id"),o.at("executions")[1].at("exec_id"));
    EXPECT_EQ(o.at("observation").at("results").at("currency_totals")[0].at("actual_cash_cost_exact"),"0.28");
    EXPECT_EQ(o.at("live_results")[0].at("daily_pnl_exact"),"-0.28");
}

TEST(QtEquityDeskCycle, NoneditableOtherEngineIsPreservedWithItsOwnAnchor) {
    auto s=selection(),in=input();auto other=s[0];other["key"]=key("other-owner",day,"OTHER");
    other["quantity_exact"]="1.5";other["average_price_exact"]="10";other["editable"]=false;s.push_back(other);
    auto p=in["previous_positions"][0];p["key"]=key("other-owner",prior_day,"OTHER");in["previous_positions"].push_back(p);
    auto t=in["previous_totals"][0];t["strategy_id"]="OTHER";in["previous_totals"].push_back(t);
    auto r=produce_qt_equity_accounting(decision(),s,in);ASSERT_TRUE(r.is_ok());ASSERT_EQ(r.value().at("live_results").size(),2U);
    for(const auto& live:r.value().at("live_results"))if(live.at("strategy_id")=="OTHER") {
        EXPECT_EQ(live.at("daily_pnl_exact"),"0");EXPECT_EQ(live.at("current_portfolio_value_exact"),"1026");
    }
}

TEST(QtEquityDeskCycle, SplitRestatesPriorBeforeDeltaWithoutAddingFictitiousIncome) {
    auto s=selection(),in=input();s[0]["quantity_exact"]="3";in["actions"].push_back(action());
    split_prices(in);
    auto r=produce_qt_equity_accounting(decision(),s,in);ASSERT_TRUE(r.is_ok());const auto& o=r.value();
    EXPECT_TRUE(o.at("executions").empty());const auto& f=o.at("observation").at("fills")[0];
    EXPECT_EQ(f.at("average_price_exact"),"5");EXPECT_EQ(f.at("daily_unrealized_pnl_exact"),"6");
    EXPECT_EQ(f.at("daily_realized_pnl_exact"),"0");EXPECT_EQ(o.at("live_results")[0].at("daily_pnl_exact"),"0");
    ASSERT_EQ(o.at("corporate_action_adjustments").size(),1U);
    EXPECT_EQ(o.at("corporate_action_adjustments")[0].at("key"),key());
    EXPECT_EQ(o.at("distance")[0].at("restated_previous_quantity_exact"),"3");
}

TEST(QtEquityDeskCycle, DividendChangesBasisOnlyAndNeverAddsEligibilityCashToPnl) {
    auto s=selection(),in=input();s[0]["quantity_exact"]="1.5";
    in["previous_positions"][0]["average_price_exact"]="12";in["previous_positions"][0]["daily_unrealized_pnl_exact"]="3";anchors(in,"3","23","1023");
    auto a=action("DIVIDEND","2");a["raw_close_model_number"]="10";a["eligible_quantity_exact"]="3";in["actions"].push_back(a);
    for(auto p:{"reference","mark"}) {in["instruments"][0][p]["price_frame_id"]="adjusted-2";in["instruments"][0][p]["price_model_number"]="11";}
    auto r=produce_qt_equity_accounting(decision(),s,in);ASSERT_TRUE(r.is_ok());const auto& o=r.value();
    EXPECT_TRUE(o.at("executions").empty());EXPECT_EQ(o.at("observation").at("fills")[0].at("average_price_exact"),"10");
    EXPECT_EQ(o.at("live_results")[0].at("daily_realized_pnl_exact"),"0");
    EXPECT_EQ(o.at("live_results")[0].at("daily_pnl_exact"),"-1.5");
}

TEST(QtEquityDeskCycle, PositivelyProvedPostEventBasisIsNotRestatedAgain) {
    auto s=selection(),in=input();s[0]["quantity_exact"]="1.5";
    in["previous_positions"][0]["basis_evidence"]["formed_day"]=prior_day;
    auto a=action();a["ex_date"]="2026-09-23";a["basis_provenance"]="formed_after_ex_date";
    a["frame_after"]="adjusted-1";in["actions"].push_back(a);
    auto r=produce_qt_equity_accounting(decision(),s,in);ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().at("observation").at("fills")[0].at("average_price_exact"),"10");
    EXPECT_TRUE(r.value().at("corporate_action_adjustments").empty());
}

TEST(QtEquityDeskCycle, ExistingSharedEquityImplicitCostRemainsSeparateFromGrossRealized) {
    auto in=input();in["instruments"][0]["cost_parameters"]["max_total_implicit_bps"]="200";
    auto r=produce_qt_equity_accounting(decision(),selection(),in);ASSERT_TRUE(r.is_ok());
    const auto& e=r.value().at("executions")[0];
    EXPECT_EQ(e.at("commissions_fees_exact"),"0.14");
    EXPECT_EQ(e.at("slippage_market_impact_exact"),"0.005028");
    EXPECT_EQ(e.at("total_transaction_costs_exact"),"0.145028");
    EXPECT_EQ(r.value().at("observation").at("fills")[0].at("daily_realized_pnl_exact"),"4");
    EXPECT_EQ(r.value().at("live_results")[0].at("daily_pnl_exact"),"-0.145028");
}

TEST(QtEquityDeskCycle, MissingAnchorsOrFinancialEvidenceNeverDefault) {
    for(auto field:{"previous_totals","prior_finalization_digest","market_source_id","accounting_input_id","cost_config","actions"}) {
        auto in=input();in.erase(field);refused(decision(),selection(),in);
    }
    for(auto field:{"reference","mark","cost_evidence","cost_parameters"}) {
        auto in=input();in["instruments"][0].erase(field);refused(decision(),selection(),in);
    }
}
TEST(QtEquityDeskCycle, RehashedContradictoryPriorFinancialAnchorsAreRefused) {
    for(auto field:{"equity_exact","total_pnl_exact","total_unrealized_pnl_exact","initial_capital_exact"}) {
        auto in=input();in["previous_totals"][0][field]="999";refused(decision(),selection(),in);
    }
}
TEST(QtEquityDeskCycle, OmittedPriorOwnerOrDuplicateOwnerCannotCreatePartialSuccessor) {
    auto in=input(),s=selection();auto p=in["previous_positions"][0];p["key"]=key("missing",prior_day);
    in["previous_positions"].push_back(p);anchors(in,"12","32","1032");refused(decision(),s,in);
    in=input();s.push_back(s[0]);refused(decision(),s,in);
    in=input();in["previous_positions"].push_back(in["previous_positions"][0]);refused(decision(),selection(),in);
}
TEST(QtEquityDeskCycle, WrongOwnerDayBookOrAssetTypeIsRefused) {
    for(auto field:{"portfolio_id","date","portfolio_type"}) {
        auto s=selection();s[0]["key"][field]="foreign";refused(decision(),s,input());
    }
    auto s=selection();s[0]["asset_type"]="FUTURE";refused(decision(),s,input());
    auto in=input();in["decision_id"]="10000000-0000-0000-0000-000000000002";refused(decision(),selection(),in);
}
TEST(QtEquityDeskCycle, NoNoneditableTradeOrNewHoldingIsAdmitted) {
    auto s=selection();s[0]["editable"]=false;refused(decision(),s,input());
    auto in=input();in["previous_positions"]=J::array();anchors(in,"0","20","1020");refused(decision(),s,in);
}
TEST(QtEquityDeskCycle, PriceAndCostEvidenceMustBeStrictFiniteDatedAndFrameConsistent) {
    for(auto number:{"nan","inf","1e999","0","-1"," 14"}) {
        auto in=input();in["instruments"][0]["reference"]["price_model_number"]=number;refused(decision(),selection(),in);
    }
    for(auto name:{"reference","mark","cost_evidence"}) {
        auto in=input();in["instruments"][0][name]["date"]=day;refused(decision(),selection(),in);
    }
    auto in=input();in["instruments"][0]["mark"]["price_frame_id"]="foreign-frame";refused(decision(),selection(),in);
    for(auto stamp:{"2026-09-23T23:59:59Z","2026-09-25T00:00:01Z","2026-09-24T15:31:42","2026-09-24T99:00:00Z"}) {
        in=input();in["previous_positions"][0]["last_update"]=stamp;refused(decision(),selection(),in);
    }
}
TEST(QtEquityDeskCycle, UnknownOrExtraMarketCoverageAndCostFallbackAreRefused) {
    auto in=input();in["instruments"]=J::array();refused(decision(),selection(),in);
    in=input();auto extra=in["instruments"][0];extra["symbol"]="EXTRA";in["instruments"].push_back(extra);refused(decision(),selection(),in);
    in=input();in["instruments"][0]["cost_parameters"]["commission_per_unit"]="-1";refused(decision(),selection(),in);
    in=input();in["instruments"][0]["cost_parameters"].erase("apply_regulatory_fees");refused(decision(),selection(),in);
    in=input();in["instruments"][0]["cost_parameters"]["point_value"]="50";refused(decision(),selection(),in);
}
TEST(QtEquityDeskCycle, BrokenOrUnknownActionProvenanceCannotPartiallyApply) {
    auto s=selection();s[0]["quantity_exact"]="3";
    for(auto field:{"basis_provenance","basis_provenance_evidence","frame_before","source_digest"}) {
        auto in=input();split_prices(in);auto a=action();
        a[field]=std::string(field)=="basis_provenance_evidence"?"":"foreign";
        in["actions"].push_back(a);refused(decision(),s,in);
    }
    auto in=input();split_prices(in);auto a=action();a["ex_date"]="2026-09-26";in["actions"].push_back(a);refused(decision(),s,in);
    in=input();split_prices(in);in["actions"].push_back(action());in["actions"].push_back(action());refused(decision(),s,in);
}
TEST(QtEquityDeskCycle, UnsupportedActionDomainsAndClosedPhaseAreExplicitlyUnavailable) {
    auto s=selection();s[0]["quantity_exact"]="3";
    for(auto type:{"TERMINATION","SPINOFF","UNKNOWN"}) {
        auto in=input();split_prices(in);in["actions"].push_back(action(type));refused(decision(),s,in);
    }
    auto in=input(),a=action("DIVIDEND","2");a["raw_close_model_number"]="10";a["eligible_quantity_exact"]="0";
    in["actions"].push_back(a);refused(decision(),selection(),in);
    in=input();in["day_mode"]="closed";refused(decision(),selection(),in);
}
TEST(QtEquityDeskCycle, CanonicalExactQuantitiesAndClosedSchemaAreRequired) {
    for(auto q:{"0.5000000001","5e-1","+0.5","nan"}) {
        auto s=selection();s[0]["quantity_exact"]=q;refused(decision(),s,input());
    }
    auto in=input();in["calculation_version"]="foreign";refused(decision(),selection(),in);
    in=input();in["silent_default"]=true;refused(decision(),selection(),in);
}

TEST(QtEquityDeskCycle, OptionalTracePreservesFinancialBytesAndRecordsOnlyActualCostReads) {
    auto d=decision(),s=selection(),in=input();std::vector<QtEquityCostTrace> trace;
    auto plain=produce_qt_equity_accounting(d,s,in);
    auto observed=produce_qt_equity_accounting(d,s,in,&trace);
    ASSERT_TRUE(plain.is_ok());ASSERT_TRUE(observed.is_ok());EXPECT_EQ(plain.value(),observed.value());
    ASSERT_EQ(trace.size(),1U);const auto& read=trace[0];
    EXPECT_EQ(read.key.portfolio_id,"BOOK");EXPECT_EQ(read.key.strategy_name,"owner-a");
    EXPECT_DOUBLE_EQ(read.outcome.commissions_fees,0.14);
    EXPECT_EQ(read.observation.input_source,transaction_cost::CostInputSource::explicit_values);
    EXPECT_EQ(read.observation.asset_lookup.path,transaction_cost::AssetLookupPath::exact_symbol);
    EXPECT_EQ(read.observation.quantity,-1);EXPECT_EQ(read.observation.reference_price,14);
    EXPECT_EQ(read.observation.commission_per_unit,0.005);
    EXPECT_EQ(read.observation.min_commission_per_order,1);
    EXPECT_EQ(read.observation.max_commission_pct,0.01);
    EXPECT_FALSE(read.observation.max_commission_per_order.has_value()); // actual percentage branch
    EXPECT_FALSE(read.observation.explicit_fee_per_contract.has_value()); // no fee fallback read
    EXPECT_FALSE(read.observation.sec_fee_per_million.has_value()); // disabled regulation is unread
    EXPECT_EQ(read.observation.apply_regulatory_fees,false);
    EXPECT_EQ(read.observation.spread.tick_constrained,false);
    EXPECT_EQ(read.observation.max_total_implicit_bps,0);
}

TEST(QtEquityDeskCycle, TraceIsEmptyOnQuietSuccessAndUnchangedOnRefusal) {
    QtEquityCostTrace sentinel;sentinel.key.strategy_name="caller-sentinel";
    std::vector<QtEquityCostTrace> trace{sentinel};auto s=selection();s[0]["quantity_exact"]="1.5";
    auto quiet=produce_qt_equity_accounting(decision(),s,input(),&trace);
    ASSERT_TRUE(quiet.is_ok());EXPECT_TRUE(trace.empty());
    trace={sentinel};auto bad=input();bad["instruments"][0]["cost_parameters"].erase("commission_per_unit");
    auto no=produce_qt_equity_accounting(decision(),selection(),bad,&trace);
    EXPECT_TRUE(no.is_error());ASSERT_EQ(trace.size(),1U);EXPECT_EQ(trace[0].key.strategy_name,"caller-sentinel");
    // Owner-a can complete its cost call before owner-z's mark overflows.
    // Neither a partial financial successor nor that partial trace may escape.
    bad=input();auto owners=selection();auto z=owners[0];z["key"]=key("owner-z");
    z["key"]["symbol"]="ZBIG";z["quantity_exact"]="20";z["average_price_exact"]=nullptr;owners.push_back(z);
    auto market=bad["instruments"][0];market["symbol"]="ZBIG";market["mark"]["price_model_number"]="10000000000";
    bad["instruments"].push_back(market);const auto frozen=J::array({owners,bad});
    auto partial=produce_qt_equity_accounting(decision(),owners,bad,&trace);
    EXPECT_TRUE(partial.is_error());ASSERT_EQ(trace.size(),1U);EXPECT_EQ(trace[0].key.strategy_name,"caller-sentinel");
    EXPECT_EQ(J::array({owners,bad}),frozen);
}

TEST(QtEquityDeskCycle, NoneditableSplitPreservesChosenQuantityAndRestatesActualBasisWithoutExecution) {
    auto d=decision(),s=selection(),in=input();noneditable_action(s,in,false);
    const auto before=J::array({d,s,in});std::vector<QtEquityCostTrace> trace;
    auto r=produce_qt_equity_accounting(d,s,in,&trace);ASSERT_TRUE(r.is_ok());const auto& o=r.value();
    EXPECT_EQ(J::array({d,s,in}),before);EXPECT_TRUE(trace.empty());EXPECT_TRUE(o.at("executions").empty());
    const auto& f=o.at("observation").at("fills")[0];EXPECT_EQ(f.at("selected_quantity_exact"),"8");
    EXPECT_EQ(f.at("average_price_exact"),"12.5");EXPECT_EQ(f.at("actual_cash_cost_exact"),"0");
    EXPECT_EQ(f.at("observation_kind"),"carried");EXPECT_TRUE(f.at("execution_id").is_null());
    EXPECT_EQ(o.at("distance")[0].at("restated_previous_quantity_exact"),"8");
    EXPECT_EQ(o.at("distance")[0].at("execution_delta_exact"),"0");
    EXPECT_EQ(o.at("live_results")[0].at("daily_realized_pnl_exact"),"0");
    EXPECT_EQ(o.at("live_results")[0].at("daily_pnl_exact"),"0");
}

TEST(QtEquityDeskCycle, NoneditableDividendRestatesBasisOnlyWithoutCashIncomeOrCharge) {
    auto d=decision(),s=selection(),in=input();noneditable_action(s,in,true);
    const auto before=J::array({d,s,in});std::vector<QtEquityCostTrace> trace;
    auto r=produce_qt_equity_accounting(d,s,in,&trace);ASSERT_TRUE(r.is_ok());const auto& o=r.value();
    EXPECT_EQ(J::array({d,s,in}),before);EXPECT_TRUE(trace.empty());EXPECT_TRUE(o.at("executions").empty());
    const auto& f=o.at("observation").at("fills")[0];EXPECT_EQ(f.at("selected_quantity_exact"),"8");
    EXPECT_EQ(f.at("average_price_exact"),"24.75247525");EXPECT_EQ(f.at("actual_cash_cost_exact"),"0");
    EXPECT_EQ(f.at("observation_kind"),"carried");EXPECT_TRUE(f.at("execution_id").is_null());
    EXPECT_EQ(o.at("distance")[0].at("execution_delta_exact"),"0");
    EXPECT_EQ(o.at("live_results")[0].at("daily_realized_pnl_exact"),"0");
    EXPECT_EQ(o.at("live_results")[0].at("daily_pnl_exact"),"0");
}

TEST(QtEquityDeskCycle, NoneditableActionNeverAcceptsArbitraryAdvisoryBasisOrResizesChoice) {
    for(bool dividend:{false,true}){
        auto d=decision(),s=selection(),in=input();noneditable_action(s,in,dividend);
        s[0]["average_price_exact"]="999";std::vector<QtEquityCostTrace> trace(1);
        trace[0].key.strategy_name="sentinel";const auto frozen=J::array({d,s,in});
        auto r=produce_qt_equity_accounting(d,s,in,&trace);EXPECT_TRUE(r.is_error());
        EXPECT_EQ(J::array({d,s,in}),frozen);ASSERT_EQ(trace.size(),1U);EXPECT_EQ(trace[0].key.strategy_name,"sentinel");
    }
    auto s=selection(),in=input();noneditable_action(s,in,false);
    in["previous_positions"][0]["quantity_exact"]="8";refused(decision(),s,in);
}

TEST(QtEquityDeskCycle, NoneditableActionFrameAndFormationMustCloseBeforeBasisRestatement) {
    for(bool dividend:{false,true})for(auto damage:{"frame","formation","provenance"}){
        auto s=selection(),in=input();noneditable_action(s,in,dividend);
        if(std::string(damage)=="frame")in["actions"][0]["frame_before"]="foreign";
        else if(std::string(damage)=="formation")in["previous_positions"][0]["basis_evidence"]["formed_day"]=day;
        else in["actions"][0]["basis_provenance"]="unsupported";
        refused(decision(),s,in);
    }
}

TEST(QtEquityDeskCycle, GovernedCurrentDaySplitUsesOriginalPriorExactlyOnce) {
    auto s=selection(),in=input();s[0]["quantity_exact"]="3";split_prices(in);
    auto a=action();a["ex_date"]=day;in["actions"].push_back(a);
    const auto before=J::array({s,in});auto result=produce_qt_equity_accounting(decision(),s,in);
    ASSERT_TRUE(result.is_ok());const auto& o=result.value();
    EXPECT_TRUE(o.at("executions").empty());
    EXPECT_EQ(o.at("distance")[0].at("restated_previous_quantity_exact"),"3");
    EXPECT_EQ(o.at("observation").at("fills")[0].at("average_price_exact"),"5");
    EXPECT_EQ(o.at("live_results")[0].at("daily_pnl_exact"),"0");
    ASSERT_EQ(o.at("corporate_action_adjustments").size(),1U);
    EXPECT_EQ(o.at("corporate_action_adjustments")[0].at("event_date"),day);
    EXPECT_EQ(J::array({s,in}),before);
}
TEST(QtEquityDeskCycle, GovernedCurrentDayDividendUsesExistingBasisOnlyPolicy) {
    auto s=selection(),in=input();s[0]["quantity_exact"]="1.5";
    in["previous_positions"][0]["average_price_exact"]="12";
    in["previous_positions"][0]["daily_unrealized_pnl_exact"]="3";anchors(in,"3","23","1023");
    auto a=action("DIVIDEND","2");a["ex_date"]=day;
    a["raw_close_model_number"]="10";a["eligible_quantity_exact"]="1.5";
    in["actions"].push_back(a);
    for(auto p:{"reference","mark"}) {
        in["instruments"][0][p]["price_frame_id"]="adjusted-2";
        in["instruments"][0][p]["price_model_number"]="11";
    }
    const auto before=in;auto result=produce_qt_equity_accounting(decision(),s,in);
    ASSERT_TRUE(result.is_ok());const auto& o=result.value();
    EXPECT_TRUE(o.at("executions").empty());
    EXPECT_EQ(o.at("observation").at("fills")[0].at("average_price_exact"),"10");
    EXPECT_EQ(o.at("live_results")[0].at("daily_realized_pnl_exact"),"0");
    EXPECT_EQ(o.at("live_results")[0].at("daily_pnl_exact"),"-1.5");
    EXPECT_EQ(o.at("corporate_action_adjustments")[0].at("event_date"),day);
    EXPECT_EQ(in,before);
}
TEST(QtEquityDeskCycle, GovernedCurrentDayMixedMatchesExistingSequentialPolicy) {
    auto s=selection(),in=input();s[0]["quantity_exact"]="3";
    auto split=action();split["ex_date"]=day;
    auto dividend=action("DIVIDEND","1");dividend["ex_date"]=day;
    dividend["frame_before"]="adjusted-2";dividend["frame_after"]="adjusted-3";
    dividend["raw_close_model_number"]="10";dividend["eligible_quantity_exact"]="3";
    in["actions"]=J::array({split,dividend});
    for(auto p:{"reference","mark"}) {
        in["instruments"][0][p]["price_frame_id"]="adjusted-3";
        in["instruments"][0][p]["price_model_number"]="7";
    }
    auto historical=in;
    for(auto& a:historical["actions"])a["ex_date"]=prior_day;
    auto baseline=produce_qt_equity_accounting(decision(),s,historical);ASSERT_TRUE(baseline.is_ok());
    const auto frozen=in;auto current=produce_qt_equity_accounting(decision(),s,in);ASSERT_TRUE(current.is_ok());
    for(auto field:{"executions","distance","live_results","equity_curve","observation"})
        EXPECT_EQ(current.value().at(field),baseline.value().at(field));
    ASSERT_EQ(current.value().at("corporate_action_adjustments").size(),2U);
    for(const auto& a:current.value().at("corporate_action_adjustments"))EXPECT_EQ(a.at("event_date"),day);
    EXPECT_EQ(in,frozen);
}
TEST(QtEquityDeskCycle, CurrentDayStillRefusesUnprovedOrDuplicateAction) {
    auto s=selection();s[0]["quantity_exact"]="3";
    for(auto field:{"basis_provenance_evidence","source_digest","frame_before"}) {
        auto in=input();split_prices(in);auto a=action();a["ex_date"]=day;
        a[field]=std::string(field)=="basis_provenance_evidence"?"":"unproved";
        in["actions"].push_back(a);refused(decision(),s,in);
    }
    auto in=input();split_prices(in);auto a=action();a["ex_date"]=day;
    in["actions"]=J::array({a,a});refused(decision(),s,in);
}
TEST(QtEquityDeskCycle, CurrentDayStillRefusesFutureEventOrMissingAdjustedPrice) {
    auto s=selection();s[0]["quantity_exact"]="3";
    auto in=input();split_prices(in);auto a=action();a["ex_date"]="2026-09-26";
    in["actions"].push_back(a);refused(decision(),s,in);
    in=input();a=action();a["ex_date"]=day;in["actions"].push_back(a);
    refused(decision(),s,in);
    in=input();split_prices(in);in["instruments"][0]["reference"]["date"]=day;
    a=action();a["ex_date"]=day;in["actions"].push_back(a);refused(decision(),s,in);
}
