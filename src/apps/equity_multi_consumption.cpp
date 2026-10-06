#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/apps/equity_multi_consumption.hpp"
#include "trade_ngin/apps/equity_portfolio_consumption.hpp"
#include "trade_ngin/apps/equity_strategy_consumption.hpp"
#include "trade_ngin/data/live_config_owners.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include <set>
#include <cmath>
#include <stdexcept>
namespace trade_ngin { namespace {
using J=nlohmann::json;
void need(bool value){if(!value)throw std::invalid_argument("equity_multi_consumption_invalid");}
bool shape(const J& value,std::initializer_list<const char*> keys){
    if(!value.is_object() || value.size()!=keys.size())return false;
    for(auto key:keys)if(!value.contains(key))return false;
    return true;
}
bool number(const J& value) {
    return value.is_number() && std::isfinite(value.get<double>());
}
bool integer(const J& value,int64_t low,int64_t high) {
    if(!value.is_number_integer())return false;
    if(value.is_number_unsigned())return value.get<uint64_t>()<=static_cast<uint64_t>(high);
    const auto v=value.get<int64_t>();return v>=low && v<=high;
}
bool identity(const J& value) {
    if(!value.is_string())return false;
    const auto& s=value.get_ref<const std::string&>();
    return !s.empty() && s.size()<=64 && s.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./-")==std::string::npos;
}
bool one_of(const J& value,std::initializer_list<const char*> values) {
    if(!value.is_string())return false;
    for(const auto* allowed:values)if(value==allowed)return true;
    return false;
}
void risk_observation(const J& pass) {
    const auto& risk=pass.at("risk");
    need(risk.is_object() && risk.contains("call") && risk.contains("skip") && risk.contains("reads"));
    for(const auto& [key,value]:risk.items()) {
        (void)value;need(key=="call" || key=="skip" || key=="reads" || key=="manager_source" || key=="lookback_period");
    }
    need(one_of(risk.at("call"),{"not_reached","returned_ok"}) &&
        one_of(risk.at("skip"),{"none","absent_risk_manager","no_positions"}));
    if(risk.contains("manager_source"))need(one_of(risk.at("manager_source"),{"absent","internal","external"}));
    if(risk.contains("lookback_period"))need(integer(risk.at("lookback_period"),0,INT32_MAX));
    const auto& reads=risk.at("reads");need(reads.is_object());
    for(const auto& [key,value]:reads.items()) {
        if(key=="capital_exact")need(value.is_string() && parse_qt_quantity_exact(value.get<std::string>()).is_ok());
        else {
            need(key=="var_limit" || key=="jump_risk_limit" || key=="max_correlation" ||
                key=="max_gross_leverage" || key=="max_net_leverage" || key=="confidence_level");
            need(number(value));
        }
    }
    if(pass.at("reads").at("use_risk_management")==false) {
        need(pass.at("risk_helper")=="not_reached" &&
            risk==J{{"call","not_reached"},{"skip","none"},{"reads",J::object()}});
    }else {
        need(pass.at("risk_helper")=="returned_ok");
        if(risk.at("call")=="not_reached") {
            need(reads.empty());
            if(risk.at("skip")=="absent_risk_manager")
                need(risk.at("manager_source")=="absent" && !risk.contains("lookback_period"));
            else need(risk.at("skip")=="no_positions" &&
                one_of(risk.at("manager_source"),{"internal","external"}) && risk.contains("lookback_period"));
        }else need(risk.at("skip")=="none" && one_of(risk.at("manager_source"),{"internal","external"}));
    }
}
void charge_reads(const J& reads) {
    static const std::set<std::string> numbers={"quantity","reference_price","adv_argument",
        "volatility_multiplier_argument","retrieved_adv","retrieved_volatility_multiplier","effective_adv",
        "effective_volatility_multiplier","explicit_fee_per_contract","commission_per_unit","min_commission_per_order",
        "max_commission_per_order","max_commission_pct","sec_fee_per_million","finra_taf_per_share",
        "finra_taf_cap_per_trade","max_total_implicit_bps","point_value","volatility_lambda","volatility_min_multiplier",
        "volatility_max_multiplier","baseline_spread_ticks","min_spread_ticks","max_spread_ticks","spread_cost_multiplier",
        "tick_size","min_adv","min_participation","max_participation","max_impact_bps","selected_k_bps"};
    need(reads.is_object());
    for(const auto* key:{"quantity","reference_price","input_source","asset_lookup_path"})need(reads.contains(key));
    for(const auto& [key,value]:reads.items()) {
        if(numbers.contains(key))need(number(value));
        else if(key=="apply_regulatory_fees" || key=="tick_constrained" || key=="volatility_calculation_reached")need(value.is_boolean());
        else if(key=="input_source")need(one_of(value,{"internally_tracked","explicit_values"}));
        else if(key=="asset_lookup_path")need(one_of(value,{"exact_symbol","pre_dot_root","fallback"}));
        else need(false);
    }
}
void strategy_observation(const J& row) {
    need(shape(row,{"reads","observed_state"}));
    const auto& reads=row.at("reads");const auto& state=row.at("observed_state");
    need(reads.is_object() && state.is_object());
    static const std::set<std::string> numbers={"entry_threshold","exit_threshold","stop_loss_pct","capital_allocation",
        "position_size","risk_target","fractional_min_price","fractional_min_adv","position_limit"};
    constexpr int64_t safe_integer=9007199254740991LL;
    for(const auto& [key,value]:reads.items()) {
        if(key=="lookback_period" || key=="vol_lookback")need(integer(value,INT32_MIN,INT32_MAX));
        else if(key=="maximum_price_history" || key=="maximum_volatility_history")need(integer(value,0,safe_integer));
        else if(key=="use_stop_loss" || key=="allow_fractional_shares" || key=="position_limit_present")need(value.is_boolean());
        else {need(numbers.contains(key));need(number(value));}
    }
    for(const auto& [key,value]:state.items()) {
        if(key=="volume_sample_count")need(integer(value,0,safe_integer));
        else if(key=="average_daily_volume")need(number(value));
        else {need(key=="fractional_eligible" || key=="short_allowed");need(value.is_boolean());}
    }
    if(reads.contains("position_limit_present"))need(reads.at("position_limit_present")==reads.contains("position_limit"));
    else need(!reads.contains("position_limit"));
    if(reads.contains("allow_fractional_shares") && reads.at("allow_fractional_shares")==false)need(!reads.contains("fractional_min_price"));
    if(reads.contains("use_stop_loss") && reads.at("use_stop_loss")==false)need(!reads.contains("stop_loss_pct"));
    const bool adv=reads.contains("fractional_min_adv");
    need(adv==state.contains("volume_sample_count") && adv==state.contains("average_daily_volume"));
}

}
bool validate_equity_multi_consumption(const J& value,const J& snapshot,const std::string& engine,
    const std::string& book,const std::string& date,const std::string& hash) {
    try {
        need(shape(value,{"schema_version","profile","scope","full_run_certification","run_key",
            "effective_sha256","source_to_storage_owners","coverage","portfolio_invocation","strategy_invocations"}));
        need(value.at("schema_version")=="live-equity-multi-consumption/v1" &&
            value.at("profile")=="live_equity_multi_sleeve" && value.at("scope")=="primary_invocation" &&
            value.at("full_run_certification")==false &&
            value.at("run_key")==J{{"portfolio_id",book},{"strategy_id",engine},{"date",date}} &&
            hash.size()==64 && hash.find_first_not_of("0123456789abcdef")==std::string::npos &&
            value.at("effective_sha256")==hash && value.dump().size()<=2u*1024u*1024u);
        const auto selected=apps::collect_enabled_equity_strategies(snapshot.at("strategies"),"enabled_live");
        need(selected.is_ok()); const auto plan=apps::build_equity_live_book_plan(selected.value());
        need(plan.is_ok() && !plan.value().legacy_single && plan.value().combined_strategy_id==engine);
        const auto owners=live_config_source_owner_map(snapshot,engine);
        need(value.at("source_to_storage_owners")==owners);
        const auto& coverage=value.at("coverage");
        need(shape(coverage,{"primary","legacy_run_stages","account_execution_costs"}) &&
            coverage.at("legacy_run_stages")=="not_collected" && coverage.at("account_execution_costs")=="not_collected");
        const auto& strategies=value.at("strategy_invocations");need(strategies.is_object());
        if(coverage.at("primary")=="skipped_non_trading_day") {
            need(value.at("portfolio_invocation").is_null() && strategies.empty());return true;
        }
        need(coverage.at("primary")=="observed" && strategies.size()==owners.size());
        const auto& portfolio=value.at("portfolio_invocation");
        need(shape(portfolio,{"schema_version","scope","full_run_certification","available","unavailable_reason",
            "outcome","skip_execution_generation","passes","strategy_charges","compatibility_charges"}) && portfolio.at("schema_version")=="qt-equity-portfolio-consumption/v1" &&
            portfolio.at("scope")=="portfolio_invocation" && portfolio.at("full_run_certification")==false &&
            portfolio.at("available")==true && portfolio.at("outcome")=="returned_ok" &&
            portfolio.at("unavailable_reason").is_null() && portfolio.at("skip_execution_generation").is_boolean() &&
            portfolio.at("passes").is_array() && !portfolio.at("passes").empty() && portfolio.at("passes").size()<=5);
        for(size_t i=0;i<portfolio.at("passes").size();++i) {
            const auto& pass=portfolio.at("passes").at(i);
            need(shape(pass,{"index","reads","optimization_helper","risk_helper","risk"}) && integer(pass.at("index"),0,4) && pass.at("index")==i);
            need(shape(pass.at("reads"),{"use_optimization","use_risk_management"}) &&
                pass.at("reads").at("use_optimization")==false && pass.at("reads").at("use_risk_management").is_boolean());
            need(pass.at("optimization_helper")=="not_reached");
            risk_observation(pass);
        }
        std::set<std::string> owner_names;
        for(const auto& [source,owner]:owners.items()){(void)source;owner_names.insert(owner.get<std::string>());}
        for(const auto* key:{"strategy_charges","compatibility_charges"}) {
            const auto& charges=portfolio.at(key);need(charges.is_array() && charges.size()<=4096);
            need(portfolio.at("skip_execution_generation")==false || charges.empty());
            for(size_t index=0;index<charges.size();++index) {
                const auto& charge=charges.at(index);
                need(shape(charge,{"index","purpose","strategy_id","symbol","call","reads"}));
                need(integer(charge.at("index"),0,4095) && charge.at("index")==index && identity(charge.at("symbol")));
                need(charge.at("call")=="returned_ok");
                charge_reads(charge.at("reads"));
                need(std::string(key)=="strategy_charges" ?
                    charge.at("purpose")=="per_strategy" && owner_names.contains(charge.at("strategy_id").get<std::string>()) :
                    charge.at("purpose")=="compatibility" && charge.at("strategy_id")=="");
            }
        }
        for(const auto& [source,owner]:owners.items()) {
            (void)source;const auto& observation=strategies.at(owner.get<std::string>());
            need(shape(observation,{"schema_version","available","profile","scope","full_run_certification","symbols"}) &&
                observation.at("schema_version")=="qt-equity-strategy-consumption/v1" && observation.at("available")==true &&
                observation.at("profile")=="mean_reversion" && observation.at("scope")=="strategy_invocation" &&
                observation.at("full_run_certification")==false && observation.at("symbols").is_object());
            need(observation.at("symbols").size()<=256);
            for(const auto& [symbol,read]:observation.at("symbols").items()) {
                need(identity(symbol));strategy_observation(read);
            }
        }
        return true;
    }catch(...){return false;}
}
J project_equity_multi_consumption(const PortfolioConsumptionTrace& trace,bool non_trading,
    const J& snapshot,const std::string& engine,const std::string& book,const std::string& date,
    const std::string& hash) {
    J strategies=J::object(),portfolio=nullptr;
    if(non_trading) need(trace.outcome==PortfolioCallOutcome::NotCalled && trace.strategies.empty());
    else {
        std::set<std::string> owner_names;
        const auto owner_map=live_config_source_owner_map(snapshot,engine);
        for(const auto& [source,owner]:owner_map.items())
            {(void)source;owner_names.insert(owner.get<std::string>());}
        portfolio=project_equity_portfolio_consumption(trace,owner_names);
        for(const auto& invocation:trace.strategies) {
            need(invocation.outcome==PortfolioCallOutcome::ReturnedOk && !strategies.contains(invocation.strategy_id));
            auto observed=project_equity_strategy_consumption(invocation.strategy);need(observed.is_ok());
            strategies[invocation.strategy_id]=observed.value();
        }
    }
    J result={{"schema_version","live-equity-multi-consumption/v1"},{"profile","live_equity_multi_sleeve"},
        {"scope","primary_invocation"},{"full_run_certification",false},
        {"run_key",{{"portfolio_id",book},{"strategy_id",engine},{"date",date}}},{"effective_sha256",hash},
        {"source_to_storage_owners",live_config_source_owner_map(snapshot,engine)},
        {"coverage",{{"primary",non_trading?"skipped_non_trading_day":"observed"},
            {"legacy_run_stages","not_collected"},{"account_execution_costs","not_collected"}}},
        {"portfolio_invocation",portfolio},{"strategy_invocations",strategies}};
    need(validate_equity_multi_consumption(result,snapshot,engine,book,date,hash));return result;
}
}
