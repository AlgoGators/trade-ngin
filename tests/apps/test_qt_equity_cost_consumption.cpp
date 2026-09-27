#include <gtest/gtest.h>
#include "trade_ngin/apps/qt_equity_desk_cycle.hpp"

#include "trade_ngin/apps/qt_equity_cost_consumption.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/git_version.hpp"
#include <algorithm>
#include <limits>
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

std::string hash(const J& value) {
    auto bytes=canonical_qt_desk_input_json(value); EXPECT_TRUE(bytes.is_ok());
    if(bytes.is_error())return {};
    auto digest=qt_sha256_hex(bytes.value());EXPECT_TRUE(digest.is_ok());
    return digest.is_ok()?digest.value():std::string{};
}
const J* read(const J& charge,const char* field) {
    for(const auto& row:charge.at("reads"))if(row.at("field")==field)return &row;
    return nullptr;
}
struct Actual {
    J d=decision(), in=input(), selected=selection(), output;
    std::vector<QtEquityCostTrace> trace;
    void produce() {
        auto result=produce_qt_equity_accounting(d,selected,in,&trace);
        ASSERT_TRUE(result.is_ok());output=result.value();
    }
    Result<J> project() const {return project_qt_equity_cost_consumption(d,in,output,&trace);}
};
}

TEST(QtEquityCostConsumption, ActualChargeEmitsClosedScopedEvidenceAndCompiledIdentity) {
    Actual a;a.produce();ASSERT_EQ(a.trace.size(),1u);
    const auto before=J::array({a.d,a.in,a.output});
    auto result=a.project();ASSERT_TRUE(result.is_ok());const auto& out=result.value();
    EXPECT_EQ(out.size(),6u);
    EXPECT_EQ(out.at("schema_version"),"qt-equity-cost-consumption/v1");
    EXPECT_EQ(out.at("profile"),"qt_equity_accounting_costs");
    EXPECT_EQ(out.at("authority"),"inspection_only");
    EXPECT_EQ(out.at("coverage"),(J{{"scope","executed_equity_cost_calls"},{"status","complete"}}));
    const auto& identity=out.at("identity");EXPECT_EQ(identity.size(),7u);
    EXPECT_EQ(identity.at("decision_id"),a.d.at("decision_id"));
    EXPECT_EQ(identity.at("accounting_input_id"),a.in.at("accounting_input_id"));
    EXPECT_EQ(identity.at("book_id"),"BOOK");EXPECT_EQ(identity.at("source_day"),day);
    EXPECT_EQ(identity.at("input_digest"),hash(a.in));
    EXPECT_EQ(identity.at("financial_output_digest"),hash(a.output));
    EXPECT_EQ(identity.at("producer_version"),TRADE_NGIN_GIT_SHA);
    ASSERT_EQ(out.at("charges").size(),1u);const auto& charge=out.at("charges").at(0);
    EXPECT_EQ(charge.size(),4u);EXPECT_EQ(charge.at("key"),key());
    EXPECT_EQ(charge.at("outcome"),"returned_ok");
    EXPECT_EQ(charge.at("meta"),(J{{"input_source","explicit_values"},{"asset_lookup","exact_symbol"}}));
    ASSERT_NE(read(charge,"cost.charge.commission_per_unit"),nullptr);
    EXPECT_EQ(read(charge,"cost.charge.commission_per_unit")->at("value"),0.005);
    for(const auto* field:{"cost.charge.apply_regulatory_fees","cost.spread.tick_constrained"}) {
        ASSERT_NE(read(charge,field),nullptr);EXPECT_EQ(read(charge,field)->at("value"),false);
        EXPECT_EQ(read(charge,field)->at("value_type"),"bool");
        EXPECT_EQ(read(charge,field)->at("origin"),"runtime_effective");
    }
    EXPECT_EQ(read(charge,"cost.charge.explicit_fee_per_contract"),nullptr);
    EXPECT_EQ(read(charge,"cost.charge.sec_fee_per_million"),nullptr);
    EXPECT_EQ(read(charge,"cost.volatility.lambda"),nullptr);
    EXPECT_EQ(J::array({a.d,a.in,a.output}),before);
    RecordProperty("actual_scoped_equity_consumption",out.dump());
}

TEST(QtEquityCostConsumption, OppositeOwnersInSameSymbolRemainDistinctAndTraceOrderDoesNotMatter) {
    Actual a;auto prior=a.in["previous_positions"][0];prior["key"]=key("owner-b",prior_day);
    a.in["previous_positions"].push_back(prior);
    a.in["previous_totals"][0]["total_unrealized_pnl_exact"]="12";
    a.in["previous_totals"][0]["total_pnl_exact"]="32";
    a.in["previous_totals"][0]["equity_exact"]="1032";
    auto selected=a.selected[0];selected["key"]=key("owner-b");selected["quantity_exact"]="2.5";
    a.selected.push_back(selected);a.produce();ASSERT_EQ(a.trace.size(),2u);
    EXPECT_LT(*a.trace[0].observation.quantity,0);EXPECT_GT(*a.trace[1].observation.quantity,0);
    auto first=a.project();ASSERT_TRUE(first.is_ok());
    ASSERT_EQ(first.value().at("charges").size(),2u);
    EXPECT_EQ(first.value().at("charges")[0].at("key"),key());
    EXPECT_EQ(first.value().at("charges")[1].at("key"),key("owner-b"));
    std::reverse(a.trace.begin(),a.trace.end());auto reversed=a.project();ASSERT_TRUE(reversed.is_ok());
    EXPECT_EQ(first.value().dump(),reversed.value().dump());
}

TEST(QtEquityCostConsumption, QuietCarryRequiresPresentEmptyTraceAndHasNoInventedCalls) {
    Actual a;a.selected[0]["quantity_exact"]="1.5";a.produce();
    ASSERT_TRUE(a.trace.empty());ASSERT_TRUE(a.output.at("executions").empty());
    auto result=a.project();ASSERT_TRUE(result.is_ok());EXPECT_TRUE(result.value().at("charges").empty());
    EXPECT_EQ(result.value().at("coverage").at("status"),"complete");
    EXPECT_TRUE(project_qt_equity_cost_consumption(a.d,a.in,a.output,nullptr).is_error());
}

TEST(QtEquityCostConsumption, FinancialDigestExcludesOnlyOptionalNestedConsumption) {
    Actual a;a.produce();auto first=a.project();ASSERT_TRUE(first.is_ok());
    a.output["consumption"]=first.value();auto attached=a.project();ASSERT_TRUE(attached.is_ok());
    EXPECT_EQ(first.value(),attached.value());
    a.output["consumption"]={{"ignored_for_financial_digest",true}};
    auto changed_child=a.project();ASSERT_TRUE(changed_child.is_ok());EXPECT_EQ(first.value(),changed_child.value());
    a.output["live_results"][0]["current_portfolio_value_exact"]="999";
    // Every financial byte outside the child remains bound, even where a pure
    // projector leaves accounting recalculation to the admitted storage proof.
    auto changed_financial=a.project();
    if(changed_financial.is_ok())EXPECT_NE(changed_financial.value().at("identity").at("financial_output_digest"),first.value().at("identity").at("financial_output_digest"));
}

TEST(QtEquityCostConsumption, MissingOrDuplicateOrForeignOwnerTraceRefuses) {
    Actual a;a.produce();
    EXPECT_TRUE(project_qt_equity_cost_consumption(a.d,a.in,a.output,nullptr).is_error());
    const auto original=a.trace;a.trace.clear();EXPECT_TRUE(a.project().is_error());
    a.trace=original;a.trace.push_back(original[0]);EXPECT_TRUE(a.project().is_error());
    a.trace=original;a.trace[0].key.strategy_name="foreign-owner";EXPECT_TRUE(a.project().is_error());
}

TEST(QtEquityCostConsumption, ZeroOrWrongSignedQuantityCannotClaimExecutedCostCall) {
    Actual a;a.produce();a.trace[0].observation.quantity=0;EXPECT_TRUE(a.project().is_error());
    a.produce();a.trace[0].observation.quantity=1;EXPECT_TRUE(a.project().is_error());
    a.produce();a.output["executions"][0]["quantity_exact"]="2";EXPECT_TRUE(a.project().is_error());
}

TEST(QtEquityCostConsumption, ActualOutcomeAndReferenceOperandsMustBindExecution) {
    Actual a;a.produce();a.trace[0].outcome.total_transaction_costs+=1;EXPECT_TRUE(a.project().is_error());
    a.produce();a.trace[0].observation.reference_price=99;EXPECT_TRUE(a.project().is_error());
    a.produce();a.output["executions"][0]["commissions_fees_exact"]="999";EXPECT_TRUE(a.project().is_error());
}

TEST(QtEquityCostConsumption, TraceReadsMustMatchAdmittedParametersAndActualBranch) {
    Actual a;a.produce();a.trace[0].observation.commission_per_unit=99;EXPECT_TRUE(a.project().is_error());
    a.produce();a.trace[0].observation.commission_per_unit.reset();EXPECT_TRUE(a.project().is_error());
    a.produce();a.trace[0].observation.sec_fee_per_million=20.6;EXPECT_TRUE(a.project().is_error());
    a.produce();a.trace[0].observation.volatility.lambda=0.94;EXPECT_TRUE(a.project().is_error());
    a.produce();a.trace[0].observation.asset_lookup.path=transaction_cost::AssetLookupPath::fallback;
    EXPECT_TRUE(a.project().is_error());
}

TEST(QtEquityCostConsumption, NonfiniteReadAndOperandRefuseInsteadOfSerializingNull) {
    Actual a;a.produce();a.trace[0].observation.point_value=std::numeric_limits<double>::infinity();
    EXPECT_TRUE(a.project().is_error());a.produce();
    a.trace[0].observation.adv_argument=std::numeric_limits<double>::quiet_NaN();EXPECT_TRUE(a.project().is_error());
}

TEST(QtEquityCostConsumption, DecisionInputOutputScopesMustAgree) {
    Actual a;a.produce();a.d["book_id"]="OTHER";EXPECT_TRUE(a.project().is_error());
    a.d=decision();a.in["accounting_input_id"]="20000000-0000-0000-0000-000000000002";
    EXPECT_TRUE(a.project().is_error());a.in=input();
    a.output["observation"]["source_day"]=prior_day;EXPECT_TRUE(a.project().is_error());
}

TEST(QtEquityCostConsumption, FailedProducerHasNoNewTraceOrSuccessfulConsumptionEvidence) {
    Actual a;a.produce();const auto before=a.trace[0].key;
    a.in["instruments"][0]["cost_evidence"]["adv_model_number"]="0";
    auto failed=produce_qt_equity_accounting(a.d,a.selected,a.in,&a.trace);ASSERT_TRUE(failed.is_error());
    ASSERT_EQ(a.trace.size(),1u);EXPECT_EQ(a.trace[0].key,before);
    EXPECT_TRUE(project_qt_equity_cost_consumption(a.d,a.in,J(nullptr),nullptr).is_error());
    EXPECT_TRUE(a.project().is_error());
}

TEST(QtEquityCostConsumption, ActualSellFixedMaximumAndRegulatoryReadsAreRetained) {
    Actual a;auto& config=a.in["instruments"][0]["cost_parameters"];
    config["apply_regulatory_fees"]=true;config["tick_constrained"]=true;
    config["max_commission_pct"]="-1";a.produce();
    auto result=a.project();ASSERT_TRUE(result.is_ok());const auto& charge=result.value().at("charges").at(0);
    for(const auto* field:{"cost.charge.max_commission_per_order","cost.charge.sec_fee_per_million",
                          "cost.charge.finra_taf_per_share","cost.charge.finra_taf_cap_per_trade"})
        ASSERT_NE(read(charge,field),nullptr);
    EXPECT_EQ(read(charge,"cost.charge.sec_fee_per_million")->at("value"),20.6);
    EXPECT_EQ(read(charge,"cost.charge.max_commission_pct")->at("value"),-1);
    EXPECT_EQ(read(charge,"cost.charge.apply_regulatory_fees")->at("value"),true);
    EXPECT_EQ(read(charge,"cost.spread.tick_constrained")->at("value"),true);
}