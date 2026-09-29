#include "trade_ngin/data/qt_desk_processor.hpp"
#include "trade_ngin/data/qt_desk_accounting.hpp"
#include "trade_ngin/data/qt_desk_upstream.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/data/qt_desk_owner_scope.hpp"
#include "trade_ngin/data/qt_desk_publication.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/apps/qt_report_quantity_projection.hpp"
#include "trade_ngin/apps/qt_empty_owner_report.hpp"
#include "trade_ngin/data/qt_model_publication.hpp"
#include "trade_ngin/data/qt_empty_model_owner_publication.hpp"
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include <array>
#include <sstream>
#include <iomanip>
#include <set>
#include <map>
#include <limits>
#include <ctime>
#include <algorithm>
#include <stdexcept>

namespace trade_ngin {
namespace {
using Json=nlohmann::json;
[[noreturn]] void refuse(){throw std::invalid_argument("qt_desk_unavailable");}
void require(bool predicate){if(!predicate)refuse();}
void shape(const Json& value,std::initializer_list<const char*> names){
    require(value.is_object());std::set<std::string> fields,wanted;
    for(auto it=value.begin();it!=value.end();++it)fields.insert(it.key());
    for(auto name:names)wanted.insert(name);
    require(fields==wanted);
}
std::string text(const Json& value){require(value.is_string());auto s=value.get<std::string>();
    require(!s.empty()&&s.find_first_not_of(" \t\r\n")!=std::string::npos&&s.size()<=16384);return s;}
void uuid(const std::string& s){
    require(s.size()==36);for(size_t i=0;i<s.size();++i)
        require(i==8||i==13||i==18||i==23?s[i]=='-':std::string("0123456789abcdef").find(s[i])!=std::string::npos);
}
Json rows(pqxx::work& tx,const std::string& q){
    Json out=Json::array();for(auto row:tx.exec(q)){require(!row[0].is_null());out.push_back(Json::parse(row[0].c_str()));}return out;
}
Json one(pqxx::work& tx,const std::string& q){auto r=rows(tx,q);require(r.size()==1);return r[0];}
std::string time_sql(const std::string& e){return "CASE WHEN "+e+" IS NULL THEN NULL ELSE "
    "to_char("+e+" AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS') || CASE WHEN extract(microseconds FROM "+
    e+")::bigint%1000000=0 THEN '' ELSE to_char("+e+" AT TIME ZONE 'UTC','.US') END || 'Z' END";}
std::string clock(pqxx::work& tx){return tx.exec("SELECT "+time_sql("sampled")+
    " FROM(SELECT clock_timestamp() sampled)c")[0][0].as<std::string>();}
std::string digest(const Json& value){auto bytes=canonical_qt_desk_input_json(value);require(bytes.is_ok());
    auto hash=qt_sha256_hex(bytes.value());require(hash.is_ok());return hash.value();}
Quantity number(const Json& value){auto p=parse_qt_quantity_exact(text(value));require(p.is_ok());return p.value();}
int64_t add(int64_t a,int64_t b){
    require(!((b>0&&a>INT64_MAX-b)||(b<0&&a<INT64_MIN-b)));return a+b;
}
std::string key_id(const Json& key){
    shape(key,{"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"});
    return key.dump();
}
bool role(const Json& user){return user.at("role")=="admin"||user.at("role")=="general_member";}
Json account(pqxx::work& tx,int64_t user){
    return one(tx,"SELECT jsonb_build_object('id',id,'role',role) FROM auth.users WHERE id="+tx.quote(user));
}
Json grant(pqxx::work& tx,int64_t user,const std::string& capability){
    return one(tx,"SELECT to_jsonb(g) FROM trading.qt_action_grants g WHERE user_id="+tx.quote(user)+
        " AND capability="+tx.quote(capability));
}
Json approvals(pqxx::work& tx,const std::string& id){
    return rows(tx,"SELECT to_jsonb(a) FROM trading.qt_override_approvals a JOIN trading.qt_override_requests r"
        " ON r.request_id=a.request_id WHERE r.decision_id="+tx.quote(id)+"::uuid ORDER BY a.user_id,a.person_id");
}
Json decision_row(pqxx::work& tx,const std::string& id){
    return one(tx,"SELECT to_jsonb(d) FROM trading.qt_decisions d WHERE decision_id="+tx.quote(id)+"::uuid");
}
void lock_book(pqxx::work& tx,const std::string& book){
    tx.exec("SELECT pg_advisory_xact_lock(hashtextextended('algolens:qt-book:'||upper(btrim("+tx.quote(book)+")),0))");
}
Json lock_decision(pqxx::work& tx,const std::string& id){
    auto before=decision_row(tx,id);const auto book=text(before.at("book_id"));
    require(book.find_first_of(" \t\r\n")==std::string::npos);
    auto initial=approvals(tx,id);std::set<int64_t> users{before.at("created_by").get<int64_t>()};
    for(const auto& item:initial)users.insert(item.at("user_id").get<int64_t>());
    for(auto user:users){
        require(user>0);require(tx.exec("SELECT id FROM auth.users WHERE id="+tx.quote(user)+" FOR UPDATE").size()==1);
        tx.exec("SELECT user_id FROM trading.qt_action_grants WHERE user_id="+tx.quote(user)+" ORDER BY capability FOR UPDATE");
        tx.exec("SELECT person_id FROM trading.qt_approver_allowlist WHERE user_id="+tx.quote(user)+" ORDER BY person_id FOR UPDATE");
    }
    const auto registries="SELECT id FROM trading.strategy_registry WHERE portfolio_id="+tx.quote(book)+
        " OR id IN(SELECT strategy_id FROM trading.strategy_book_memberships WHERE portfolio_id="+tx.quote(book)+") ORDER BY id";
    auto ids=tx.exec(registries);for(auto row:ids)
        require(tx.exec("SELECT id FROM trading.strategy_registry WHERE id="+tx.quote(row[0].as<std::string>())+" FOR UPDATE").size()==1);
    lock_book(tx,book);
    require(tx.exec("SELECT draft_id FROM trading.qt_draft_heads WHERE book_id="+tx.quote(book)+
        " AND source_day="+tx.quote(text(before.at("source_day")))+"::date FOR UPDATE").size()==1);
    tx.exec("SELECT preview_id FROM trading.qt_previews WHERE preview_id="+tx.quote(text(before.at("preview_id")))+"::uuid FOR UPDATE");
    tx.exec("SELECT decision_id FROM trading.qt_decisions WHERE decision_id="+tx.quote(id)+"::uuid FOR UPDATE");
    tx.exec("SELECT request_id FROM trading.qt_override_requests WHERE decision_id="+tx.quote(id)+"::uuid FOR UPDATE");
    tx.exec("SELECT a.approval_id FROM trading.qt_override_approvals a JOIN trading.qt_override_requests r ON r.request_id=a.request_id"
        " WHERE r.decision_id="+tx.quote(id)+"::uuid ORDER BY a.approval_id FOR UPDATE OF a");
    tx.exec("SELECT decision_id FROM trading.qt_desk_receipts WHERE decision_id="+tx.quote(id)+"::uuid FOR UPDATE");
    auto current=decision_row(tx,id);require(current==before&&approvals(tx,id)==initial);
    auto nowids=tx.exec(registries);require(nowids.size()==ids.size());
    for(pqxx::result::size_type i=0;i<ids.size();++i)require(nowids[i][0].as<std::string>()==ids[i][0].as<std::string>());
    return current;
}
Json preview_row(pqxx::work& tx,const Json& d){
    return one(tx,"SELECT to_jsonb(p) FROM trading.qt_previews p WHERE preview_id="+tx.quote(text(d.at("preview_id")))+"::uuid");
}
Json policy(pqxx::work& tx,const std::string& book,const std::string& purpose){
    return one(tx,"SELECT to_jsonb(p) FROM trading.qt_source_policies p WHERE book_id="+tx.quote(book)+" AND purpose="+tx.quote(purpose));
}
void authorize(pqxx::work& tx,const Json& d,const Json& p){
    auto actor=d.at("created_by").get<int64_t>();require(role(account(tx,actor)));
    auto g=grant(tx,actor,"qt_submit");
    require(g.at("active")==true&&g.at("version")==d.at("submitter_grant_version"));
    auto cap=one(tx,"SELECT to_jsonb(c) FROM trading.qt_workflow_capabilities c WHERE book_id="+tx.quote(text(d.at("book_id"))));
    require(cap.at("enabled")==true&&cap.at("version")==d.at("workflow_capability_version"));
    auto ep=policy(tx,text(d.at("book_id")),"evaluation");
    require(ep.at("enabled")==true&&ep.at("policy_version")==d.at("policy_version"));
    if(!p.at("payload").at("requires_override").get<bool>())return;
    const std::set<std::string> known={"portfolio_var","jump_risk","correlation","gross_leverage","net_leverage"};
    std::set<std::string> allowed;for(auto& code:ep.at("allowed_override_codes"))allowed.insert(text(code));
    const auto& breaches=p.at("payload").at("evaluation").at("selected_risk").at("breaches");
    require(!breaches.empty());for(const auto& breach:breaches){
        auto code=text(breach.at("code"));require(known.contains(code)&&allowed.contains(code));
    }
    auto request=one(tx,"SELECT to_jsonb(r) FROM trading.qt_override_requests r WHERE decision_id="+tx.quote(text(d.at("decision_id")))+"::uuid");
    require(request.at("required_approvals")==2&&request.at("eligibility_version")==ep.at("version"));
    const std::set<std::string> canonical={"eric_shwartz","john_riley","xander_robbins","hemdutt_rao","dominick_dupuoy"};
    std::set<std::string> people;std::set<int64_t> users;
    for(const auto& approval:approvals(tx,text(d.at("decision_id")))){
        const auto person=text(approval.at("person_id"));const auto user=approval.at("user_id").get<int64_t>();
        auto mappings=rows(tx,"SELECT to_jsonb(m) FROM trading.qt_approver_allowlist m WHERE person_id="+tx.quote(person));
        auto grants=rows(tx,"SELECT to_jsonb(g) FROM trading.qt_action_grants g WHERE user_id="+tx.quote(user)+" AND capability='qt_approve'");
        if(!canonical.contains(person)||mappings.size()!=1||grants.size()!=1||!role(account(tx,user)))continue;
        const auto& m=mappings[0];const auto& ag=grants[0];
        if(m.at("active")!=true||m.at("user_id")!=user||m.at("mapping_version")!=approval.at("mapping_version")||
            ag.at("active")!=true||ag.at("version")!=approval.at("grant_version"))continue;
        require(people.insert(person).second&&users.insert(user).second);
    }require(people.size()>=2&&users.size()>=2);
}
Json observation_row(pqxx::work& tx,const Json& d,const std::string& id){
    auto o=one(tx,"SELECT to_jsonb(o)||jsonb_build_object('as_of',"+time_sql("as_of")+
        ",'valid_until',"+time_sql("valid_until")+") FROM trading.qt_execution_observations o WHERE observation_id="+tx.quote(id)+"::uuid");
    auto ep=policy(tx,text(d.at("book_id")),"execution");
    require(o.at("decision_id")==d.at("decision_id")&&ep.at("enabled")==true&&
        o.at("producer_id")==ep.at("producer_id")&&o.at("policy_version")==ep.at("policy_version"));
    text(o.at("source_version"));text(o.at("producer_id"));text(o.at("policy_version"));
    // PostgreSQL compares true timestamps, independent of textual fractional lengths.
    auto valid=tx.exec("SELECT as_of<=clock_timestamp() AND clock_timestamp()<=valid_until FROM trading.qt_execution_observations"
        " WHERE observation_id="+tx.quote(id)+"::uuid");
    require(valid.size()==1&&valid[0][0].as<bool>()&&digest(o.at("payload"))==o.at("content_digest").get<std::string>());
    return o;
}
void validate_owner_scope(pqxx::work& tx,const Json& d,const Json& p,const Json& facts){
    const auto& registry=facts.at("registry");const auto& memberships=facts.at("memberships");
    auto snapshot=one(tx,"SELECT to_jsonb(s) FROM trading.qt_evaluation_snapshots s WHERE book_id="+tx.quote(text(d.at("book_id")))+
        " AND source_day="+tx.quote(text(d.at("source_day")))+"::date ORDER BY snapshot_id DESC LIMIT 1");
    std::map<std::string,Json> catalog;
    for(const auto& row:snapshot.at("payload").at("instrument_catalog")){
        shape(row,{"key","instrument_type","editable"});require(catalog.emplace(key_id(row.at("key")),row).second);
    }
    std::set<std::string> actual;
    std::map<std::string,std::set<std::string>> owner_names;
    for(const auto& row:facts.at("source_rows"))
        owner_names[text(row.at("key").at("strategy_id"))].insert(text(row.at("key").at("strategy_name")));
    for(auto field:{"source_rows","saved_rows"})for(const auto& row:facts.at(field)){
        auto key=row.at("key");key["portfolio_type"]="qt_proposal";actual.insert(key_id(key));
    }
    require(catalog.size()==p.at("payload").at("selection_rows").size());
    auto draft=one(tx,"SELECT to_jsonb(r) FROM trading.qt_drafts r WHERE draft_id="+
        tx.quote(text(d.at("draft_id")))+"::uuid");
    std::set<std::string> selected;
    for(const auto& row:p.at("payload").at("selection_rows")){
        const auto& key=row.at("key");
        // Governed evaluator identities use qt_proposal even for separately
        // held immutable QT rows. Preserve the physical selection/fill key;
        // normalize only the catalog and complete authority-set lookup.
        auto catalog_key=key;catalog_key["portfolio_type"]="qt_proposal";
        auto id=key_id(catalog_key);require(selected.insert(id).second&&catalog.contains(id));
        const auto& authority=catalog.at(id);
        require(authority.at("instrument_type")==row.at("asset_type")&&authority.at("editable")==row.at("editable"));
        if(!actual.contains(id)){
            // A4 permits a new key only under one established component owner.
            // The immutable draft and governed typed catalog must both admit
            // its unfilled state; no physical before/zero row is fabricated.
            require(row.at("editable")==true&&row.at("origin")=="qt_draft"&&
                row.at("basis_status")=="unfilled"&&row.at("average_price_exact").is_null());
            require(owner_names[text(key.at("strategy_id"))]==std::set<std::string>{text(key.at("strategy_name"))});
            bool drafted=false;
            for(const auto& saved:draft.at("selection_payload").at("selection_rows")){
                if(saved.at("key")!=key)continue;
                drafted=true;
                for(auto field:{"asset_type","editable","origin","basis_status","average_price_exact"})
                    require(saved.at(field)==row.at(field));
            }
            require(drafted);
        }
        if(row.at("editable")==false)continue;
        require(qt_desk_owner_authorized(registry,memberships,
            text(key.at("strategy_id")),text(d.at("book_id"))));
    }
    for(const auto& key:actual)require(selected.contains(key));
}

Json flat(const Json& row){Json value=row.at("key");value["quantity_exact"]=row.at("quantity_exact");
    value["average_price_exact"]=row.at("average_price_exact");return value;}
Json fills(const Json& d,const Json& p,const Json& observation,const Json& before,Json& totals,bool produced=false,const Json& accounting=Json()){
    const auto& payload=observation.at("payload");
    if(produced){
        shape(payload,{"schema_version","decision_id","book_id","source_day","fills","results","accounting_input_id"});
        require(payload.at("schema_version")=="qt-execution/v2"&&payload.at("accounting_input_id")==observation.at("observation_id"));
        require(accounting.at("observation")==payload);
    }else{
        shape(payload,{"schema_version","decision_id","book_id","source_day","fills","results"});
        require(payload.at("schema_version")=="qt-execution/v1");
    }
    for(auto name:{"decision_id","book_id","source_day"})require(payload.at(name)==d.at(name));
    require(payload.at("fills").is_array()&&payload.at("fills").size()<=4096);
    std::map<std::string,Json> selected,previous;
    for(const auto& row:p.at("payload").at("selection_rows")){
        auto key=row.at("key");key["portfolio_type"]="qt";require(selected.emplace(key_id(key),row).second);
    }
    for(const auto& row:before)require(previous.emplace(key_id(row.at("key")),row).second);
    require(selected.size()==payload.at("fills").size());
    std::set<std::string> seen;std::map<std::string,std::array<int64_t,3>> sums;
    if(produced&&accounting.at("schema_version")=="qt-equity-accounting-empty-owner/v2"){
        require(selected.empty()&&previous.empty()&&payload.at("fills").empty()&&accounting.at("executions").empty());
        require(accounting.at("live_results").size()==1&&accounting.at("live_results")[0].at("strategy_id")=="LIVE_EQUITY_MEAN_REVERSION");
        const auto& totals=payload.at("results").at("currency_totals");require(totals.is_array()&&totals.size()==1);
        auto currency=text(totals[0].at("currency"));require(currency=="USD");sums.emplace(currency,std::array<int64_t,3>{0,0,0});
    }
    Json after=Json::array();
    for(const auto& fill:payload.at("fills")){
        shape(fill,{"key","observation_kind","selected_quantity_exact","average_price_exact","actual_cash_cost_exact",
            "currency","execution_id","accounting_source_id","daily_unrealized_pnl_exact","daily_realized_pnl_exact","last_update"});
        auto identity=key_id(fill.at("key"));require(seen.insert(identity).second&&selected.contains(identity));
        require(fill.at("key").at("portfolio_type")=="qt"&&fill.at("key").at("portfolio_id")==d.at("book_id")&&
            fill.at("key").at("date")==d.at("source_day"));
        const auto& choice=selected.at(identity);auto qty=number(fill.at("selected_quantity_exact"));
        auto basis=number(fill.at("average_price_exact"));require(qty==number(choice.at("quantity_exact")));
        const auto cost=number(fill.at("actual_cash_cost_exact"));require(cost.raw_value()>=0);
        auto unreal=number(fill.at("daily_unrealized_pnl_exact")),real=number(fill.at("daily_realized_pnl_exact"));
        auto currency=text(fill.at("currency"));text(fill.at("accounting_source_id"));text(fill.at("last_update"));
        auto kind=text(fill.at("observation_kind"));
        const bool unchanged=previous.contains(identity)&&previous.at(identity).at("quantity_exact")==fill.at("selected_quantity_exact")&&
            previous.at(identity).at("average_price_exact")==fill.at("average_price_exact");
        if(kind=="carried")require((unchanged||produced)&&fill.at("execution_id").is_null()&&cost.is_zero());
        else{require(kind=="executed");text(fill.at("execution_id"));}
        if(choice.at("editable")==false) {
            bool equity_action_carry=false;
            if(!unchanged&&produced&&accounting.at("schema_version")=="qt-equity-accounting/v1"&&
               choice.at("asset_type")=="EQUITY"&&previous.contains(identity)&&
               previous.at(identity).at("quantity_exact")==fill.at("selected_quantity_exact")) {
                const Json* distance=nullptr;
                for(const auto& row:accounting.at("distance"))if(row.at("key")==fill.at("key")) {
                    require(distance==nullptr);distance=&row;
                }
                require(distance!=nullptr&&distance->at("execution_delta_exact")=="0"&&
                    distance->at("restated_previous_quantity_exact")==fill.at("selected_quantity_exact"));
                const Json *first=nullptr,*last=nullptr;
                for(const auto& action:accounting.at("corporate_action_adjustments"))if(action.at("key")==fill.at("key")) {
                    if(first==nullptr)first=&action;
                    if(last!=nullptr)require(last->at("average_price_after_exact")==action.at("average_price_before_exact"));
                    last=&action;
                }
                equity_action_carry=first!=nullptr&&
                    first->at("average_price_before_exact")==previous.at(identity).at("average_price_exact")&&
                    last->at("average_price_after_exact")==fill.at("average_price_exact");
            }
            require(kind=="carried"&&(unchanged||equity_action_carry));
        }
        Json row={{"key",fill.at("key")},{"quantity_exact",qty.to_string()},{"average_price_exact",basis.to_string()},
            {"daily_unrealized_pnl_exact",unreal.to_string()},{"daily_realized_pnl_exact",real.to_string()},{"last_update",fill.at("last_update")}};
        after.push_back(std::move(row));
        auto& sum=sums[currency];sum[0]=add(sum[0],cost.raw_value());sum[1]=add(sum[1],unreal.raw_value());sum[2]=add(sum[2],real.raw_value());
    }
    std::sort(after.begin(),after.end(),[](const Json& a,const Json& b){return a.at("key").dump()<b.at("key").dump();});
    Json currencies=Json::array();for(const auto& [currency,sum]:sums)
        currencies.push_back({{"currency",currency},{"actual_cash_cost_exact",Quantity::from_raw(sum[0]).to_string()},
            {"daily_unrealized_pnl_exact",Quantity::from_raw(sum[1]).to_string()},{"daily_realized_pnl_exact",Quantity::from_raw(sum[2]).to_string()}});
    totals={{"position_count",after.size()},{"currency_totals",currencies}};require(payload.at("results")==totals);
    return after;
}
ComponentPositionKey component_key(const Json& k){return{text(k.at("portfolio_id")),text(k.at("strategy_id")),
    text(k.at("strategy_name")),text(k.at("date")),text(k.at("symbol")),text(k.at("portfolio_type"))};}
Timestamp timestamp(const Json& value){
    const auto s=text(value);require(s.size()>=20&&s.back()=='Z');
    std::tm tm{};std::istringstream input(s.substr(0,19));input>>std::get_time(&tm,"%Y-%m-%dT%H:%M:%S");require(!input.fail());
    int64_t micros=0;if(s.size()>20){require(s[19]=='.'&&s.size()<=27);auto part=s.substr(20,s.size()-21);
        require(!part.empty()&&part.find_first_not_of("0123456789")==std::string::npos);
        while(part.size()<6)part+='0';
        micros=std::stoll(part);}
    const auto seconds=timegm(&tm);require(seconds>=0);
    return std::chrono::system_clock::from_time_t(seconds)+std::chrono::microseconds(micros);
}
Json empty_owner_document(pqxx::work& tx,const Json& d){
    auto model=load_qt_model_publication_record(tx,text(d.at("model_publication_id")));
    require(model.is_ok()&&model.value().kind==QtModelPublicationKind::EmptyOwnerV2);
    const auto& row=model.value().row;
    require(row.at("portfolio_id")==d.at("book_id")&&row.at("source_day")==d.at("source_day")&&row.at("strategy_id")=="LIVE_EQUITY_MEAN_REVERSION"&&row.at("configured_owner_names").size()==1);
    Json document={{"schema_version","qt-empty-model-owner-publication/v2"},{"book_id",row.at("portfolio_id")}};
    for(auto f:{"publication_id","strategy_id","source_day","configured_owner_names","configuration_digest","system_components","seed_digest","proposal_components","proposal_manifest_digest","qt_components","qt_digest"})document[f]=row.at(f);
    for(auto f:{"system_components","proposal_components","qt_components"})require(document.at(f).is_array()&&document.at(f).empty());
    require(canonical_qt_empty_model_owner_bytes(document).is_ok());return document;
}
QtReportEligibility report(const Json& before,const Json& after,const Json& preview,
    const std::string& id,const std::string& published,Json& scope,const Json* empty_owner=nullptr){
    scope=nullptr;QtReportEligibility unavailable{"unavailable",{"qt_report_row_mapping_changed"},std::nullopt,std::nullopt};
    try{
        if(empty_owner){
            require(before.is_array()&&before.empty()&&after.is_array()&&after.empty()&&preview.at("payload").at("selection_rows").empty());
            const auto name=text(empty_owner->at("configured_owner_names")[0]),book=text(empty_owner->at("book_id")),day=text(empty_owner->at("source_day"));
            scope={{"portfolio_id",book},{"strategy_id","LIVE_EQUITY_MEAN_REVERSION"},{"strategy_names",empty_owner->at("configured_owner_names")},{"portfolio_type","qt"},{"date",day}};
            ReportPositionSnapshot snapshot{{{name,{}}},{},book,"LIVE_EQUITY_MEAN_REVERSION",{name},"qt",timestamp(day+"T00:00:00Z"),{{name,0}}};
            return build_qt_empty_owner_report_quantity_projection(*empty_owner,snapshot,id,published);
        }
        // Saved keys cover every before key (checked by the projection); a saved key with no before
        // row is a newly opened position and gets its own row, so the scope comes from both sides.
        if(after.empty())return unavailable;
        std::set<std::string> ids,names;
        for(const auto* side:{&before,&after})for(auto& row:*side){
            ids.insert(text(row.at("key").at("strategy_id")));names.insert(text(row.at("key").at("strategy_name")));}
        if(ids.size()!=1)return unavailable;
        auto book=text(after[0].at("key").at("portfolio_id")),day=text(after[0].at("key").at("date"));
        scope={{"portfolio_id",book},{"strategy_id",*ids.begin()},{"strategy_names",names},{"portfolio_type","qt"},{"date",day}};
        std::map<std::string,AssetType> types;
        for(auto& row:preview.at("payload").at("selection_rows")){
            auto k=row.at("key");k["portfolio_type"]="qt";types.emplace(key_id(k),row.at("asset_type")=="EQUITY"?AssetType::EQUITY:AssetType::FUTURE);
        }
        auto candidates=[&](const Json& rows){
            std::vector<ComponentPositionCandidate> result;
            for(auto& row:rows){
                auto k=component_key(row.at("key"));require(types.contains(key_id(row.at("key"))));
                Position position{k.symbol,number(row.at("quantity_exact")),number(row.at("average_price_exact")),
                    number(row.at("daily_unrealized_pnl_exact")),number(row.at("daily_realized_pnl_exact")),timestamp(row.at("last_update"))};
                result.push_back({k,{types.at(key_id(row.at("key"))),k.symbol},true,std::nullopt,position,false});
            }return result;
        };
        auto old=candidates(before),saved=candidates(after);
        const auto snapshot=build_qt_saved_report_snapshot(old,saved,{names.begin(),names.end()},book,*ids.begin(),timestamp(day+"T00:00:00Z"));
        return build_qt_report_quantity_projection(old,saved,snapshot,id,published);
    }catch(const std::exception&){return unavailable;}
}
void store_positions(pqxx::work& tx,const Json& before,const Json& after,const Json& d,const Json& reference){
    std::map<std::string,Json> old;for(auto& row:before)old.emplace(key_id(row.at("key")),flat(row));
    for(const auto& row:after){
        const auto& k=row.at("key");auto name=key_id(k);
        const auto fields="portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type";
        std::string values;for(auto field:{"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"}){
            if(!values.empty())values+=',';
            values+=tx.quote(text(k.at(field)));
        }
        for(auto field:{"quantity_exact","average_price_exact","daily_unrealized_pnl_exact","daily_realized_pnl_exact","last_update"})
            values+=","+tx.quote(text(row.at(field)));
        tx.exec("INSERT INTO trading.positions("+std::string(fields)+
            ",quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update) VALUES("+values+
            ") ON CONFLICT("+fields+") DO UPDATE SET quantity=EXCLUDED.quantity,average_price=EXCLUDED.average_price,"
            "daily_unrealized_pnl=EXCLUDED.daily_unrealized_pnl,daily_realized_pnl=EXCLUDED.daily_realized_pnl,last_update=EXCLUDED.last_update,"
            "updated_at=clock_timestamp()");
        tx.exec("INSERT INTO trading.position_overrides(user_id,source_app,portfolio_id,strategy_id,symbol,"
            "before_state,after_state,reason,risk_check_result,overrode_risk) VALUES("+
            tx.quote(d.at("created_by").get<int64_t>())+",'algolens',"+tx.quote(text(k.at("portfolio_id")))+","+
            tx.quote(text(k.at("strategy_id")))+","+tx.quote(text(k.at("symbol")))+","+
            tx.quote(old.contains(name)?old.at(name).dump():std::string("null"))+"::jsonb,"+
            tx.quote(flat(row).dump())+"::jsonb,'confirmed QT decision publication',"+tx.quote(reference.dump())+"::jsonb,"+
            (d.at("_override").get<bool>()?"true":"false")+")");
    }
}
} // namespace

static Result<QtDeskProcessedReceipt> process_qt_desk_impl(pqxx::connection& connection,const std::string& id,
    const std::string& attempt,const std::string& observation_id,bool produce,const std::string& market_id="",const std::string& final_id=""){
    const char* stage="identity";
    try{
        uuid(id);uuid(attempt);uuid(observation_id);pqxx::work tx(connection);
        stage="locks";auto d=lock_decision(tx,id),p=preview_row(tx,d);
        stage="immutable_selection";auto selection=validate_qt_desk_decision_snapshot(d,p);require(selection.is_ok());
        stage="authorization";authorize(tx,d,p);
        auto existing=rows(tx,"SELECT to_jsonb(r) FROM trading.qt_desk_receipts r WHERE decision_id="+tx.quote(id)+"::uuid");
        if(!existing.empty()){
            require(existing.size()==1&&existing[0].at("status")=="processed"&&existing[0].at("attempt_id")==attempt&&
                existing[0].at("publication_payload").at("observation_id")==observation_id);
            auto current=capture_qt_desk_processed_facts(tx,d,p,existing[0]);require(current.is_ok());
            if(!market_id.empty()){auto input=one(tx,"SELECT payload FROM trading.qt_desk_accounting_inputs WHERE input_id="+tx.quote(observation_id)+"::uuid");require(input.at("market_source_id")==market_id&&input.at("prior_finalization_source_id")==final_id);}
            if(produce){stage="accounting_replay";require(verify_qt_desk_accounting_outputs(tx,d,observation_id).is_ok());}
            auto payload=existing[0].at("publication_payload");tx.commit();
            return QtDeskProcessedReceipt{id,attempt,selection.value().selected_book_digest,payload,true};
        }
        require(d.at("model_publication_id").is_string());
        stage="current_facts";auto capture=capture_qt_desk_current_facts(tx,text(d.at("book_id")),text(d.at("source_day")),
            d.at("created_by").get<int64_t>(),text(d.at("model_publication_id")),p.at("read_set_payload"));
        require(capture.is_ok()&&capture.value().digest==d.at("read_set_digest").get<std::string>());
        stage="draft";auto draft=one(tx,"SELECT to_jsonb(r) FROM trading.qt_drafts r WHERE draft_id="+tx.quote(text(d.at("draft_id")))+"::uuid");
        for(auto field:{"source_digest","provenance_digest","draft_digest"})require(draft.at(field)==p.at(field));
        require(draft.at("revision")==d.at("draft_revision")&&draft.at("model_publication_id")==d.at("model_publication_id"));
        auto dd=qt_digest_v1(draft.at("selection_payload"));require(dd.is_ok()&&dd.value()==draft.at("draft_digest").get<std::string>());
        stage="owner_scope";validate_owner_scope(tx,d,p,capture.value().payload);
        if(!market_id.empty()){stage="upstream_input";require(assemble_qt_desk_accounting_input(tx,d,p.at("payload").at("selection_rows"),observation_id,market_id,final_id).is_ok());}
        Json accounting;
        if(produce){stage="accounting_producer";auto generated=prepare_qt_desk_accounting(tx,d,p,observation_id,capture.value().payload);
            require(generated.is_ok());accounting=generated.value();}
        stage="observation";auto observation=observation_row(tx,d,observation_id);
        const auto before=capture.value().payload.at("saved_accounting");
        stage="fills";Json results;auto after=fills(d,p,observation,before,results,produce,accounting);
        // The saved complete accounting book must still satisfy exact native types.
        auto shape_probe=capture.value().payload;shape_probe["saved_accounting"]=after;shape_probe["saved_rows"]=Json::array();
        for(auto& row:after)shape_probe["saved_rows"].push_back({{"key",row.at("key")},{"quantity_exact",row.at("quantity_exact")},{"average_price_exact",row.at("average_price_exact")}});
        stage="accounting";require(admit_qt_desk_read_set(shape_probe,clock(tx)).is_ok());
        auto published=selection.value().selected_book_digest;
        Json result={{"schema_version","qt-desk-result/v1"},{"decision_id",id},{"observation_id",observation_id},
            {"book_id",d.at("book_id")},{"source_day",d.at("source_day")},{"selected_book_digest",published},{"results",results}};
        auto result_digest=digest(result);
        Json scope,empty_owner;
        if(produce&&accounting.at("schema_version")=="qt-equity-accounting-empty-owner/v2")empty_owner=empty_owner_document(tx,d);
        auto eligibility=report(before,after,p,id,published,scope,empty_owner.is_null()?nullptr:&empty_owner);
        Json publication={{"schema_version","qt-desk-publication/v1"},{"decision_id",id},{"attempt_id",attempt},
            {"observation_id",observation_id},{"book_id",d.at("book_id")},{"source_day",d.at("source_day")},
            {"model_publication_id",d.at("model_publication_id")},{"preview_payload_digest",p.at("payload_digest")},
            {"read_set_digest",d.at("read_set_digest")},{"selected_book_digest",published},{"published_book_digest",published},
            {"observation_digest",observation.at("content_digest")},{"results_digest",result_digest},
            {"before_accounting",before},{"after_accounting",after},{"report_scope",scope}};
        Json reference={{"schema_version","qt-desk-audit/v1"},{"decision_id",id},{"attempt_id",attempt},
            {"preview_id",d.at("preview_id")},{"preview_payload_digest",p.at("payload_digest")},
            {"read_set_digest",d.at("read_set_digest")},{"selected_book_digest",published},
            {"published_book_digest",published},{"observation_digest",observation.at("content_digest")}};
        stage="claim";tx.exec("INSERT INTO trading.qt_desk_receipts(decision_id,attempt_id,status,report_eligibility_status,report_reason_codes)"
            " VALUES("+tx.quote(id)+"::uuid,"+tx.quote(attempt)+"::uuid,'pending','unavailable','[]')");
        auto writable=d;writable["_override"]=p.at("payload").at("requires_override");
        stage="positions";store_positions(tx,before,after,writable,reference);
        if(produce){stage="accounting_storage";require(store_qt_desk_accounting(tx,d,observation_id,accounting).is_ok());}
        stage="results";tx.exec("INSERT INTO trading.qt_desk_results(decision_id,attempt_id,observation_id,content_digest,payload) VALUES("+
            tx.quote(id)+"::uuid,"+tx.quote(attempt)+"::uuid,"+tx.quote(observation_id)+"::uuid,"+
            tx.quote(result_digest)+","+tx.quote(result.dump())+"::jsonb)");
        stage="receipt";tx.exec("UPDATE trading.qt_desk_receipts SET status='processed',published_book_digest="+tx.quote(published)+
            ",processed_at=clock_timestamp(),publication_payload="+tx.quote(publication.dump())+"::jsonb,"
            "report_eligibility_status="+tx.quote(eligibility.status)+",report_reason_codes="+tx.quote(Json(eligibility.reason_codes).dump())+
            "::jsonb,row_manifest_digest="+(eligibility.row_manifest_digest?tx.quote(*eligibility.row_manifest_digest):"NULL")+
            " WHERE decision_id="+tx.quote(id)+"::uuid");
        stage="precommit_authorization";authorize(tx,d,p);observation_row(tx,d,observation_id);
        if(produce){stage="precommit_accounting";require(revalidate_qt_desk_accounting(tx,d,observation_id).is_ok());}
        if(produce){stage="precommit_accounting_outputs";require(verify_qt_desk_accounting_outputs(tx,d,observation_id).is_ok());}
        auto receipt=one(tx,"SELECT to_jsonb(r) FROM trading.qt_desk_receipts r WHERE decision_id="+tx.quote(id)+"::uuid");
        stage="precommit_facts";require(capture_qt_desk_processed_facts(tx,d,p,receipt).is_ok());
        tx.commit();return QtDeskProcessedReceipt{id,attempt,published,publication,false};
    }catch(const std::exception&){return make_error<QtDeskProcessedReceipt>(ErrorCode::INVALID_DATA,
        std::string("qt_desk_unavailable:")+stage,"qt_desk_processor");}
}
Result<QtDeskProcessedReceipt> process_qt_desk_decision(pqxx::connection& connection,const std::string& id,
    const std::string& attempt,const std::string& observation_id){
    return process_qt_desk_impl(connection,id,attempt,observation_id,false);
}
Result<QtDeskProcessedReceipt> process_qt_desk_accounting_decision(pqxx::connection& connection,const std::string& id,
    const std::string& attempt,const std::string& input_id){
    return process_qt_desk_impl(connection,id,attempt,input_id,true);
}
Result<QtDeskProcessedReceipt> process_qt_desk_sourced_decision(pqxx::connection& c,const std::string& id,const std::string& attempt,
    const std::string& input_id,const std::string& market_id,const std::string& final_id){
    if(market_id.empty()||final_id.empty())return make_error<QtDeskProcessedReceipt>(ErrorCode::INVALID_DATA,"qt_sourced_accounting_unavailable","qt_desk_processor");
    return process_qt_desk_impl(c,id,attempt,input_id,true,market_id,final_id);
}
Result<Json> load_qt_desk_report_evidence(pqxx::connection& connection,const std::string& book,const std::string& day){
    try{
        pqxx::work tx(connection);
        auto selected=rows(tx,"SELECT to_jsonb(d) FROM trading.qt_decisions d WHERE book_id="+tx.quote(book)+
            " AND source_day="+tx.quote(day)+"::date ORDER BY created_at DESC,decision_id DESC LIMIT 1");
        if(selected.empty()){
            lock_book(tx,book);
            auto cap=one(tx,"SELECT to_jsonb(c) FROM trading.qt_workflow_capabilities c WHERE book_id="+tx.quote(book));
            require(cap.at("enabled")==false);
            tx.commit();return Json{{"workflow_required",false}};
        }
        auto d=lock_decision(tx,text(selected[0].at("decision_id")));
        auto cap=one(tx,"SELECT to_jsonb(c) FROM trading.qt_workflow_capabilities c WHERE book_id="+tx.quote(book));
        if(cap.at("enabled")==false){tx.commit();return Json{{"workflow_required",false}};}
        auto latest=one(tx,"SELECT to_jsonb(d) FROM trading.qt_decisions d WHERE book_id="+tx.quote(book)+
            " AND source_day="+tx.quote(day)+"::date ORDER BY created_at DESC,decision_id DESC LIMIT 1");
        require(latest==d);auto p=preview_row(tx,d);
        require(validate_qt_desk_decision_snapshot(d,p).is_ok());authorize(tx,d,p);
        auto r=one(tx,"SELECT to_jsonb(r) FROM trading.qt_desk_receipts r WHERE decision_id="+tx.quote(text(d.at("decision_id")))+"::uuid");
        require(capture_qt_desk_processed_facts(tx,d,p,r).is_ok());
        Json output={{"workflow_required",true},{"decision",d},{"preview",p},{"receipt",r}};
        const auto observation_id=text(r.at("publication_payload").at("observation_id"));
        // Processed facts above proves this immutable receipt-linked observation
        // and its historical admission; reporting does not renew its lease.
        auto observed=one(tx,"SELECT payload FROM trading.qt_execution_observations WHERE observation_id="+
            tx.quote(observation_id)+"::uuid AND decision_id="+tx.quote(text(d.at("decision_id")))+"::uuid");
        const auto schema=text(observed.at("schema_version"));
        require(schema=="qt-execution/v1"||schema=="qt-execution/v2");
        for(auto field:{"decision_id","book_id","source_day"})require(observed.at(field)==d.at(field));
        if(schema=="qt-execution/v2"){
            // Only actual accounting-producer observations own this table.
            // Missing or malformed v2 output remains unavailable.
            auto result=one(tx,"SELECT payload FROM trading.desk_run_results WHERE decision_id="+
                tx.quote(text(d.at("decision_id")))+"::uuid");
            if(result.at("schema_version")=="qt-equity-accounting-empty-owner/v2"){
                require(verify_qt_desk_accounting_outputs(tx,d,observation_id).is_ok());
                output["empty_owner"]=empty_owner_document(tx,d);
            }
        }
        tx.commit();return output;
    }catch(const std::exception&){return make_error<Json>(ErrorCode::INVALID_DATA,"qt_report_snapshot_stale","qt_desk_processor");}
}
} // namespace trade_ngin
