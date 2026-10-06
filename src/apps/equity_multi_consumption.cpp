#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/apps/equity_multi_consumption.hpp"
#include "trade_ngin/apps/equity_portfolio_consumption.hpp"
#include "trade_ngin/apps/equity_strategy_consumption.hpp"
#include "trade_ngin/data/live_config_owners.hpp"
#include <set>
#include <cmath>
#include <stdexcept>
namespace trade_ngin { namespace {
using J=nlohmann::json;
void need(bool value){if(!value)throw std::invalid_argument("equity_multi_consumption_invalid");}
bool shape(const J& value,std::initializer_list<const char*> keys){
    if(!value.is_object() || value.size()!=keys.size())return false;
    for(auto key:keys)if(!value.contains(key))return false;return true;
}
bool scalar_map(const J& value) {
    if(!value.is_object())return false;
    for(const auto& [key,read]:value.items()) {
        if(key.empty() || key.size()>64 || key=="full_run_certification")return false;
        if(read.is_boolean())continue;
        if(read.is_number() && std::isfinite(read.get<double>()))continue;
        if(read.is_string() && read.get<std::string>().size()<=128)continue;
        return false;
    }
    return true;
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
            need(shape(pass,{"index","reads","optimization_helper","risk_helper","risk"}) && pass.at("index")==i);
            need(shape(pass.at("reads"),{"use_optimization","use_risk_management"}) &&
                pass.at("reads").at("use_optimization")==false && pass.at("reads").at("use_risk_management").is_boolean());
            const auto& risk=pass.at("risk");need(risk.is_object() && risk.size()>=3 && risk.size()<=5);
            for(const auto& [key,read]:risk.items()) {
                (void)read;need(key=="call" || key=="skip" || key=="reads" || key=="manager_source" || key=="lookback_period");
            }
            need(risk.at("call").is_string() && risk.at("skip").is_string() && scalar_map(risk.at("reads")));
            need(pass.at("optimization_helper")=="not_reached" && pass.at("risk_helper").is_string());
        }
        std::set<std::string> owner_names;
        for(const auto& [source,owner]:owners.items()){(void)source;owner_names.insert(owner.get<std::string>());}
        for(const auto* key:{"strategy_charges","compatibility_charges"}) {
            const auto& charges=portfolio.at(key);need(charges.is_array() && charges.size()<=4096);
            for(const auto& charge:charges) {
                need(shape(charge,{"index","purpose","strategy_id","symbol","call","reads"}));
                need(charge.at("index").is_number_unsigned() || charge.at("index").is_number_integer());
                need(charge.at("call")=="returned_ok" && scalar_map(charge.at("reads")));
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
                need(!symbol.empty() && symbol.size()<=64 && shape(read,{"reads","observed_state"}) &&
                    scalar_map(read.at("reads")) && scalar_map(read.at("observed_state")));
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
