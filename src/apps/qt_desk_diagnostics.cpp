// Stored diagnostics are a projection of admitted evidence; they never resize a choice.
#include "trade_ngin/apps/qt_desk_cycle.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <stdexcept>

namespace trade_ngin { namespace {
using J=nlohmann::json;
void require(bool p){if(!p)throw std::invalid_argument("qt_desk_diagnostics_unavailable");}
Decimal number(const J& v){require(v.is_string());auto n=parse_qt_quantity_exact(v.get<std::string>());require(n.is_ok());return n.value();}
std::string key(J k){k["portfolio_type"]="qt";return k.dump();}
Decimal difference(Decimal a,Decimal b){auto x=a.raw_value(),y=b.raw_value();
    require(!((y>0&&x<INT64_MIN+y)||(y<0&&x>INT64_MAX+y)));return Decimal::from_raw(x-y);}
// Integer digit multiplication retains all 24 fractional places of three fixed8
// factors. Diagnostic notionals may exceed ledger range; never silently round.
std::string product(Decimal a,Decimal b,Decimal c){
    std::string digits="1";bool negative=false;
    for(auto d:{a,b,c}){
        auto factor=std::to_string(d.raw_value());if(factor.front()=='-'){negative=!negative;factor.erase(0,1);}
        std::vector<int> result(digits.size()+factor.size());
        for(int i=static_cast<int>(digits.size())-1;i>=0;--i)
            for(int j=static_cast<int>(factor.size())-1;j>=0;--j)result[i+j+1]+=(digits[i]-'0')*(factor[j]-'0');
        for(int i=static_cast<int>(result.size())-1;i>0;--i){result[i-1]+=result[i]/10;result[i]%=10;}
        digits.clear();for(auto n:result)digits+=static_cast<char>('0'+n);
        auto first=digits.find_first_not_of('0');digits=first==std::string::npos?"0":digits.substr(first);
    }
    if(digits=="0")return "0";
    if(digits.size()<=24)digits=std::string(25-digits.size(),'0')+digits;
    digits.insert(digits.size()-24,".");while(digits.back()=='0')digits.pop_back();if(digits.back()=='.')digits.pop_back();
    return (negative?"-":"")+digits;
}
// Exact decimal product of the admitted round-trip model operand strings.
// This diagnostic retains source precision; it does not quantize ledger cash.
std::string model_product(const std::vector<std::string>& factors){
    std::string digits="1";int scale=0;bool negative=false;
    for(auto factor:factors){
        require(!factor.empty()&&factor.size()<=64);
        if(factor.front()=='-'){negative=!negative;factor.erase(0,1);}
        auto exponent=factor.find_first_of("eE");if(exponent!=std::string::npos){size_t used=0;int n=std::stoi(factor.substr(exponent+1),&used);require(used==factor.size()-exponent-1&&n>=-400&&n<=400);scale-=n;factor.erase(exponent);}
        auto point=factor.find('.');if(point!=std::string::npos){scale+=static_cast<int>(factor.size()-point-1);factor.erase(point,1);}
        require(!factor.empty()&&factor.find_first_not_of("0123456789")==std::string::npos);
        std::vector<int> result(digits.size()+factor.size());
        for(int i=static_cast<int>(digits.size())-1;i>=0;--i)for(int j=static_cast<int>(factor.size())-1;j>=0;--j)result[i+j+1]+=(digits[i]-'0')*(factor[j]-'0');
        for(int i=static_cast<int>(result.size())-1;i>0;--i){result[i-1]+=result[i]/10;result[i]%=10;}
        digits.clear();for(auto n:result)digits+=static_cast<char>('0'+n);auto first=digits.find_first_not_of('0');digits=first==std::string::npos?"0":digits.substr(first);
    }
    if(digits=="0")return "0";
    require(scale>=-1200&&scale<=1200);
    if(scale<0)digits.append(static_cast<size_t>(-scale),'0');
    else if(scale>0){if(digits.size()<=static_cast<size_t>(scale))digits=std::string(static_cast<size_t>(scale)+1-digits.size(),'0')+digits;digits.insert(digits.size()-static_cast<size_t>(scale),".");while(digits.back()=='0')digits.pop_back();if(digits.back()=='.')digits.pop_back();}
    return (negative?"-":"")+digits;
}
long double diagnostic(const J& v){require(v.is_string());auto s=v.get<std::string>();size_t used=0;auto n=std::stold(s,&used);require(used==s.size()&&std::isfinite(n));return n;}
std::string diagnostic_difference(const J& a,const J& b){double n=static_cast<double>(diagnostic(a)-diagnostic(b));require(std::isfinite(n));char out[64];auto [end,ec]=std::to_chars(out,out+64,n,std::chars_format::general);require(ec==std::errc{});return {out,end};}
}
Result<J> build_qt_desk_diagnostics(const J& d,const J& p,const J& facts,const J& in,const J& accounting){
 try{
    J result={{"schema_version","qt-desk-diagnostics/v1"},{"decision_id",d.at("decision_id")},
        {"model_publication_id",d.at("model_publication_id")},{"preview_id",d.at("preview_id")},
        {"selection_policy","exact-confirmed-choice"},{"controls_applied",J::array()},
        {"model_to_choice",J::array()},{"execution_distance",accounting.at("distance")},
        {"per_owner_solver_recommendation",{{"status","unavailable"},{"reason","optimizer_produces_aggregate_weights_without_owner_allocation"}}},
        {"diagnostics_computed",J::array()},{"diagnostics_skipped",J::array()}};
    std::map<std::string,J> model,markets;
    for(const auto& row:facts.at("source_rows"))require(model.emplace(key(row.at("key")),row).second);
    for(const auto& row:in.at("instruments"))require(markets.emplace(row.at("symbol").get<std::string>(),row).second);
    for(const auto& row:p.at("selection_rows")){
        auto id=key(row.at("key"));J item={{"key",J::parse(id)},{"selected_quantity_exact",row.at("quantity_exact")}};
        if(!model.contains(id)){
            item.update({{"status","unavailable"},{"reason","model_component_absent"},{"model_quantity_exact",nullptr},{"quantity_delta_exact",nullptr},{"notional_delta_exact",nullptr}});
        }else{
            const auto& price=markets.at(row.at("key").at("symbol").get<std::string>());
            auto delta=difference(number(row.at("quantity_exact")),number(model.at(id).at("quantity_exact")));
            const bool model_numbers=in.at("schema_version")=="qt-futures-accounting-input/v2"||
                in.at("schema_version")=="qt-futures-accounting-input-first-day/v1";
            std::string notional;
            if(model_numbers)notional=model_product({delta.to_string(),price.at("price_model_number").get<std::string>(),price.at("point_value").get<std::string>()});
            else{auto mark=number(price.at("price_exact")),multiplier=number(price.at("point_value"));require(mark.raw_value()>0&&multiplier.raw_value()>0);notional=product(delta,mark,multiplier);}
            item.update({{"status","computed"},{"model_quantity_exact",model.at(id).at("quantity_exact")},{"quantity_delta_exact",delta.to_string()},
                {"notional_delta_exact",notional},
                {"price_source_id",price.at("source_id")},{"accounting_source_id",in.at("accounting_source_id")},{"currency",in.at("currency")},
                {"moved_by",delta.is_zero()?"unchanged":"confirmed_qt_choice"},{"controls_applied",J::array()}});
            item[model_numbers?"price_model_number":"price_exact"]=price.at(model_numbers?"price_model_number":"price_exact");
            item[model_numbers?"valuation_multiplier_model_number":"valuation_multiplier_exact"]=price.at("point_value");
        }result["model_to_choice"].push_back(item);
    }
    std::sort(result["model_to_choice"].begin(),result["model_to_choice"].end(),[](const J&a,const J&b){return a.at("key").dump()<b.at("key").dump();});
    const auto& evaluation=p.at("evaluation");
    for(auto stage:{"optimizer","selected_risk","selected_costs"}){
        const auto& s=evaluation.at(stage);J entry={{"stage",stage},{"status",s.at("status")},{"diagnostics",s.at("diagnostics")}};
        result[s.at("status")=="evaluated"?"diagnostics_computed":"diagnostics_skipped"].push_back(entry);
    }
    const auto& risk=evaluation.at("selected_risk");result["risk"]=risk;
    J multipliers=J::object(),vars=J::object(),bindings=J::array();
    for(const auto& metric:risk.at("metrics")){
        auto code=metric.at("code").get<std::string>();
        if(code=="portfolio_multiplier"||code=="jump_multiplier"||code=="correlation_multiplier"||code=="leverage_multiplier")multipliers[code]=metric;
        if(code=="portfolio_var"||code=="portfolio_var_gate")vars[code]=metric;
    }
    if(multipliers.size()==4){
        long double minimum=1;for(const auto& m:multipliers)minimum=std::min(minimum,diagnostic(m.at("value_diagnostic")));
        // A unit multiplier does not constrain a scale; every tied active limit is retained.
        if(minimum<1)for(auto it=multipliers.begin();it!=multipliers.end();++it)
            if(diagnostic(it.value().at("value_diagnostic"))==minimum)bindings.push_back(it.key());
    }
    result["risk"]["multipliers"]=multipliers;result["risk"]["var_forms"]=vars;
    result["risk"]["binding_multipliers"]=bindings;
    result["risk"]["binding_status"]=multipliers.size()==4?"computed":"unavailable";
    const auto& optimizer=evaluation.at("optimizer");result["optimizer"]=optimizer;
    result["optimizer"]["actual_iterations"]=nullptr;result["optimizer"]["tracking_error_diagnostic"]=nullptr;
    J trace=nullptr;
    for(const auto& line:optimizer.at("trace")){
        auto s=line.get<std::string>();
        if(s.starts_with("actual_iterations=")){auto count=s.substr(18);size_t used=0;auto n=std::stoll(count,&used);require(used==count.size()&&n>=0&&n<=INT_MAX);result["optimizer"]["actual_iterations"]=n;}
        else if(s.starts_with("tracking_error="))result["optimizer"]["tracking_error_diagnostic"]=s.substr(15);
        else if(s.starts_with("{"))trace=J::parse(s);
    }
    result["optimizer"]["buffer_trace"]=trace;
    result["optimizer"]["aggregate_recommendation"]=J::array();
    const auto& bindings_in=optimizer.at("aggregate_bindings");
    for(size_t index=0;index<bindings_in.size();++index){
        const auto& binding=bindings_in[index];J entry=binding;
        const auto symbol=binding.at("symbol"),type=binding.at("instrument_type");
        for(auto field:{"current_weights","target_weights","solved_weights"}){
            bool found=false;for(const auto& weight:optimizer.at(field))if(weight.at("symbol")==symbol&&weight.at("instrument_type")==type){require(!found);entry[field]=weight.at("weight_diagnostic");found=true;}require(found);
        }
        entry["target_to_recommendation_weight_delta_diagnostic"]=diagnostic_difference(entry.at("solved_weights"),entry.at("target_weights"));
        Decimal selected(0);for(const auto& member:binding.at("component_keys"))for(const auto& row:p.at("selection_rows"))if(key(row.at("key"))==key(member)){
            auto q=number(row.at("quantity_exact"));require(q.raw_value()!=INT64_MIN);selected=difference(selected,Decimal::from_raw(-q.raw_value()));}
        entry["confirmed_net_quantity_exact"]=selected.to_string();
        entry["moved_by"]=J::array();
        if(!trace.is_null()){
            for(auto pair:{std::pair{"solver_positions","solver"},std::pair{"continuous_buffered_positions","buffer"},std::pair{"rounded_buffered_positions","rounding"}}){
                const auto& values=trace.at(pair.first);if(!values.is_null())entry[pair.first]=values.at(index);
            }
            auto before=entry.at("target_weights");
            for(auto pair:{std::pair{"solver_positions","solver"},std::pair{"continuous_buffered_positions","buffer"},std::pair{"rounded_buffered_positions","rounding"}})
                if(entry.contains(pair.first)){if(diagnostic(entry.at(pair.first))!=diagnostic(before))entry["moved_by"].push_back(pair.second);before=entry.at(pair.first);}
            // ReturnedPrior has no continuous/rounded vectors: the actual final
            // recommendation is the current weight returned by the buffer.
            if(trace.at("buffer_branch")=="returned_prior"&&entry.contains("solver_positions")&&
                diagnostic(entry.at("solver_positions"))!=diagnostic(entry.at("solved_weights")))
                entry["moved_by"].push_back("buffer");
            entry["buffer_branch"]=trace.at("buffer_branch");
        }
        entry["controls_applied_to_confirmed_choice"]=J::array();
        result["optimizer"]["aggregate_recommendation"].push_back(entry);
    }
    return result;
 }catch(const std::exception&){return make_error<J>(ErrorCode::INVALID_DATA,"qt_desk_diagnostics_unavailable","qt_desk_diagnostics");}
}
}
