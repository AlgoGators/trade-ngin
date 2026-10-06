#include "trade_ngin/apps/qt_equity_accounting_output.hpp"
#include "trade_ngin/apps/qt_equity_desk_cycle.hpp"
#include "trade_ngin/apps/qt_equity_cost_consumption.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/git_version.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <regex>
#include <set>
#include <stdexcept>

namespace trade_ngin { namespace {
using J=nlohmann::json;
void need(bool ok){if(!ok)throw std::invalid_argument("qt_equity_output_unavailable");}
std::string text(const J& v){need(v.is_string());auto s=v.get<std::string>();need(!s.empty()&&s.size()<=4096);return s;}
void hex(const J& v){auto s=text(v);need(s.size()==64&&s.find_first_not_of("0123456789abcdef")==std::string::npos);}
void uuid(const J& v){static const std::regex form(R"([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})");need(std::regex_match(text(v),form));}
void ordinal(const J& v){need(v.is_number_integer()&&!v.is_boolean());need(v.is_number_unsigned()?v.get<uint64_t>()>0&&v.get<uint64_t>()<=INT64_MAX:v.get<int64_t>()>0);}
int64_t timestamp(const J& v){static const std::regex form(R"([0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]{1,6})?Z)");const auto s=text(v);need(std::regex_match(s,form));using namespace std::chrono;const auto y=std::stoi(s.substr(0,4));year_month_day day{year{y},month{static_cast<unsigned>(std::stoi(s.substr(5,2)))},std::chrono::day{static_cast<unsigned>(std::stoi(s.substr(8,2)))}};need(y>0&&day.ok());const int h=std::stoi(s.substr(11,2)),m=std::stoi(s.substr(14,2)),sec=std::stoi(s.substr(17,2));need(h<24&&m<60&&sec<60);int64_t fraction=0;if(s.size()>20){auto part=s.substr(20,s.size()-21);while(part.size()<6)part+='0';fraction=std::stoll(part);}return duration_cast<microseconds>(sys_days{day}.time_since_epoch()+hours{h}+minutes{m}+seconds{sec}).count()+fraction;}
std::string hash(const J& j){auto b=canonical_qt_desk_input_json(j);need(b.is_ok());auto h=qt_sha256_hex(b.value());need(h.is_ok());return h.value();}
}
Result<J> recompute_qt_equity_accounting_output(const J& d,const J& selection,const J& input,const J& a){try{
    const std::set<std::string> fields={"schema_version","book_id","source_day","model_publication_id","snapshot_id","source_version","as_of","valid_until","content_digest","producer_id","policy_version","policy_revision","policy_updated_at","evaluator_build","evaluator_sha256","evaluator_bundle_sha256","allowed_override_codes"};
    need(a.is_object()&&a.size()==fields.size());for(const auto& f:fields)need(a.contains(f));
    need(a.at("schema_version")=="qt-input-authority/v1");
    for(auto f:{"book_id","source_day"})need(a.at(f)==d.at(f));
    // The offline arithmetic wire intentionally supplies only decision_id,
    // book_id and source_day. SQL separately binds the full confirmed model.
    if(d.contains("model_publication_id"))need(a.at("model_publication_id")==d.at("model_publication_id"));
    text(a.at("book_id"));text(a.at("source_day"));uuid(a.at("model_publication_id"));
    ordinal(a.at("snapshot_id"));ordinal(a.at("policy_revision"));
    for(auto f:{"source_version","producer_id","policy_version"})text(a.at(f));
    for(auto f:{"content_digest","evaluator_sha256","evaluator_bundle_sha256"})hex(a.at(f));
    need(timestamp(a.at("as_of"))<timestamp(a.at("valid_until")));timestamp(a.at("policy_updated_at"));
    need(text(a.at("evaluator_build"))==std::string(TRADE_NGIN_GIT_SHA));
    // Preserve the captured sorted policy array, including duplicates or unused
    // codes. Override admission belongs to the confirmed caller, not arithmetic.
    const auto& codes=a.at("allowed_override_codes");need(codes.is_array()&&codes.size()<=4096);
    std::string previous;for(const auto& c:codes){auto code=text(c);need(previous.empty()||previous<=code);previous=code;}
    std::vector<QtEquityCostTrace> trace;
    auto result=produce_qt_equity_accounting(d,selection,input,&trace);need(result.is_ok());
    auto output=result.value();output["input_digest"]=hash(input);output["producer_authority"]=a;
    auto child=project_qt_equity_cost_consumption(d,input,output,&trace);need(child.is_ok());
    output["consumption"]=child.value();return output;
}catch(const std::exception&){return make_error<J>(ErrorCode::INVALID_DATA,"qt_equity_output_unavailable","qt_equity_accounting_output");}}
}
