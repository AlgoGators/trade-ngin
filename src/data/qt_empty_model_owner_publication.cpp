#include "trade_ngin/data/live_config_owners.hpp"
// STAGED CANDIDATE ONLY. Keep the separately retained unavailable stub live
// until its genuine native RED gate; this file has not been compiled or run.
#include "trade_ngin/data/qt_empty_model_owner_publication.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>

namespace trade_ngin {
namespace {
using J=nlohmann::json;
constexpr const char* schema="qt-empty-model-owner-publication/v2";
constexpr const char* engine="LIVE_EQUITY_MEAN_REVERSION";
[[noreturn]] void reject(){throw std::invalid_argument("qt_empty_model_owner_invalid");}
void need(bool condition){if(!condition)reject();}
bool fields(const J& j,const std::set<std::string>& names){
    if(!j.is_object()||j.size()!=names.size())return false;
    for(auto it=j.begin();it!=j.end();++it)if(!names.contains(it.key()))return false;
    return true;
}
std::string text(const J& j){
    need(j.is_string());auto s=j.get<std::string>();
    size_t characters=0;for(unsigned char c:s)if((c&0xc0)!=0x80)++characters;
    need(!s.empty()&&characters<=4096&&s.find('\0')==std::string::npos);
    (void)J(s).dump(-1,' ',false,J::error_handler_t::strict);return s;
}
bool uuid(const std::string& s){
    if(s.size()!=36)return false;
    for(size_t i=0;i<s.size();++i){
        if(i==8||i==13||i==18||i==23){if(s[i]!='-')return false;}
        else if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return false;
    }return true;
}
bool digest_text(const J& j){
    if(!j.is_string())return false;const auto s=j.get<std::string>();
    return s.size()==64&&std::all_of(s.begin(),s.end(),[](char c){
        return(c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
bool day(const std::string& s){
    if(s.size()!=10||s[4]!='-'||s[7]!='-')return false;
    for(size_t i=0;i<s.size();++i)if(i!=4&&i!=7&&(s[i]<'0'||s[i]>'9'))return false;
    auto value=std::chrono::year_month_day{std::chrono::year(std::stoi(s.substr(0,4))),
        std::chrono::month(static_cast<unsigned>(std::stoi(s.substr(5,2)))),
        std::chrono::day(static_cast<unsigned>(std::stoi(s.substr(8,2))))};
    return value.ok()&&static_cast<int>(value.year())>0;
}
std::string hash(const J& value){
    auto wire=canonical_qt_desk_source_json(value);need(wire.is_ok());
    auto result=qt_sha256_hex(wire.value());need(result.is_ok());return result.value();
}
J key_json(const ComponentPositionKey& k){return J{{"portfolio_id",k.portfolio_id},
    {"strategy_id",k.strategy_id},{"strategy_name",k.strategy_name},{"date",k.date},
    {"symbol",k.symbol},{"portfolio_type",k.portfolio_type}};}
std::vector<std::string> owners(const J& value){
    need(value.is_array()&&!value.empty()&&value.size()<=4096);
    std::vector<std::string> result;
    for(const auto& owner:value){auto name=text(owner);need(result.empty()||result.back()<name);result.push_back(name);}
    return result;
}
void rows(const J& values,const J& doc,const std::vector<std::string>& names,const std::string& stream){
    need(values.is_array()&&values.size()<=4096);
    const std::set<std::string> ordinary={"key","quantity_exact","average_price_exact"};
    std::set<std::string> shape=ordinary;
    if(stream=="qt_proposal")shape.insert({"action","position_revision","origin_publication_id"});
    const std::set<std::string> key_fields={"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"};
    std::optional<ComponentPositionKey> previous;
    for(const auto& value:values){
        need(fields(value,shape)&&fields(value.at("key"),key_fields));const auto& k=value.at("key");
        ComponentPositionKey key{text(k.at("portfolio_id")),text(k.at("strategy_id")),text(k.at("strategy_name")),
            text(k.at("date")),text(k.at("symbol")),text(k.at("portfolio_type"))};
        need(key.portfolio_id==text(doc.at("book_id"))&&key.strategy_id==engine&&key.date==text(doc.at("source_day"))&&
            key.portfolio_type==stream&&std::binary_search(names.begin(),names.end(),key.strategy_name));
        need(!previous||previous.value()<key);previous=key;
        need(parse_qt_quantity_exact(text(value.at("quantity_exact"))).is_ok()&&
            parse_qt_quantity_exact(text(value.at("average_price_exact"))).is_ok());
        if(stream=="qt_proposal"){
            const auto action=text(value.at("action"));need(action=="inserted"||action=="preserved");
            for(const auto* name:{"position_revision","origin_publication_id"})
                need(value.at(name).is_null()||uuid(text(value.at(name))));
            need(action!="inserted"||(!value.at("position_revision").is_null()&&!value.at("origin_publication_id").is_null()));
            need(!value.at("position_revision").is_null()||value.at("origin_publication_id").is_null());
        }
    }
}
void validate(const J& doc){
    static const std::set<std::string> shape={"schema_version","publication_id","book_id","strategy_id","source_day",
        "configured_owner_names","configuration_digest","system_components","seed_digest","proposal_components",
        "proposal_manifest_digest","qt_components","qt_digest"};
    need(fields(doc,shape)&&text(doc.at("schema_version"))==schema&&text(doc.at("strategy_id"))==engine);
    need(uuid(text(doc.at("publication_id")))&&day(text(doc.at("source_day"))));(void)text(doc.at("book_id"));
    const auto names=owners(doc.at("configured_owner_names"));
    need(doc.at("system_components").is_array()&&doc.at("system_components").empty());
    for(const auto* name:{"configuration_digest","seed_digest","proposal_manifest_digest","qt_digest"})need(digest_text(doc.at(name)));
    rows(doc.at("proposal_components"),doc,names,"qt_proposal");rows(doc.at("qt_components"),doc,names,"qt");
    need(text(doc.at("seed_digest"))==hash(J{{"seed_rows",J::array()}}));
    need(text(doc.at("proposal_manifest_digest"))==hash(J{{"proposal_rows",doc.at("proposal_components")}}));
    need(text(doc.at("qt_digest"))==hash(J{{"qt_rows",doc.at("qt_components")}}));
    auto wire=canonical_qt_desk_source_json(doc);need(wire.is_ok()&&wire.value().size()<=1048576);
}
}
Result<J> qt_empty_model_owner_document(const QtEmptyModelOwnerPublication& value){
    try{
        const auto& p=value.publication;need(!p.producer_version.empty()&&p.system_components.empty());
        need(value.configuration_snapshot.is_object()&&value.configuration_snapshot.contains("strategies")&&
            value.configuration_snapshot.at("strategies").is_object());
        std::vector<std::string> expected;
        const auto strategies=live_config_storage_strategies(value.configuration_snapshot,p.strategy_id);
        for(auto it=strategies.begin();it!=strategies.end();++it){
            (void)text(J(it.key()));need(it.value().is_object());
            if(it.value().contains("enabled_live"))need(it.value().at("enabled_live").is_boolean());
            if(it.value().value("enabled_live",false))expected.push_back(it.key());
        }
        need(!expected.empty()&&expected.size()<=4096&&value.configured_owner_names==expected&&
            value.fresh_empty_batches.size()==expected.size());
        std::set<std::string> fresh;
        for(const auto& batch:value.fresh_empty_batches){
            need(batch.portfolio_id==p.portfolio_id&&batch.strategy_id==p.strategy_id&&batch.source_day==p.source_day&&
                std::binary_search(expected.begin(),expected.end(),batch.strategy_name)&&fresh.insert(batch.strategy_name).second);
        }
        auto manifest=qt_proposal_manifest_document(p);need(manifest.is_ok());
        auto qt=value.qt_components;std::sort(qt.begin(),qt.end(),[](const auto& a,const auto& b){return a.key<b.key;});
        J qt_rows=J::array();for(const auto& row:qt)qt_rows.push_back({{"key",key_json(row.key)},
            {"quantity_exact",row.quantity.to_string()},{"average_price_exact",row.average_price.to_string()}});
        J doc={{"schema_version",schema},{"publication_id",p.publication_id},{"book_id",p.portfolio_id},
            {"strategy_id",p.strategy_id},{"source_day",p.source_day},{"configured_owner_names",expected},
            {"configuration_digest",hash(value.configuration_snapshot)},{"system_components",J::array()},
            {"seed_digest",hash(J{{"seed_rows",J::array()}})},{"proposal_components",manifest.value().at("proposal_rows")},
            {"proposal_manifest_digest",hash(manifest.value())},{"qt_components",qt_rows},{"qt_digest",hash(J{{"qt_rows",qt_rows}})}};
        validate(doc);return doc;
    }catch(const std::exception&){return make_error<J>(ErrorCode::INVALID_ARGUMENT,"qt_empty_model_owner_invalid");}
}
Result<std::string> canonical_qt_empty_model_owner_bytes(const J& doc){
    try{validate(doc);auto encoded=canonical_qt_desk_source_json(doc);need(encoded.is_ok());
        return std::string(schema)+"\n"+encoded.value();
    }catch(const std::exception&){return make_error<std::string>(ErrorCode::INVALID_ARGUMENT,"qt_empty_model_owner_invalid");}
}
} // namespace trade_ngin
