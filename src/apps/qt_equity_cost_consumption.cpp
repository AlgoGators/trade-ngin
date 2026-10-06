#include "trade_ngin/apps/qt_equity_cost_consumption.hpp"
#include "trade_ngin/apps/consumption_projection.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/git_version.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>

namespace trade_ngin { namespace {
using J=nlohmann::json;
using namespace transaction_cost;
constexpr std::size_t max_rows=2048,max_bytes=2097152;
void need(bool ok){if(!ok)throw std::invalid_argument("qt_equity_cost_consumption_unavailable");}
void shape(const J& row,std::initializer_list<const char*> keys){
    need(row.is_object()&&row.size()==keys.size());for(auto key:keys)need(row.contains(key));
}
std::string text(const J& value){
    need(value.is_string());auto result=value.get<std::string>();need(!result.empty()&&result.size()<=96);return result;
}
std::string identity(const J& value,bool symbol=false){
    const auto s=text(value);static const std::regex ordinary("[A-Za-z0-9][A-Za-z0-9_.:-]{0,63}");
    static const std::regex instrument("[A-Za-z0-9][A-Za-z0-9_.:/-]{0,63}");
    need(std::regex_match(s,symbol?instrument:ordinary));return s;
}
std::string uuid(const J& value){
    auto s=text(value);static const std::regex form("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}");
    need(std::regex_match(s,form));return s;
}
std::string day(const J& value){
    auto s=text(value);need(s.size()==10&&s[4]=='-'&&s[7]=='-');
    for(std::size_t i=0;i<s.size();++i)if(i!=4&&i!=7)need(s[i]>='0'&&s[i]<='9');
    using namespace std::chrono;
    const year_month_day d{year{std::stoi(s.substr(0,4))},month{static_cast<unsigned>(std::stoi(s.substr(5,2)))},
        std::chrono::day{static_cast<unsigned>(std::stoi(s.substr(8,2)))}};
    need(d.ok()&&int(d.year())>0);return s;
}
double number(const J& value){
    auto s=text(value);static const std::regex form(R"(-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?)");
    need(s.size()<=64&&std::regex_match(s,form));double n=0;
    const auto parsed=std::from_chars(s.data(),s.data()+s.size(),n,std::chars_format::general);
    need(parsed.ec==std::errc{}&&parsed.ptr==s.data()+s.size()&&std::isfinite(n));return n;
}
Decimal exact(const J& value){auto q=parse_qt_quantity_exact(text(value));need(q.is_ok());return q.value();}
Decimal cash(double value){
    need(std::isfinite(value));const double scaled=value*100000000.0+(value>=0?0.5:-0.5);
    need(std::isfinite(scaled)&&static_cast<long double>(scaled)>=static_cast<long double>(INT64_MIN)&&
        static_cast<long double>(scaled)<=static_cast<long double>(INT64_MAX));
    return Decimal::from_raw(static_cast<int64_t>(scaled));
}
std::string hash(const J& value){
    auto bytes=canonical_qt_desk_input_json(value);need(bytes.is_ok());
    auto result=qt_sha256_hex(bytes.value());need(result.is_ok());return result.value();
}
void bounded(const J& rows){need(rows.is_array()&&rows.size()<=max_rows);}
J key_json(const ComponentPositionKey& key){return {{"portfolio_id",key.portfolio_id},{"strategy_id",key.strategy_id},
    {"strategy_name",key.strategy_name},{"date",key.date},{"symbol",key.symbol},{"portfolio_type",key.portfolio_type}};}
ComponentPositionKey component(const J& key,const std::string& book,const std::string& date){
    shape(key,{"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"});
    need(key.at("portfolio_id")==book&&key.at("date")==date&&key.at("portfolio_type")=="qt");
    return {identity(key.at("portfolio_id")),identity(key.at("strategy_id")),identity(key.at("strategy_name")),
        day(key.at("date")),identity(key.at("symbol"),true),"qt"};
}
using Components=std::map<ComponentPositionKey,const J*>;
Components indexed(const J& rows,const std::string& book,const std::string& date){
    bounded(rows);Components result;for(const auto& row:rows)need(result.emplace(component(row.at("key"),book,date),&row).second);
    return result;
}
void matches(const std::optional<double>& observed,double expected){
    need(observed.has_value()&&std::isfinite(*observed)&&std::isfinite(expected)&&*observed==expected);
}
void matches(const std::optional<bool>& observed,const J& expected){
    need(expected.is_boolean()&&observed.has_value()&&*observed==expected.get<bool>());
}

// This validates and serializes observed reads. It never calls the financial
// model and never manufactures a read from a supplied setting alone.
J charge_reads(const CostChargeObservation& used,const J& instrument,const J& config){
    const auto& parameters=instrument.at("cost_parameters");
    need(parameters.is_object()&&config.is_object());
    std::map<std::string,J> values;
    auto numeric=[&](const char* field,const std::optional<double>& observed,const J& supplied){
        matches(observed,number(supplied));need(values.emplace(field,*observed).second);
    };
    auto flag=[&](const char* field,const std::optional<bool>& observed,const J& supplied){
        matches(observed,supplied);need(values.emplace(field,*observed).second);
    };
    numeric("cost.spread.baseline_spread_ticks",used.spread.baseline_spread_ticks,parameters.at("baseline_spread_ticks"));
    numeric("cost.spread.min_spread_ticks",used.spread.min_spread_ticks,parameters.at("min_spread_ticks"));
    numeric("cost.spread.max_spread_ticks",used.spread.max_spread_ticks,parameters.at("max_spread_ticks"));
    numeric("cost.spread.spread_cost_multiplier",used.spread.spread_cost_multiplier,parameters.at("spread_cost_multiplier"));
    numeric("cost.spread.tick_size",used.spread.tick_size,parameters.at("tick_size"));
    flag("cost.spread.tick_constrained",used.spread.tick_constrained,parameters.at("tick_constrained"));
    numeric("cost.impact.min_adv",used.impact.min_adv,config.at("min_adv"));
    numeric("cost.impact.min_participation",used.impact.min_participation,config.at("min_participation"));
    numeric("cost.impact.max_participation",used.impact.max_participation,config.at("max_participation"));
    numeric("cost.impact.max_impact_bps",used.impact.max_impact_bps,parameters.at("max_impact_bps"));
    numeric("cost.charge.point_value",used.point_value,parameters.at("point_value"));
    need(*used.point_value==1&&number(config.at("min_adv"))>0);
    numeric("cost.charge.commission_per_unit",used.commission_per_unit,parameters.at("commission_per_unit"));
    need(*used.commission_per_unit>=0&&!used.explicit_fee_per_contract);
    numeric("cost.charge.max_commission_pct",used.max_commission_pct,parameters.at("max_commission_pct"));
    if(*used.max_commission_pct<0)
        numeric("cost.charge.max_commission_per_order",used.max_commission_per_order,parameters.at("max_commission_per_order"));
    else need(!used.max_commission_per_order);
    numeric("cost.charge.min_commission_per_order",used.min_commission_per_order,parameters.at("min_commission_per_order"));
    flag("cost.charge.apply_regulatory_fees",used.apply_regulatory_fees,parameters.at("apply_regulatory_fees"));
    if(*used.apply_regulatory_fees&&*used.quantity<0){
        numeric("cost.charge.sec_fee_per_million",used.sec_fee_per_million,parameters.at("sec_fee_per_million"));
        numeric("cost.charge.finra_taf_per_share",used.finra_taf_per_share,parameters.at("finra_taf_per_share"));
        numeric("cost.charge.finra_taf_cap_per_trade",used.finra_taf_cap_per_trade,parameters.at("finra_taf_cap_per_trade"));
    }else need(!used.sec_fee_per_million&&!used.finra_taf_per_share&&!used.finra_taf_cap_per_trade);
    numeric("cost.charge.max_total_implicit_bps",used.max_total_implicit_bps,parameters.at("max_total_implicit_bps"));
    need(!used.volatility.lambda&&!used.volatility.min_multiplier&&!used.volatility.max_multiplier);
    need(used.impact.selected_k_bps&&std::isfinite(*used.impact.selected_k_bps)&&*used.impact.selected_k_bps>0);
    J reads=J::array();
    for(const auto& [field,value]:values){
        const J* definition=nullptr;
        for(const auto& row:consumption_projection_catalog().at("fields"))
            if(row.at("consumer")=="cost.execution"&&row.at("field")==field){need(!definition);definition=&row;}
        need(definition);const auto type=definition->at("type");
        need((type=="bool"&&value.is_boolean())||(type=="number"&&value.is_number()&&
            std::isfinite(value.get<double>())&&value.dump().size()<=25));
        need(std::find(definition->at("origins").begin(),definition->at("origins").end(),J("runtime_effective"))!=definition->at("origins").end());
        reads.push_back({{"field",field},{"value_type",type},{"value",value},{"origin","runtime_effective"}});
    }
    return reads;
}
void outcome_matches(const QtEquityCostTrace& trace,const J& execution){
    const auto& result=trace.outcome;
    for(const double value:{result.commissions_fees,result.spread_price_impact,result.market_impact_price_impact,
        result.implicit_price_impact,result.slippage_market_impact,result.total_transaction_costs})
        need(std::isfinite(value)&&value>=0);
    need(cash(result.commissions_fees)==exact(execution.at("commissions_fees_exact"))&&
        cash(result.implicit_price_impact)==exact(execution.at("implicit_price_impact_exact"))&&
        cash(result.slippage_market_impact)==exact(execution.at("slippage_market_impact_exact"))&&
        cash(result.total_transaction_costs)==exact(execution.at("total_transaction_costs_exact")));
    // Check identities between recorded result components, not another cost run.
    double implicit=result.spread_price_impact+result.market_impact_price_impact;
    const auto& used=trace.observation;
    if(*used.max_total_implicit_bps>=0&&*used.reference_price>0)
        implicit=std::min(implicit,(*used.max_total_implicit_bps/10000.0)* *used.reference_price);
    need(result.implicit_price_impact==implicit&&result.slippage_market_impact==
        result.implicit_price_impact*std::abs(*used.quantity)* *used.point_value&&
        result.total_transaction_costs==result.commissions_fees+result.slippage_market_impact);
}
}

Result<nlohmann::json> project_qt_equity_cost_consumption(
    const J& d,const J& input,const J& financial_output,const std::vector<QtEquityCostTrace>* trace) {
    try{
        need(trace&&trace->size()<=max_rows&&d.is_object()&&input.is_object()&&financial_output.is_object());
        const auto decision_id=uuid(d.at("decision_id")),input_id=uuid(input.at("accounting_input_id"));
        const auto book=identity(d.at("book_id")),date=day(d.at("source_day"));
        const auto build=identity(J(TRADE_NGIN_GIT_SHA));need(build!="unknown"&&build!="unversioned");
        const bool empty_owner=input.at("schema_version")=="qt-equity-accounting-input-empty-owner/v2";
        need((empty_owner||input.at("schema_version")=="qt-equity-accounting-input/v1")&&input.at("calculation_version")=="qt-equity-main08b15c/v1"&&
            input.at("day_mode")=="open"&&input.at("currency")=="USD");
        for(auto field:{"decision_id","book_id","source_day"})need(input.at(field)==d.at(field));
        J output=financial_output;output.erase("consumption");
        need(output.at("schema_version")== (empty_owner ? "qt-equity-accounting-empty-owner/v2" : "qt-equity-accounting/v1")&&output.at("calculation_version")==input.at("calculation_version")&&
            output.at("selection_policy")=="exact-confirmed-choice");
        const auto& observation=output.at("observation");
        need(observation.at("schema_version")=="qt-execution/v2"&&observation.at("accounting_input_id")==input_id);
        for(auto field:{"decision_id","book_id","source_day"})need(observation.at(field)==d.at(field));
        const auto executions=indexed(output.at("executions"),book,date);
        const auto fills=indexed(observation.at("fills"),book,date);
        const auto distances=indexed(output.at("distance"),book,date);
        need((empty_owner ? fills.empty() : !fills.empty())&&fills.size()==distances.size()&&trace->size()==executions.size());
        if(empty_owner) {
            for(const auto field:{"previous_positions","instruments","actions"}) {bounded(input.at(field));need(input.at(field).empty());}
            need(executions.empty()&&trace->empty()&&output.at("corporate_action_adjustments").empty()&&output.at("layers_applied").empty());
            bounded(input.at("previous_totals"));bounded(output.at("live_results"));bounded(output.at("equity_curve"));
            need(input.at("previous_totals").size()==1&&output.at("live_results").size()==1&&output.at("equity_curve").size()==1);
            const auto& engine=input.at("previous_totals")[0].at("strategy_id");
            need(engine=="LIVE_EQUITY_MEAN_REVERSION"&&output.at("live_results")[0].at("strategy_id")==engine&&
                output.at("equity_curve")[0].at("strategy_id")==engine);
        }
        std::map<std::string,const J*> instruments;bounded(input.at("instruments"));
        for(const auto& item:input.at("instruments")){
            need(item.at("asset_type")=="EQUITY");need(instruments.emplace(identity(item.at("symbol"),true),&item).second);
        }
        std::set<std::string> used_symbols;
        std::set<ComponentPositionKey> executed_keys;
        for(const auto& [key,fill]:fills){
            need(distances.contains(key)&&instruments.contains(key.symbol));used_symbols.insert(key.symbol);
            const auto delta=exact(distances.at(key)->at("execution_delta_exact"));
            if(delta.is_zero())need(fill->at("observation_kind")=="carried"&&fill->at("execution_id").is_null()&&
                exact(fill->at("actual_cash_cost_exact")).is_zero()&&!executions.contains(key));
            else{
                need(delta.raw_value()!=INT64_MIN&&fill->at("observation_kind")=="executed"&&executions.contains(key));
                const auto& execution=*executions.at(key);
                need(execution.at("exec_id")==fill->at("execution_id")&&exact(execution.at("quantity_exact"))==delta.abs()&&
                    execution.at("side")== (delta.is_positive()?"BUY":"SELL")&&execution.at("execution_time")==date+"T00:00:00Z"&&
                    exact(execution.at("total_transaction_costs_exact"))==exact(fill->at("actual_cash_cost_exact")));
                executed_keys.insert(key);
            }
        }
        need(used_symbols.size()==instruments.size()&&executed_keys.size()==executions.size());
        std::map<ComponentPositionKey,const QtEquityCostTrace*> ordered;
        for(const auto& item:*trace){
            need(component(key_json(item.key),book,date)==item.key&&executed_keys.contains(item.key));
            need(ordered.emplace(item.key,&item).second);
        }
        J charges=J::array();std::size_t reads_total=0;
        for(const auto& [key,item]:ordered){
            const auto& used=item->observation;const auto& instrument=*instruments.at(key.symbol);
            const auto& execution=*executions.at(key);const auto& evidence=instrument.at("cost_evidence");
            const double reference=number(instrument.at("reference").at("price_model_number"));
            const double adv=number(evidence.at("adv_model_number")),volatility=number(evidence.at("volatility_multiplier_model_number"));
            need(reference>0&&adv>0&&volatility>0&&used.input_source==CostInputSource::explicit_values&&
                used.asset_lookup.path==AssetLookupPath::exact_symbol&&!used.retrieved_adv&&!used.retrieved_volatility_multiplier);
            matches(used.quantity,exact(distances.at(key)->at("execution_delta_exact")).as_double());need(*used.quantity!=0);
            matches(used.reference_price,reference);matches(used.adv_argument,adv);matches(used.effective_adv,adv);
            matches(used.volatility_multiplier_argument,volatility);matches(used.effective_volatility_multiplier,volatility);
            need(cash(reference)==exact(execution.at("price_exact")));
            auto reads=charge_reads(used,instrument,input.at("cost_config"));outcome_matches(*item,execution);
            reads_total+=reads.size();need(reads_total<=16384);
            charges.push_back({{"key",key_json(key)},{"outcome","returned_ok"},
                {"meta",{{"input_source","explicit_values"},{"asset_lookup","exact_symbol"}}},{"reads",std::move(reads)}});
        }
        J result={{"schema_version","qt-equity-cost-consumption/v1"},{"profile","qt_equity_accounting_costs"},
            {"authority","inspection_only"},{"identity",{{"decision_id",decision_id},{"accounting_input_id",input_id},
                {"book_id",book},{"source_day",date},{"input_digest",hash(input)},{"financial_output_digest",hash(output)},
                {"producer_version",build}}},{"coverage",{{"scope","executed_equity_cost_calls"},{"status","complete"}}},
            {"charges",std::move(charges)}};
        need(result.dump().size()<=max_bytes);return result;
    }catch(const std::exception&){return make_error<J>(ErrorCode::INVALID_DATA,
        "qt_equity_cost_consumption_unavailable","qt_equity_cost_consumption");}
}
}
