// STAGED CANDIDATE; not compiled or run. Native historical archive proof.
#include "trade_ngin/data/qt_model_publication.hpp"
#include "trade_ngin/data/qt_empty_model_owner_storage.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include <set>
#include <regex>
#include <chrono>
#include <stdexcept>
namespace trade_ngin { namespace {
using J=nlohmann::json;
void need(bool b){if(!b)throw std::invalid_argument("qt_model_publication_unavailable");}
std::string text(const J& j){need(j.is_string());auto s=j.get<std::string>();need(!s.empty()&&s.size()<=1048576);return s;}
void shape(const J& j,std::initializer_list<const char*> names){need(j.is_object()&&j.size()==names.size());for(auto n:names)need(j.contains(n));}
J one(pqxx::work& tx,const std::string& sql){auto r=tx.exec(sql);need(r.size()==1&&!r[0][0].is_null());auto value=r[0][0].as<std::string>();need(value.size()<=8388608);return J::parse(value);}
std::string digest(const J& j){auto b=canonical_qt_desk_source_json(j);need(b.is_ok());auto h=qt_sha256_hex(b.value());need(h.is_ok());return h.value();}
Decimal exact(const J& j){auto d=parse_qt_quantity_exact(text(j));need(d.is_ok());return d.value();}
ComponentPositionKey key(const J& j){shape(j,{"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"});return {text(j.at("portfolio_id")),text(j.at("strategy_id")),text(j.at("strategy_name")),text(j.at("date")),text(j.at("symbol")),text(j.at("portfolio_type"))};}
bool utc_time(const J& value){
 if(!value.is_string())return false;const auto v=value.get<std::string>();
 static const std::regex grammar(R"([0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]{1,6})?Z)");
 if(!std::regex_match(v,grammar))return false;
 const auto y=std::stoi(v.substr(0,4));
 return y>0&&std::chrono::year_month_day(std::chrono::year(y),std::chrono::month(std::stoi(v.substr(5,2))),std::chrono::day(std::stoi(v.substr(8,2)))).ok()&&std::stoi(v.substr(11,2))<24&&std::stoi(v.substr(14,2))<60&&std::stoi(v.substr(17,2))<60;
}
void inspection(pqxx::work& tx,const J& row){
 const auto& c=row.at("inspection_capture");shape(c,{"publication_schema_version","profile","authority","stream","identity","captured_at","publication_recorded_at","status","reason","equity_run_consumption"});
 need(c.at("publication_schema_version").is_number_integer()&&c.at("publication_schema_version")==3&&c.at("profile")=="live_equity_mean_reversion"&&c.at("authority")=="inspection_only"&&c.at("stream")=="system"&&c.at("status")=="available"&&c.at("reason")=="none");
 const auto& id=c.at("identity");shape(id,{"registry_id","registry_revision","engine_strategy_id","portfolio_id","run_date","capture_id","publication_id","runtime_attempt_id","producer_version","control_mode"});
 need(id.at("registry_id")==row.at("registry_id")&&id.at("registry_revision").is_number_integer()&&id.at("registry_revision")==row.at("registry_revision")&&id.at("engine_strategy_id")==row.at("strategy_id")&&id.at("portfolio_id")==row.at("portfolio_id")&&id.at("run_date")==row.at("source_day")&&id.at("capture_id")==row.at("publication_id")&&id.at("publication_id")==row.at("publication_id")&&id.at("producer_version")==row.at("producer_version")&&id.at("runtime_attempt_id")==row.at("attempt_id")&&id.at("control_mode")==(row.at("attempt_id").is_null()?"uncontrolled":"controlled"));
 // This archive is actual metadata read back by the same publisher transaction,
 // not a supplied Boolean or a mutable same-day metadata read by this helper.
 const auto& run=c.at("equity_run_consumption");need(run.is_object()&&run.at("available").is_boolean()&&run.at("available")==true&&run.at("complete").is_boolean()&&run.at("complete")==true);
 shape(run,{"schema_version","catalog_version","scope","profile","run_key","available","complete","unavailable_reason","stages"});
 const bool action_v2=run.at("schema_version")=="qt-equity-run-consumption/v2";
 need((action_v2?run.at("catalog_version")=="qt-equity-main08b15c-run/v2":run.at("schema_version")=="qt-equity-run-consumption/v1"&&run.at("catalog_version")=="qt-equity-main08b15c-run/v1")&&run.at("scope")=="full_run"&&run.at("profile")=="mean_reversion"&&run.at("unavailable_reason").is_null());
 if(action_v2){
  const auto& reads=run.at("stages").at("corporate_actions").at("reads");
  shape(reads,{"path","effective_event_count","original_action_count","successor_action_count","original_action_digest","successor_action_digest","basis_frame_digest"});
  need(reads.at("path")=="proved_action_adjusted_prior"&&reads.at("effective_event_count").is_number_integer()&&reads.at("effective_event_count")==0&&run.at("stages").at("prior").at("reads").at("mode")=="verified_desk_prior");
  int64_t count=0;for(auto name:{"original_action_count","successor_action_count"}){need(reads.at(name).is_number_integer());const auto n=reads.at(name).get<int64_t>();need(n>=0&&n<=INT32_MAX);count+=n;}need(count>0);
  for(auto name:{"original_action_digest","successor_action_digest","basis_frame_digest"}){const auto h=text(reads.at(name));need(h.size()==64&&h.find_first_not_of("0123456789abcdef")==std::string::npos);}
 }
 const auto& run_key=run.at("run_key");shape(run_key,{"portfolio_id","strategy_id","strategy_name","date"});
 need(run_key.at("portfolio_id")==row.at("portfolio_id")&&run_key.at("strategy_id")==row.at("strategy_id")&&run_key.at("date")==row.at("source_day")&&run_key.at("strategy_name")=="EQUITY_MEAN_REVERSION"&&row.at("configured_owner_names")==J::array({run_key.at("strategy_name")}));
 const auto& stages=run.at("stages");shape(stages,{"setup","market_input","cost_history","prior","corporate_actions","preparation","primary","execution","eod","result_assembly"});
 for(const auto& stage:stages.items()){
  const auto& value=stage.value();shape(value,{"outcome","skip_reason","reads","symbols","executions"});
  need(value.at("reads").is_object()&&value.at("symbols").is_object()&&value.at("symbols").size()<=256&&value.at("executions").is_array()&&value.at("executions").size()<=4096);
  if(value.at("outcome")=="returned_ok")need(value.at("skip_reason").is_null());
  else{
   need(value.at("outcome")=="skipped");
   if(stage.key()=="eod")need(value.at("skip_reason")=="proved_desk_successor"&&value.at("reads").at("path")=="proved_desk_successor");
   else need((stage.key()=="preparation"||stage.key()=="primary"||stage.key()=="execution")&&value.at("skip_reason")=="non_trading_day"&&value.at("reads").empty()&&value.at("symbols").empty()&&value.at("executions").empty());
  }
 }
 const bool nontrading=stages.at("primary").at("outcome")=="skipped";
 for(auto name:{"preparation","primary","execution"})need((stages.at(name).at("outcome")=="skipped")==nontrading);
 // Closed stage identity/outcome binding complements the actual typed capture
 // seal. It is not a new financial calculation or a replacement for the full
 // existing API inspection parser used before QT preview readiness.


 need(utc_time(c.at("captured_at"))&&utc_time(c.at("publication_recorded_at"))&&utc_time(row.at("created_at")));
 const auto times=tx.exec("SELECT $1::timestamptz<=$2::timestamptz AND $2::timestamptz<=$3::timestamptz AND ($1::timestamptz AT TIME ZONE 'UTC')::date=$4::date AND ($2::timestamptz AT TIME ZONE 'UTC')::date=$4::date",pqxx::params{text(c.at("captured_at")),text(c.at("publication_recorded_at")),text(row.at("created_at")),text(row.at("source_day"))});
 need(times.size()==1&&times[0][0].as<bool>());
 if(row.at("attempt_id").is_null()){need(row.at("registry_revision")==0);return;} // Actual uncontrolled publisher requires original revision0, never current authority.
 auto attempt=one(tx,"SELECT to_jsonb(a) FROM trading.runtime_attempts a WHERE id="+tx.quote(text(row.at("attempt_id")))+"::text FOR SHARE");
 auto intent=one(tx,"SELECT to_jsonb(i) FROM trading.runtime_intents i WHERE id="+tx.quote(attempt.at("intent_id").dump())+"::bigint FOR SHARE");
 need(attempt.at("id")==row.at("publication_id")&&attempt.at("publication_id")==row.at("publication_id")&&attempt.at("status")=="applied"&&attempt.at("outcome")=="published"&&attempt.at("registry_revision")==row.at("registry_revision")&&attempt.at("run_date")==row.at("source_day")&&attempt.at("producer_version")==row.at("producer_version")&&digest(attempt.at("config_snapshot"))==text(row.at("configuration_digest")));
 need(intent.at("registry_id")==row.at("registry_id")&&intent.at("registry_revision")==row.at("registry_revision")&&intent.at("portfolio_id")==row.at("portfolio_id")&&intent.at("engine_strategy_id")==row.at("strategy_id")&&intent.at("action")=="run"&&intent.at("status")=="approved"&&digest(intent.at("config_snapshot"))==text(row.at("configuration_digest")));
}
} // namespace
Result<QtModelPublicationRecord> load_qt_model_publication_record(pqxx::work& tx,const std::string& id){try{
 const bool v2=require_qt_empty_owner_storage_capability(tx);
 auto legacy=tx.exec("SELECT CASE WHEN octet_length(to_jsonb(p)::text)<=8388608 THEN to_jsonb(p) ELSE NULL END FROM trading.qt_model_seed_publications p WHERE publication_id=$1::uuid FOR SHARE",pqxx::params{id});
 pqxx::result empty;if(v2)empty=tx.exec("SELECT CASE WHEN octet_length(to_jsonb(p)::text)<=8388608 THEN to_jsonb(p)||jsonb_build_object('created_at',to_char(p.created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')) ELSE NULL END FROM trading.qt_empty_model_owner_publications p WHERE publication_id=$1::uuid FOR SHARE",pqxx::params{id});
 need(legacy.size()+empty.size()==1);
 if(!legacy.empty()){
   need(!legacy[0][0].is_null());J row=J::parse(legacy[0][0].c_str()),ref;for(auto n:{"publication_id","strategy_id","publication_version","seed_digest","proposal_manifest_digest","producer_version"})ref[n]=row.at(n);
   return QtModelPublicationRecord{QtModelPublicationKind::LegacyV1,std::move(row),std::move(ref)};
 }
 need(!empty[0][0].is_null());J r=J::parse(empty[0][0].c_str());
 shape(r,{"publication_id","attempt_id","schema_version","portfolio_id","strategy_id","source_day","publication_version","registry_id","registry_revision","configured_owner_names","configuration_snapshot","configuration_digest","fresh_empty_batches","inspection_capture","system_components","seed_digest","proposal_components","proposal_manifest_digest","qt_components","qt_digest","producer_version","created_at"});
 need(r.at("schema_version")=="qt-empty-model-owner-publication/v2"&&r.at("registry_revision").is_number_integer()&&r.at("publication_version").is_number_integer());
 QtEmptyModelOwnerPublication operand;auto& p=operand.publication;
 p.publication_id=text(r.at("publication_id"));p.portfolio_id=text(r.at("portfolio_id"));p.strategy_id=text(r.at("strategy_id"));p.source_day=text(r.at("source_day"));p.producer_version=text(r.at("producer_version"));
 need(r.at("system_components").is_array()&&r.at("system_components").empty());
 operand.configuration_snapshot=r.at("configuration_snapshot");operand.configured_owner_names=r.at("configured_owner_names").get<std::vector<std::string>>();
 need(r.at("fresh_empty_batches").is_array()&&r.at("fresh_empty_batches").size()<=4096);
 for(const auto& b:r.at("fresh_empty_batches")){shape(b,{"portfolio_id","strategy_id","strategy_name","source_day"});operand.fresh_empty_batches.push_back({text(b.at("portfolio_id")),text(b.at("strategy_id")),text(b.at("strategy_name")),text(b.at("source_day"))});}
 need(r.at("qt_components").is_array()&&r.at("qt_components").size()<=4096);
 for(const auto& q:r.at("qt_components")){shape(q,{"key","quantity_exact","average_price_exact"});operand.qt_components.push_back({key(q.at("key")),exact(q.at("quantity_exact")),exact(q.at("average_price_exact"))});}
 need(r.at("proposal_components").is_array()&&r.at("proposal_components").size()<=4096);
 for(const auto& q:r.at("proposal_components")){shape(q,{"key","quantity_exact","average_price_exact","action","position_revision","origin_publication_id"});p.proposal_components.push_back({key(q.at("key")),exact(q.at("quantity_exact")),exact(q.at("average_price_exact")),text(q.at("action")),q.at("position_revision").is_null()?std::nullopt:std::optional<std::string>{text(q.at("position_revision"))},q.at("origin_publication_id").is_null()?std::nullopt:std::optional<std::string>{text(q.at("origin_publication_id"))}});}
 auto doc=qt_empty_model_owner_document(operand);need(doc.is_ok());
 for(auto n:{"system_components","seed_digest","proposal_components","proposal_manifest_digest","qt_components","qt_digest","configuration_digest","configured_owner_names"})need(doc.value().at(n)==r.at(n));
 auto reference=qt_empty_model_owner_reference(doc.value(),r.at("publication_version").get<int64_t>(),text(r.at("producer_version")),text(r.at("registry_id")),r.at("registry_revision").get<int64_t>());need(reference.is_ok());
 inspection(tx,r);
 // No current registry/metadata/run_inputs, physical snapshot equality, lease
 // freshness, recursive accounting or changed clock participates in this archive.
 return QtModelPublicationRecord{QtModelPublicationKind::EmptyOwnerV2,std::move(r),reference.value()};
}catch(const std::exception&){return make_error<QtModelPublicationRecord>(ErrorCode::INVALID_DATA,"qt_model_publication_unavailable");}}
Result<J> load_qt_model_publication_scope(pqxx::work& tx,const std::string& book,const std::string& day){try{
 const bool v2=require_qt_empty_owner_storage_capability(tx);
 const auto query=v2?
   "SELECT publication_id::text,publication_version FROM (SELECT publication_id,publication_version FROM trading.qt_model_seed_publications WHERE portfolio_id=$1 AND source_day=$2::date UNION ALL SELECT publication_id,publication_version FROM trading.qt_empty_model_owner_publications WHERE portfolio_id=$1 AND source_day=$2::date) p ORDER BY publication_id LIMIT 4097":
   "SELECT publication_id::text,publication_version FROM trading.qt_model_seed_publications WHERE portfolio_id=$1 AND source_day=$2::date ORDER BY publication_id LIMIT 4097";
 auto rows=tx.exec(query,pqxx::params{book,day});need(!rows.empty()&&rows.size()<=4096);
 std::set<std::string> ids;std::set<int64_t> versions;int64_t latest=0;std::string head;J refs=J::array();size_t bytes=0;
 for(const auto& row:rows){need(!row[0].is_null()&&!row[1].is_null());auto id=row[0].as<std::string>();auto version=row[1].as<int64_t>();need(ids.insert(id).second&&version>0&&versions.insert(version).second);
  auto loaded=load_qt_model_publication_record(tx,id);need(loaded.is_ok());const auto& record=loaded.value();need(record.row.at("portfolio_id")==book&&record.row.at("source_day")==day);
  const auto count=record.row.dump().size();need(count<=256u*1024*1024&&bytes<=256u*1024*1024-count);bytes+=count;
  refs.push_back(record.reference);if(version>latest){latest=version;head=id;}
 }
 return J{{"references",refs},{"latest_publication_id",head}};
}catch(const std::exception&){return make_error<J>(ErrorCode::INVALID_DATA,"qt_model_publication_scope_unavailable");}}
} // namespace trade_ngin
