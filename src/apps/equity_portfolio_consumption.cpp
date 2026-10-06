#include "trade_ngin/apps/equity_portfolio_consumption.hpp"
#include <cmath>
#include <regex>
#include <stdexcept>
namespace trade_ngin { namespace {
using J=nlohmann::json;
std::string call(PortfolioCallOutcome v) {switch(v){
case PortfolioCallOutcome::NotCalled:return "not_reached";case PortfolioCallOutcome::InProgress:return "in_progress";
case PortfolioCallOutcome::ReturnedOk:return "returned_ok";case PortfolioCallOutcome::ReturnedError:return "returned_error";
case PortfolioCallOutcome::Threw:return "threw";}throw std::invalid_argument("invalid_call");}
std::string skip(PortfolioHelperSkip v) {switch(v){
case PortfolioHelperSkip::None:return "none";case PortfolioHelperSkip::NoEligibleSymbols:return "no_eligible_symbols";
case PortfolioHelperSkip::InsufficientHistory:return "insufficient_history";case PortfolioHelperSkip::AbsentOptimizer:return "absent_optimizer";
case PortfolioHelperSkip::AbsentRiskManager:return "absent_risk_manager";case PortfolioHelperSkip::NoPositions:return "no_positions";}
throw std::invalid_argument("invalid_skip");}
std::string manager(PortfolioRiskManagerSource v) {switch(v){case PortfolioRiskManagerSource::Absent:return "absent";
case PortfolioRiskManagerSource::Internal:return "internal";case PortfolioRiskManagerSource::External:return "external";}
throw std::invalid_argument("invalid_manager_source");}
void add(J& out,const char* key,const std::optional<double>& value){if(value){if(!std::isfinite(*value))throw std::invalid_argument("nonfinite_read");out[key]=*value;}}
void add(J& out,const char* key,const std::optional<bool>& value){if(value)out[key]=*value;}
J risk_reads(const RiskConfigConsumption& risk) {J j=J::object();
add(j,"var_limit",risk.var_limit);add(j,"jump_risk_limit",risk.jump_risk_limit);add(j,"max_correlation",risk.max_correlation);
add(j,"max_gross_leverage",risk.max_gross_leverage);add(j,"max_net_leverage",risk.max_net_leverage);
add(j,"confidence_level",risk.confidence_level);if(risk.capital)j["capital_exact"]=risk.capital->to_string();return j;}
J cost_reads(const transaction_cost::CostChargeObservation& c) {J j=J::object();
add(j,"quantity",c.quantity);
add(j,"reference_price",c.reference_price);
add(j,"adv_argument",c.adv_argument);
add(j,"volatility_multiplier_argument",c.volatility_multiplier_argument);
add(j,"retrieved_adv",c.retrieved_adv);
add(j,"retrieved_volatility_multiplier",c.retrieved_volatility_multiplier);
add(j,"effective_adv",c.effective_adv);
add(j,"effective_volatility_multiplier",c.effective_volatility_multiplier);
add(j,"explicit_fee_per_contract",c.explicit_fee_per_contract);
add(j,"commission_per_unit",c.commission_per_unit);
add(j,"min_commission_per_order",c.min_commission_per_order);
add(j,"max_commission_per_order",c.max_commission_per_order);
add(j,"max_commission_pct",c.max_commission_pct);
add(j,"sec_fee_per_million",c.sec_fee_per_million);
add(j,"finra_taf_per_share",c.finra_taf_per_share);
add(j,"finra_taf_cap_per_trade",c.finra_taf_cap_per_trade);
add(j,"max_total_implicit_bps",c.max_total_implicit_bps);
add(j,"point_value",c.point_value);
add(j,"volatility_calculation_reached",c.volatility.calculation_reached);
add(j,"volatility_lambda",c.volatility.lambda);
add(j,"volatility_min_multiplier",c.volatility.min_multiplier);
add(j,"volatility_max_multiplier",c.volatility.max_multiplier);
add(j,"baseline_spread_ticks",c.spread.baseline_spread_ticks);
add(j,"min_spread_ticks",c.spread.min_spread_ticks);
add(j,"max_spread_ticks",c.spread.max_spread_ticks);
add(j,"spread_cost_multiplier",c.spread.spread_cost_multiplier);
add(j,"tick_size",c.spread.tick_size);
add(j,"min_adv",c.impact.min_adv);
add(j,"min_participation",c.impact.min_participation);
add(j,"max_participation",c.impact.max_participation);
add(j,"max_impact_bps",c.impact.max_impact_bps);
add(j,"selected_k_bps",c.impact.selected_k_bps);
add(j,"apply_regulatory_fees",c.apply_regulatory_fees);
add(j,"tick_constrained",c.spread.tick_constrained);
if(c.input_source){switch(*c.input_source){case transaction_cost::CostInputSource::internally_tracked:j["input_source"]="internally_tracked";break;
case transaction_cost::CostInputSource::explicit_values:j["input_source"]="explicit_values";break;default:throw std::invalid_argument("invalid_cost_source");}}
if(c.asset_lookup.path){switch(*c.asset_lookup.path){case transaction_cost::AssetLookupPath::exact_symbol:j["asset_lookup_path"]="exact_symbol";break;
case transaction_cost::AssetLookupPath::pre_dot_root:j["asset_lookup_path"]="pre_dot_root";break;
case transaction_cost::AssetLookupPath::fallback:j["asset_lookup_path"]="fallback";break;default:throw std::invalid_argument("invalid_cost_lookup");}}
return j;}
bool entered(PortfolioCallOutcome value){return value!=PortfolioCallOutcome::NotCalled;}
}
nlohmann::json project_equity_portfolio_consumption(const PortfolioConsumptionTrace& trace) {
J doc={{"schema_version","qt-equity-portfolio-consumption/v1"},{"scope","portfolio_invocation"},{"full_run_certification",false},
    {"available",false},{"unavailable_reason","instrumentation_missing"},{"outcome","not_reached"},
    {"skip_execution_generation",trace.skip_execution_generation?J(*trace.skip_execution_generation):J(nullptr)},
    {"passes",J::array()},{"strategy_charges",J::array()},{"compatibility_charges",J::array()}};
try {
    doc["outcome"]=call(trace.outcome);
    if(trace.pass_count>5 || trace.strategy_charges.size()>4096 || trace.compatibility_charges.size()>4096)
        throw std::length_error("capacity_exceeded");
    bool complete=trace.outcome==PortfolioCallOutcome::ReturnedOk && trace.skip_execution_generation.has_value() && trace.pass_count>0;
    bool unsupported_optimizer=false;
    for(std::size_t index=0;index<trace.pass_count;++index){const auto& pass=trace.passes[index];
        J reads=J::object();add(reads,"use_optimization",pass.use_optimization);add(reads,"use_risk_management",pass.use_risk_management);
        const auto& r=pass.risk;J risk={{"call",call(r.risk_call)},{"skip",skip(r.skip)},{"reads",risk_reads(r.risk)}};
        if(r.source)risk["manager_source"]=manager(*r.source);
        if(r.lookback_period){if(*r.lookback_period<0)throw std::invalid_argument("negative_lookback");risk["lookback_period"]=*r.lookback_period;}
        if(!pass.use_optimization || !pass.use_risk_management)complete=false;
        if(pass.use_optimization==false && entered(pass.optimization_helper))throw std::invalid_argument("disabled_optimizer_called");
        if(pass.use_optimization==true)unsupported_optimizer=true;
        if(pass.use_risk_management==false && (entered(pass.risk_helper) || entered(r.risk_call) ||
            r.skip!=PortfolioHelperSkip::None || r.source || r.lookback_period || !risk.at("reads").empty()))
            throw std::invalid_argument("disabled_risk_has_reads");
        if(pass.use_risk_management==true){
            if(pass.risk_helper!=PortfolioCallOutcome::ReturnedOk)complete=false;
            if(entered(r.risk_call)){
                if(r.skip!=PortfolioHelperSkip::None || !r.source || r.source==PortfolioRiskManagerSource::Absent)
                    throw std::invalid_argument("called_risk_has_skip");
                if(r.risk_call!=PortfolioCallOutcome::ReturnedOk)complete=false;
            }else{
                if(!risk.at("reads").empty())throw std::invalid_argument("uncalled_risk_has_reads");
                if(r.skip==PortfolioHelperSkip::None)complete=false;
                // These are the only skip branches in apply_risk_management.
                if(r.skip!=PortfolioHelperSkip::None && r.skip!=PortfolioHelperSkip::AbsentRiskManager &&
                   r.skip!=PortfolioHelperSkip::NoPositions)throw std::invalid_argument("invalid_risk_skip");
                if(r.skip==PortfolioHelperSkip::AbsentRiskManager && (r.source!=PortfolioRiskManagerSource::Absent || r.lookback_period))
                    throw std::invalid_argument("absent_risk_source_mismatch");
                if(r.skip==PortfolioHelperSkip::NoPositions && (!r.source || r.source==PortfolioRiskManagerSource::Absent || !r.lookback_period))
                    throw std::invalid_argument("positions_skip_source_mismatch");
            }
        }
        doc["passes"].push_back({{"index",index},{"reads",reads},{"optimization_helper",call(pass.optimization_helper)},
            {"risk_helper",call(pass.risk_helper)},{"risk",risk}});
    }
    auto charges=[&](const auto& vector,const char* key,PortfolioChargePurpose purpose){
        for(std::size_t index=0;index<vector.size();++index){const auto& c=vector[index];
            if(c.purpose!=purpose || c.symbol.empty() || c.symbol.size()>64 ||
                !std::regex_match(c.symbol,std::regex("[A-Za-z0-9_./-]+")) ||
                (purpose==PortfolioChargePurpose::PerStrategy?c.strategy_id!="LIVE_EQUITY_MEAN_REVERSION":!c.strategy_id.empty()))
                throw std::invalid_argument("invalid_internal_charge_identity");
            const auto values=cost_reads(c.charge);
            if(trace.skip_execution_generation==true)throw std::invalid_argument("skipped_execution_has_charges");
            if(c.charge_call==PortfolioCallOutcome::NotCalled && !values.empty())throw std::invalid_argument("uncalled_charge_has_reads");
            if(c.charge_call!=PortfolioCallOutcome::ReturnedOk)complete=false;
            if(c.charge_call==PortfolioCallOutcome::ReturnedOk)for(const auto* required:{"quantity","reference_price","input_source","asset_lookup_path"})
                if(!values.contains(required))complete=false;
            doc[key].push_back({{"index",index},{"purpose",purpose==PortfolioChargePurpose::PerStrategy?"per_strategy":"compatibility"},
                {"strategy_id",c.strategy_id},{"symbol",c.symbol},{"call",call(c.charge_call)},{"reads",values}});
        }
    };
    charges(trace.strategy_charges,"strategy_charges",PortfolioChargePurpose::PerStrategy);
    charges(trace.compatibility_charges,"compatibility_charges",PortfolioChargePurpose::Compatibility);
    if(unsupported_optimizer){doc["unavailable_reason"]="unsupported_enabled_helper";return doc;}
    doc["available"]=complete;doc["unavailable_reason"]=complete?J(nullptr):J("stage_failed");
}catch(const std::length_error&){doc["unavailable_reason"]="capacity_exceeded";}
catch(const std::exception&){doc["unavailable_reason"]="invalid_observed_value";}
return doc;
}
}
