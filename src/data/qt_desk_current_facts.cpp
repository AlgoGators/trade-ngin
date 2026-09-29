#include "trade_ngin/data/qt_desk_upstream.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/data/qt_model_publication.hpp"
#include "trade_ngin/data/qt_desk_accounting.hpp"
#include "trade_ngin/data/qt_desk_owner_scope.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <algorithm>

namespace trade_ngin {
namespace {
using Json=nlohmann::json;
[[noreturn]] void reject(){throw std::invalid_argument("qt_desk_source_unavailable");}
std::string float_wire(double number) {
    if(!std::isfinite(number)) reject();
    if(number==0) return std::signbit(number)?"-0.0":"0.0";
    char buffer[64];
    auto converted=std::to_chars(buffer,buffer+sizeof(buffer),number,std::chars_format::scientific);
    if(converted.ec!=std::errc{}) reject();
    std::string scientific(buffer,converted.ptr);
    const auto split=scientific.find('e');
    if(split==std::string::npos) reject();
    const auto exponent=std::stoi(scientific.substr(split+1));
    std::string mantissa=scientific.substr(0,split),sign;
    if(mantissa.front()=='-'){sign="-";mantissa.erase(0,1);}
    std::string digits=mantissa;digits.erase(std::remove(digits.begin(),digits.end(),'.'),digits.end());
    if(exponent>=-4&&exponent<16){
        const int point=exponent+1;
        if(point<=0) return sign+"0."+std::string(-point,'0')+digits;
        if(point>=static_cast<int>(digits.size()))
            return sign+digits+std::string(point-digits.size(),'0')+".0";
        return sign+digits.substr(0,point)+"."+digits.substr(point);
    }
    const auto magnitude=std::to_string(std::abs(exponent));
    return sign+mantissa+"e"+(exponent<0?"-":"+")+
        (magnitude.size()<2?"0":"")+magnitude;
}
void encode(const Json& j,std::string& out,bool input,size_t depth,size_t& nodes) {
    if(++nodes>200000||depth>32) reject();
    if(j.is_number_float()){if(input)reject();out+=float_wire(j.get<double>());}
    else if(j.is_number_unsigned()){
        auto n=j.get<uint64_t>();if(n>static_cast<uint64_t>(INT64_MAX))reject();out+=std::to_string(n);
    }else if(j.is_number_integer()) out+=std::to_string(j.get<int64_t>());
    else if(j.is_string()){
        const auto& s=j.get_ref<const std::string&>();size_t count=0;
        for(unsigned char c:s)if((c&0xc0)!=0x80)++count;
        if(input&&count>4096)reject();
        out+=j.dump(-1,' ',false,Json::error_handler_t::strict);
    }else if(j.is_boolean())out+=j.get<bool>()?"true":"false";
    else if(j.is_null())out+="null";
    else if(j.is_array()){
        if(j.size()>4096)reject();
        out+='[';bool first=true;
        for(auto& value:j){if(!first)out+=',';first=false;encode(value,out,input,depth+1,nodes);}out+=']';
    }else if(j.is_object()){
        if(j.size()>256)reject();
        out+='{';bool first=true;
        for(auto it=j.begin();it!=j.end();++it){
            if(!first)out+=',';
            first=false;encode(Json(it.key()),out,input,depth+1,nodes);
            out+=':';encode(it.value(),out,input,depth+1,nodes);
        }out+='}';
    }else reject();
    if(out.size()>(input?8u*1024*1024:1024*1024))reject();
}
std::string hash(const Json& value,bool input=false){
    std::string wire;size_t nodes=0;encode(value,wire,input,0,nodes);
    auto result=qt_sha256_hex(wire);if(result.is_error())reject();return result.value();
}
std::string stamp(const std::string& expression){
    return "CASE WHEN "+expression+" IS NULL THEN NULL ELSE to_char("+expression+
        " AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS') || CASE WHEN "
        "extract(microseconds FROM "+expression+")::bigint % 1000000=0 THEN '' "
        "ELSE to_char("+expression+" AT TIME ZONE 'UTC','.US') END || 'Z' END";
}
Json query_one(pqxx::work& tx,const std::string& query){
    auto rows=tx.exec(query);if(rows.size()!=1||rows[0][0].is_null())reject();
    return Json::parse(rows[0][0].c_str());
}
Json query_rows(pqxx::work& tx,const std::string& query){
    Json result=Json::array();for(const auto& row:tx.exec(query)){
        if(row[0].is_null())reject();
        result.push_back(Json::parse(row[0].c_str()));
    }return result;
}
std::string decimal_text(const Json& value){
    if(!value.is_string())reject();
    auto raw=value.get<std::string>();
    if(raw.find('.')!=std::string::npos){while(raw.back()=='0')raw.pop_back();if(raw.back()=='.')raw.pop_back();}
    if(raw=="-0")raw="0";
    auto parsed=parse_qt_quantity_exact(raw);if(parsed.is_error())reject();return parsed.value().to_string();
}
Json positions(pqxx::work& tx,const std::string& book,const std::string& day,
               const std::string& stream,bool accounting=false){
    const auto scope="portfolio_id="+tx.quote(book)+" AND date="+tx.quote(day)+
        "::date AND portfolio_type="+tx.quote(stream);
    auto rows=query_rows(tx,
        "SELECT jsonb_build_object('key',jsonb_build_object('portfolio_id',portfolio_id,"
        "'strategy_id',strategy_id,'strategy_name',strategy_name,'date',date,'symbol',symbol,"
        "'portfolio_type',portfolio_type),'quantity_exact',quantity::text,"
        "'average_price_exact',average_price::text)"+
        std::string(stream=="qt_proposal"?
            " || jsonb_build_object('position_revision',qt_proposal_revision::text)":"")+
        (accounting?" || jsonb_build_object('daily_unrealized_pnl_exact',daily_unrealized_pnl::text,"
            "'daily_realized_pnl_exact',daily_realized_pnl::text,'last_update',"+stamp("last_update")+")":"")+
        " FROM trading.positions WHERE "+scope+
        " ORDER BY portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type");
    for(auto& row:rows){
        for(auto field:{"quantity_exact","average_price_exact"})row[field]=decimal_text(row.at(field));
        if(accounting)for(auto field:{"daily_unrealized_pnl_exact","daily_realized_pnl_exact"})
            row[field]=decimal_text(row.at(field));
    }return rows;
}
Json collect(pqxx::work& tx,const std::string& book,const std::string& day,int64_t actor,
             const std::string& model,const Json& original){
    const auto b=tx.quote(book),d=tx.quote(day)+"::date";
    if(original.at("book_id")!=book||original.at("source_day")!=day)reject();
    // Retain only the immutable preview's proven lineage object. Every fact
    // supporting that proof is selected again below and compared after native
    // canonicalization; equal quantities alone never establish an origin.
    Json facts=original;
    facts["source_rows"]=positions(tx,book,day,"qt_proposal");
    facts["saved_rows"]=positions(tx,book,day,"qt");
    facts["saved_accounting"]=positions(tx,book,day,"qt",true);
    facts["system_rows"]=positions(tx,book,day,"system");
    // Explicit discriminated union. Legacy six-field references retain their
    // exact bytes; v2 is never translated into an untagged v1 publication.
    auto publications=load_qt_model_publication_scope(tx,book,day);
    if(publications.is_error())reject();
    facts["publication_refs"]=publications.value().at("references");
    if(publications.value().at("latest_publication_id")!=model)reject();
    facts["audit_refs"]=Json::array();
    auto audits=query_rows(tx,
        "SELECT jsonb_build_object('id',o.id,'user_id',o.user_id::text,'source_app',o.source_app,"
        "'strategy_id',o.strategy_id,'symbol',o.symbol,'before',o.before_state,'after',o.after_state)"
        " FROM trading.position_overrides o LEFT JOIN trading.position_override_legacy_scopes s"
        " ON s.override_id=o.id WHERE o.portfolio_id="+b+" OR s.portfolio_id="+b+" ORDER BY o.id");
    for(auto& audit:audits){
        const auto before=hash(audit.at("before")),after=hash(audit.at("after"));
        audit.erase("before");audit.erase("after");
        audit["before_digest"]=before;audit["after_digest"]=after;
        facts["audit_refs"].push_back(std::move(audit));
    }
    facts["registry"]=query_rows(tx,"SELECT jsonb_build_object('id',id,'strategy_type',strategy_type,"
        "'portfolio_id',portfolio_id,'is_active',is_active,'lifecycle',lifecycle,'updated_at',"+stamp("updated_at")+
        ") FROM trading.strategy_registry WHERE portfolio_id="+b+
        " OR id IN(SELECT strategy_id FROM trading.strategy_book_memberships WHERE portfolio_id="+b+") ORDER BY id");
    facts["memberships"]=query_rows(tx,"SELECT jsonb_build_object('strategy_id',strategy_id,'portfolio_id',portfolio_id)"
        " FROM trading.strategy_book_memberships WHERE portfolio_id="+b+" ORDER BY strategy_id,portfolio_id");
    facts["capability"]=query_one(tx,"SELECT jsonb_build_object('status','present','enabled',enabled,'version',version)"
        " FROM trading.qt_workflow_capabilities WHERE book_id="+b);
    facts["grants"]=query_rows(tx,"SELECT jsonb_build_object('user_id',user_id::text,'capability',capability,"
        "'active',active,'version',version) FROM trading.qt_action_grants WHERE user_id="+tx.quote(actor)+" ORDER BY capability");
    auto heads=query_rows(tx,"SELECT jsonb_build_object('status','present','draft_id',h.draft_id::text,"
        "'revision',h.revision,'digest',r.draft_digest) FROM trading.qt_draft_heads h JOIN trading.qt_drafts r"
        " ON (r.book_id,r.source_day,r.draft_id,r.revision)=(h.book_id,h.source_day,h.draft_id,h.revision)"
        " WHERE h.book_id="+b+" AND h.source_day="+d);
    if(heads.size()!=1)reject();
    facts["draft"]=heads[0];
    facts["risk_inventory"]=Json::array();
    auto limits=query_rows(tx,"SELECT jsonb_build_object('id',id,'strategy_id',strategy_id,"
        "'published_at',"+stamp("published_at")+",'limits',limits) FROM trading.risk_limits WHERE portfolio_id="+b+" ORDER BY id");
    for(auto& row:limits){auto digest=hash(row.at("limits"));row.erase("limits");row["content_digest"]=digest;
        facts["risk_inventory"].push_back(std::move(row));}
    auto policy=query_one(tx,"SELECT to_jsonb(p)||jsonb_build_object('updated_at',"+stamp("updated_at")+
        ") FROM trading.qt_source_policies p WHERE book_id="+b+" AND purpose='evaluation'");
    // The complete immutable evaluator closure is required authority, not an
    // optional fixture/runtime hint. Missing legacy migration/pins fail closed.
    for(auto name:{"evaluator_sha256","evaluator_bundle_sha256"}){
        const auto& value=policy.at(name);if(!value.is_string())reject();
        const auto pin=value.get<std::string>();
        if(pin.size()!=64||pin.find_first_not_of("0123456789abcdef")!=std::string::npos)reject();
    }
    auto snapshot=query_one(tx,"SELECT to_jsonb(s)||jsonb_build_object('as_of',"+stamp("as_of")+
        ",'valid_until',"+stamp("valid_until")+") FROM trading.qt_evaluation_snapshots s WHERE book_id="+b+
        " AND source_day="+d+" ORDER BY snapshot_id DESC LIMIT 1");
    if(policy.at("enabled")!=true||snapshot.at("producer_id")!=policy.at("producer_id")||
       snapshot.at("policy_version")!=policy.at("policy_version")||snapshot.at("model_publication_id")!=model)reject();
    const auto& payload=snapshot.at("payload");
    if(payload.size()!=6||(payload.at("schema_version")!="qt-inputs/v1"&&payload.at("schema_version")!="qt-inputs-empty-owner/v2")||payload.at("book_id")!=book||
       payload.at("source_day")!=day||payload.at("model_publication_id")!=model||
       hash(payload,true)!=snapshot.at("content_digest").get<std::string>())reject();
    if(payload.at("schema_version")=="qt-inputs-empty-owner/v2") {
        // Absence of rows alone is never owner authority. The immutable union
        // loader verifies actual original publication/configuration/batch seal.
        auto owner=load_qt_model_publication_record(tx,model);
        if(owner.is_error()||owner.value().kind!=QtModelPublicationKind::EmptyOwnerV2)reject();
        const auto& row=owner.value().row;
        if(row.at("portfolio_id")!=book||row.at("source_day")!=day||
           row.at("configured_owner_names").size()!=1||row.at("strategy_id")!="LIVE_EQUITY_MEAN_REVERSION"||
           !row.at("system_components").empty()||!row.at("proposal_components").empty()||!row.at("qt_components").empty())reject();
        for(auto field:{"source_rows","saved_rows","saved_accounting","system_rows"})
            if(!facts.at(field).is_array()||!facts.at(field).empty())reject();
        if(!payload.at("instrument_catalog").is_array()||!payload.at("instrument_catalog").empty())reject();
        const auto& engine=payload.at("engine_inputs");
        for(auto field:{"valuations","closes","expected_observation_times"})
            if(!engine.at("risk_inputs").at(field).is_array()||!engine.at("risk_inputs").at(field).empty())reject();
        for(auto field:{"quantity_rules","component_cost_inputs"})
            if(!engine.at(field).is_array()||!engine.at(field).empty())reject();
        if(engine.at("optimizer_policy").at("enabled")!=false||engine.contains("optimizer_inputs")||engine.contains("optimizer_config"))reject();
        auto capital=parse_qt_quantity_exact(engine.at("risk_config").at("capital_exact").get<std::string>());
        if(capital.is_error()||!capital.value().is_positive())reject();
        const auto registry_id=row.at("registry_id").get<std::string>();
        auto registry=query_one(tx,"SELECT to_jsonb(r) FROM trading.strategy_registry r WHERE id="+tx.quote(registry_id)+" FOR SHARE");
        // Live-only (the desk never admits an incubating owner); pinned in test_qt_desk_owner_scope.cpp.
        if(!qt_desk_empty_owner_registry_eligible(registry,row.at("strategy_id"),row.at("registry_revision")))reject();
        auto members=query_rows(tx,"SELECT to_jsonb(m) FROM trading.strategy_book_memberships m WHERE strategy_id="+tx.quote(registry_id)+" ORDER BY portfolio_id");
        bool member=members.empty()&&registry.at("portfolio_id")==book;
        for(const auto& m:members)if(m.at("portfolio_id")==book)member=true;
        if(!member)reject();
        const auto owner_engine=tx.quote(row.at("strategy_id").get<std::string>());
        auto metadata=query_rows(tx,"SELECT to_jsonb(m)||jsonb_build_object('source_day',to_char(m.date AT TIME ZONE 'UTC','YYYY-MM-DD')) FROM trading.live_run_metadata m WHERE portfolio_id="+b+" AND strategy_id="+owner_engine+" ORDER BY date DESC LIMIT 2");
        if(metadata.empty()||metadata[0].at("source_day")!=day||
           (metadata.size()>1&&metadata[0].at("date")==metadata[1].at("date"))||
           metadata[0].at("portfolio_config").at("config_inspection")!=row.at("inspection_capture"))reject();
        auto inputs=query_rows(tx,"SELECT to_jsonb(r) FROM trading.run_inputs r WHERE portfolio_id="+b+" AND strategy_id="+owner_engine+" AND (date AT TIME ZONE 'UTC')::date="+d+" FOR SHARE");
        if(inputs.size()!=1||inputs[0].at("trade_ngin_sha")!=row.at("producer_version")||
           hash(inputs[0].at("config_snapshot"))!=row.at("configuration_digest").get<std::string>())reject();
    }
    Json identity={{"schema_version","qt-input-authority/v1"},{"book_id",book},{"source_day",day},
        {"model_publication_id",model},{"snapshot_id",snapshot.at("snapshot_id")},
        {"source_version",snapshot.at("source_version")},{"as_of",snapshot.at("as_of")},
        {"valid_until",snapshot.at("valid_until")},{"content_digest",snapshot.at("content_digest")},
        {"producer_id",policy.at("producer_id")},{"policy_version",policy.at("policy_version")},
        {"policy_revision",policy.at("version")},{"policy_updated_at",policy.at("updated_at")},
        {"evaluator_build",policy.at("evaluator_build")},{"evaluator_sha256",policy.at("evaluator_sha256")},
        {"evaluator_bundle_sha256",policy.at("evaluator_bundle_sha256")},{"allowed_override_codes",policy.at("allowed_override_codes")}};
    std::sort(identity["allowed_override_codes"].begin(),identity["allowed_override_codes"].end());
    const auto authority=hash(identity,true);
    const auto source_id="qt_evaluation_snapshots:"+snapshot.at("snapshot_id").dump();
    facts["risk_limits"]={{"status","present"},{"id",snapshot.at("snapshot_id")},
        {"published_at",snapshot.at("as_of")},{"content_digest",snapshot.at("content_digest")}};
    facts["portfolio_inputs"]={{"status","present"},{"source_id",source_id},{"source_day",day},
        {"capital_exact",payload.at("engine_inputs").at("risk_config").at("capital_exact")},
        {"content_digest",snapshot.at("content_digest")}};
    facts["external_sources"]=Json::array();
    for(auto name:{"mark","history","cost","multiplier","universe","instrument_type","quantity_rule","evaluator_policy"})
        facts["external_sources"].push_back({{"name",name},{"status","available"},{"source_id",source_id},
            {"version",snapshot.at("source_version")},{"as_of",snapshot.at("as_of")},
            {"valid_until",snapshot.at("valid_until")},{"digest",authority},{"reason",nullptr}});
    facts["evaluator"]={{"status","available"},{"build",policy.at("evaluator_build")},
        {"policy_version",policy.at("policy_version")}};
    return facts;
}
}

Result<std::string> canonical_qt_desk_source_json(const Json& payload){
    try{std::string wire;size_t nodes=0;encode(payload,wire,false,0,nodes);return wire;}
    catch(const std::exception&){return make_error<std::string>(ErrorCode::INVALID_DATA,
        "qt_desk_source_unavailable","qt_desk_current_facts");}
}
Result<std::string> canonical_qt_desk_input_json(const Json& payload){
    try{std::string wire;size_t nodes=0;encode(payload,wire,true,0,nodes);return wire;}
    catch(const std::exception&){return make_error<std::string>(ErrorCode::INVALID_DATA,
        "qt_desk_source_unavailable","qt_desk_current_facts");}
}
Result<QtDeskReadSetCapture> capture_qt_desk_current_facts(pqxx::work& tx,const std::string& book,
    const std::string& day,int64_t actor,const std::string& model,const Json& original){
    try{
        auto normalized=canonical_qt_desk_read_set_bytes(original);if(normalized.is_error())reject();
        const auto proof=Json::parse(normalized.value());
        auto facts=collect(tx,book,day,actor,model,proof);
        const auto checked=tx.exec("SELECT "+stamp("sampled")+" FROM(SELECT clock_timestamp() sampled)c")[0][0].as<std::string>();
        auto admitted=admit_qt_desk_read_set(facts,checked);
        if(admitted.is_error()||admitted.value().payload!=proof)reject();
        return admitted;
    }catch(const std::exception&){return make_error<QtDeskReadSetCapture>(ErrorCode::INVALID_DATA,
        "qt_desk_source_unavailable","qt_desk_current_facts");}
}

Result<QtDeskReadSetCapture> capture_qt_desk_processed_facts(
    pqxx::work& tx,const Json& decision,const Json& preview,const Json& receipt){
    try{
        const auto id=decision.at("decision_id").get<std::string>();
        auto d=query_one(tx,"SELECT to_jsonb(d) FROM trading.qt_decisions d WHERE decision_id="+tx.quote(id)+"::uuid");
        auto p=query_one(tx,"SELECT to_jsonb(p) FROM trading.qt_previews p WHERE preview_id="+tx.quote(d.at("preview_id").get<std::string>())+"::uuid");
        auto r=query_one(tx,"SELECT to_jsonb(r) FROM trading.qt_desk_receipts r WHERE decision_id="+tx.quote(id)+"::uuid");
        if(d!=decision||p!=preview||r!=receipt||r.at("status")!="processed")reject();
        const auto& publication=r.at("publication_payload");
        const std::set<std::string> fields={"schema_version","decision_id","attempt_id","observation_id","book_id",
            "source_day","model_publication_id","preview_payload_digest","read_set_digest","selected_book_digest",
            "published_book_digest","observation_digest","results_digest","before_accounting","after_accounting","report_scope"};
        std::set<std::string> actual;for(auto it=publication.begin();it!=publication.end();++it)actual.insert(it.key());
        if(actual!=fields||publication.at("schema_version")!="qt-desk-publication/v1")reject();
        for(auto field:{"decision_id","book_id","source_day","model_publication_id","read_set_digest","selected_book_digest"})
            if(publication.at(field)!=d.at(field))reject();
        if(publication.at("attempt_id")!=r.at("attempt_id")||
           publication.at("preview_payload_digest")!=p.at("payload_digest")||
           publication.at("published_book_digest")!=r.at("published_book_digest")||
           publication.at("published_book_digest")!=d.at("selected_book_digest"))reject();
        const auto& original=p.at("read_set_payload");
        if(publication.at("before_accounting")!=original.at("saved_accounting"))reject();
        auto observation=query_one(tx,"SELECT to_jsonb(o) FROM trading.qt_execution_observations o WHERE observation_id="+
            tx.quote(publication.at("observation_id").get<std::string>())+"::uuid AND decision_id="+tx.quote(id)+"::uuid");
        // Executed immutable observations remain durable after their original
        // admission window. Current authority may still revoke publication
        // proof: the execution policy must exist, be enabled, and admit this
        // exact observation producer/policy identity.
        auto execution_policy=query_one(tx,"SELECT to_jsonb(p) FROM trading.qt_source_policies p WHERE book_id="+
            tx.quote(d.at("book_id").get<std::string>())+" AND purpose='execution'");
        if(execution_policy.at("enabled")!=true||
           execution_policy.at("producer_id")!=observation.at("producer_id")||
           execution_policy.at("policy_version")!=observation.at("policy_version"))reject();
        if(observation.at("content_digest")!=publication.at("observation_digest")||
           hash(observation.at("payload"),true)!=publication.at("observation_digest").get<std::string>())reject();
        const auto& observed=observation.at("payload");
        const bool accounting_producer=observed.at("schema_version")=="qt-execution/v2";
        if((observed.at("schema_version")!="qt-execution/v1"&&!accounting_producer)||observed.at("decision_id")!=id||
           observed.at("book_id")!=d.at("book_id")||observed.at("source_day")!=d.at("source_day"))reject();
        if(accounting_producer&&(observed.at("accounting_input_id")!=observation.at("observation_id")||
            verify_qt_desk_accounting_outputs(tx,d,observation.at("observation_id").get<std::string>()).is_error()))reject();
        Json observed_accounting=Json::array();
        for(const auto& fill:observed.at("fills"))observed_accounting.push_back({
            {"key",fill.at("key")},{"quantity_exact",fill.at("selected_quantity_exact")},
            {"average_price_exact",fill.at("average_price_exact")},
            {"daily_unrealized_pnl_exact",fill.at("daily_unrealized_pnl_exact")},
            {"daily_realized_pnl_exact",fill.at("daily_realized_pnl_exact")},
            {"last_update",fill.at("last_update")}});
        std::sort(observed_accounting.begin(),observed_accounting.end(),
            [](const Json& a,const Json& b){return a.at("key").dump()<b.at("key").dump();});
        if(observed_accounting!=publication.at("after_accounting"))reject();
        auto result=query_one(tx,"SELECT to_jsonb(o) FROM trading.qt_desk_results o WHERE decision_id="+tx.quote(id)+"::uuid");
        if(result.at("attempt_id")!=r.at("attempt_id")||result.at("observation_id")!=observation.at("observation_id")||
           result.at("content_digest")!=publication.at("results_digest")||
           hash(result.at("payload"),true)!=publication.at("results_digest").get<std::string>()||
           result.at("payload").at("results")!=observation.at("payload").at("results"))reject();
        const auto& output=result.at("payload");
        if(output.size()!=7||output.at("schema_version")!="qt-desk-result/v1"||
           output.at("decision_id")!=id||output.at("observation_id")!=observation.at("observation_id")||
           output.at("book_id")!=d.at("book_id")||output.at("source_day")!=d.at("source_day")||
           output.at("selected_book_digest")!=d.at("selected_book_digest"))reject();
        auto facts=collect(tx,d.at("book_id").get<std::string>(),d.at("source_day").get<std::string>(),d.at("created_by").get<int64_t>(),
                           d.at("model_publication_id").get<std::string>(),original);
        const auto checked=tx.exec("SELECT "+stamp("sampled")+" FROM(SELECT clock_timestamp() sampled) c")[0][0].as<std::string>();
        auto expected_accounting=publication.at("after_accounting");
        if(accounting_producer){auto successor=verified_qt_desk_finalization(tx,d);if(successor.is_error())reject();if(!successor.value().is_null()){expected_accounting=successor.value().at("positions");std::sort(expected_accounting.begin(),expected_accounting.end(),[](const Json& a,const Json& b){return a.at("key").dump()<b.at("key").dump();});}}
        if(facts.at("saved_accounting")!=expected_accounting)reject();
        Json selected=Json::array();
        for(const auto& row:publication.at("after_accounting"))
            selected.push_back({{"key",row.at("key")},{"quantity_exact",row.at("quantity_exact")}});
        auto digest=qt_digest_v1({{"selection_rows",selected}});
        if(digest.is_error()||digest.value()!=publication.at("published_book_digest").get<std::string>())reject();
        Json expected_ref={{"schema_version","qt-desk-audit/v1"},{"decision_id",id},{"attempt_id",r.at("attempt_id")},
            {"preview_id",p.at("preview_id")},{"preview_payload_digest",p.at("payload_digest")},
            {"read_set_digest",d.at("read_set_digest")},{"selected_book_digest",d.at("selected_book_digest")},
            {"published_book_digest",r.at("published_book_digest")},{"observation_digest",observation.at("content_digest")}};
        auto audits=query_rows(tx,"SELECT jsonb_build_object('id',id,'user_id',user_id,'source_app',source_app,"
            "'before',before_state,'after',after_state,'reference',risk_check_result)"
            " FROM trading.position_overrides WHERE portfolio_id="+tx.quote(d.at("book_id").get<std::string>())+" ORDER BY id");
        std::set<int64_t> original_ids;
        for(const auto& ref:original.at("audit_refs"))original_ids.insert(ref.at("id").get<int64_t>());
        std::set<std::string> covered;std::set<int64_t> addition_ids;
        auto flat=[](const Json& row){
            Json value=row.at("key");value["quantity_exact"]=row.at("quantity_exact");
            value["average_price_exact"]=row.at("average_price_exact");return value;
        };
        for(const auto& audit:audits){
            auto audit_id=audit.at("id").get<int64_t>();if(original_ids.contains(audit_id))continue;
            if(audit.at("reference")!=expected_ref||audit.at("source_app")!="algolens"||
               audit.at("user_id")!=d.at("created_by"))reject();
            const Json* after=nullptr;Json before=nullptr;
            for(const auto& row:publication.at("after_accounting"))if(flat(row)==audit.at("after"))after=&row;
            if(!after||!covered.insert(after->at("key").dump()).second)reject();
            for(const auto& row:publication.at("before_accounting"))
                if(row.at("key")==after->at("key"))before=flat(row);
            if(audit.at("before")!=before)reject();
            addition_ids.insert(audit_id);
        }
        if(covered.size()!=publication.at("after_accounting").size())reject();
        Json refs=Json::array();for(const auto& ref:facts.at("audit_refs"))
            if(!addition_ids.contains(ref.at("id").get<int64_t>()))refs.push_back(ref);
        // Restore only receipt-proven changes to compare the original hash.
        // Current SQL after-accounting and every added audit were checked above.
        facts["audit_refs"]=refs;facts["saved_rows"]=original.at("saved_rows");
        facts["saved_accounting"]=original.at("saved_accounting");
        auto admission=admit_qt_desk_read_set(facts,checked);
        auto prior=canonical_qt_desk_read_set_bytes(original);
        if(admission.is_error()||prior.is_error()||admission.value().payload!=Json::parse(prior.value())||
           admission.value().digest!=d.at("read_set_digest").get<std::string>())reject();
        return admission;
    }catch(const std::exception&){return make_error<QtDeskReadSetCapture>(ErrorCode::INVALID_DATA,
        "qt_report_snapshot_stale","qt_desk_current_facts");}
}
} // namespace trade_ngin
