#include "trade_ngin/apps/equity_model_prior.hpp"
#include "trade_ngin/apps/equity_model_action_source.hpp"
#include "trade_ngin/data/qt_equity_desk_accounting.hpp"
#include "trade_ngin/data/qt_equity_desk_finalization.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include <regex>
#include <set>
#include <stdexcept>
#include <string_view>

namespace trade_ngin { namespace {
using J=nlohmann::json;
void need(bool value){if(!value)throw std::invalid_argument("equity_model_prior_unavailable");}
bool uuid(const std::string& value){static const std::regex pattern("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");return std::regex_match(value,pattern)&&value!="00000000-0000-0000-0000-000000000000";}
std::string text(const J& value){need(value.is_string());auto s=value.get<std::string>();need(!s.empty()&&s.size()<=4096);return s;}
J one(pqxx::work& tx,const std::string& sql){auto rows=tx.exec(sql);need(rows.size()==1&&!rows[0][0].is_null()&&std::string_view(rows[0][0].c_str()).size()<=2*1024*1024);return J::parse(rows[0][0].c_str());}
void owner(const J& key,const EquityModelPriorOwner& wanted){
    need(key.at("portfolio_id")==wanted.portfolio_id&&key.at("strategy_id")==wanted.strategy_id&&
        key.at("strategy_name")==wanted.strategy_name&&key.at("date")==wanted.source_day&&key.at("portfolio_type")=="qt");
}
Timestamp timestamp(const std::string& value){
    static const std::regex pattern("^([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})(?:\\.([0-9]{1,6}))?Z$");
    std::smatch fields;need(std::regex_match(value,fields,pattern));
    const std::chrono::year_month_day day{std::chrono::year(std::stoi(fields[1])),
        std::chrono::month(static_cast<unsigned>(std::stoi(fields[2]))),
        std::chrono::day(static_cast<unsigned>(std::stoi(fields[3])))};
    const int hours=std::stoi(fields[4]),minutes=std::stoi(fields[5]),seconds=std::stoi(fields[6]);
    need(day.ok()&&hours<24&&minutes<60&&seconds<60);
    std::string fraction=fields[7];if(fraction.empty())fraction="0";while(fraction.size()<6)fraction+='0';
    return std::chrono::sys_days(day)+std::chrono::hours(hours)+std::chrono::minutes(minutes)+
        std::chrono::seconds(seconds)+std::chrono::microseconds(std::stoi(fraction));
}
}
Result<EquityModelPriorSelection> parse_equity_model_prior_arguments(const std::vector<std::string>& arguments){try{
    EquityModelPriorSelection result;bool mode=false,decision=false,finalization=false;
    for(size_t i=0;i<arguments.size();++i){const auto& value=arguments[i];
        if(value=="--verified-desk-prior"){need(!mode);mode=true;continue;}
        if(value=="--prior-decision"||value=="--prior-finalization"){
            need(i+1<arguments.size());const auto& id=arguments[++i];need(uuid(id));
            if(value=="--prior-decision"){need(!decision);decision=true;result.decision_id=id;}
            else {need(!finalization);finalization=true;result.finalization_id=id;}
            continue;
        }
        need(!value.starts_with("--prior-")&&!value.starts_with("--verified-desk-"));
        result.runner_arguments.push_back(value);
    }
    need((!mode&&!decision&&!finalization)||(mode&&decision&&finalization));
    if(mode)result.mode=EquityModelPriorMode::VerifiedDeskPrior;
    return result;
}catch(const std::exception&){return make_error<EquityModelPriorSelection>(ErrorCode::INVALID_ARGUMENT,"equity_model_prior_arguments_invalid","equity_model_prior");}}

Result<VerifiedEquityModelPrior> load_verified_equity_model_prior(pqxx::work& tx,
    const EquityModelPriorSelection& selection,const EquityModelPriorOwner& wanted){try{
    need(selection.mode==EquityModelPriorMode::VerifiedDeskPrior&&uuid(selection.decision_id)&&uuid(selection.finalization_id));
    need(!wanted.portfolio_id.empty()&&wanted.portfolio_id.size()<=100&&wanted.strategy_id=="LIVE_EQUITY_MEAN_REVERSION"&&wanted.strategy_name=="EQUITY_MEAN_REVERSION");
    tx.exec("SELECT pg_advisory_xact_lock(hashtextextended('algolens:qt-book:'||upper(btrim("+tx.quote(wanted.portfolio_id)+")),0))");
    const auto d=one(tx,"SELECT to_jsonb(d) FROM trading.qt_decisions d WHERE decision_id="+tx.quote(selection.decision_id)+"::uuid FOR SHARE");
    need(d.at("book_id")==wanted.portfolio_id&&d.at("source_day")==wanted.source_day);
    const auto f=one(tx,"SELECT to_jsonb(f) FROM trading.qt_desk_finalizations f WHERE finalization_id="+tx.quote(selection.finalization_id)+"::uuid FOR SHARE");
    need(f.at("decision_id")==selection.decision_id&&f.at("book_id")==wanted.portfolio_id&&f.at("source_day")==wanted.source_day&&f.at("valuation_day")==wanted.valuation_day);
    const auto input_id=one(tx,"SELECT to_jsonb(input_id::text) FROM trading.desk_run_results WHERE decision_id="+tx.quote(selection.decision_id)+"::uuid FOR SHARE");
    auto original=load_qt_equity_desk_original_records(tx,d,text(input_id));need(original.is_ok());
    auto after=verified_qt_equity_desk_finalization(tx,d);need(after.is_ok()&&!after.value().is_null());
    need(verify_qt_equity_desk_accounting_outputs(tx,d,text(input_id)).is_ok());
    // Original/successor/current physical proof remains authoritative.
    const auto& original_output=original.value().at("output").at("payload");
    need(original_output.at("corporate_action_adjustments").is_array());
    const auto anchor=one(tx,"SELECT to_jsonb(s) FROM trading.qt_desk_finalization_sources s WHERE source_id="+tx.quote("qt-finalization/"+selection.finalization_id)+" FOR SHARE");
    const auto& anchor_payload=anchor.at("payload");
    const bool empty_anchor=anchor_payload.at("schema_version")=="qt-equity-finalized-accounting-empty-owner/v3";
    need(anchor_payload.at("schema_version")=="qt-equity-finalized-accounting/v2" ||
        (empty_anchor && anchor_payload.at("previous_positions").is_array() &&
         anchor_payload.at("previous_positions").empty() && after.value().at("positions").is_array() &&
         after.value().at("positions").empty()));
    VerifiedEquityModelPrior result;result.financial=after.value();result.basis_positions=anchor.at("payload").at("previous_positions");
    need(result.financial.at("positions").is_array()&&result.financial.at("positions").size()<=4096&&result.basis_positions.size()==result.financial.at("positions").size());
    std::set<std::string> symbols;
    for(const auto& row:result.financial.at("positions")){
        const auto& key=row.at("key");owner(key,wanted);const auto symbol=text(key.at("symbol"));need(symbols.insert(symbol).second);
        auto qty=parse_qt_quantity_exact(text(row.at("quantity_exact"))),basis=parse_qt_quantity_exact(text(row.at("average_price_exact"))),
            unrealized=parse_qt_quantity_exact(text(row.at("daily_unrealized_pnl_exact"))),realized=parse_qt_quantity_exact(text(row.at("daily_realized_pnl_exact")));
        need(qty.is_ok()&&basis.is_ok()&&unrealized.is_ok()&&realized.is_ok());
        // The immutable UTC successor mark belongs to valuation_day, while the
        // physical owner/date remains source_day; never query DATE(last_update).
        const auto stamp=text(row.at("last_update"));need(stamp.size()>=20&&stamp.substr(0,10)==wanted.valuation_day&&stamp.back()=='Z');
        const auto when=timestamp(stamp);
        result.positions.emplace(symbol,Position{symbol,qty.value(),basis.value(),unrealized.value(),realized.value(),when});
    }
    const auto& r=original.value();
    result.replay_reference={{"schema_version","qt-equity-model-prior/v1"},{"mode","verified_desk_prior"},
        {"book_id",wanted.portfolio_id},{"source_day",wanted.source_day},{"valuation_day",wanted.valuation_day},
        {"decision_id",selection.decision_id},{"finalization_id",selection.finalization_id},
        {"finalization_digest",f.at("content_digest")},{"finalization_source_id",anchor.at("source_id")},{"finalization_source_digest",anchor.at("content_digest")},
        {"model_publication_id",r.at("model").at("publication_id")},{"model_seed_digest",r.at("model").at("seed_digest")},
        {"accounting_input_id",r.at("input").at("input_id")},{"accounting_input_digest",r.at("input").at("content_digest")},
        {"attempt_id",r.at("receipt").at("attempt_id")},{"observation_id",r.at("observed").at("observation_id")},
        {"observation_digest",r.at("observed").at("content_digest")},{"results_digest",r.at("desk_result").at("content_digest")},
        {"basis_positions",result.basis_positions},{"action_admission","action_free_only"}};
    auto frame=capture_equity_model_action_frame(tx,result,wanted,
        original_output.at("corporate_action_adjustments"),f);
    need(frame.is_ok());result.positions=frame.value().positions;
    result.replay_reference=frame.value().replay_reference;
    return result;
}catch(const std::exception&){return make_error<VerifiedEquityModelPrior>(ErrorCode::INVALID_DATA,"equity_model_verified_prior_unavailable","equity_model_prior");}}
}
