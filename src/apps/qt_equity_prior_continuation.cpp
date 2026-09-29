#include "trade_ngin/apps/qt_equity_prior_continuation.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <utility>
// Pure continuation rule for equity day 2. No pqxx call and no I/O happen here:
// the canonicalizer header only declares the shared digest rule.
namespace trade_ngin { namespace {
using J=nlohmann::json;
constexpr const char* kEngine="LIVE_EQUITY_MEAN_REVERSION";
constexpr const char* kOwner="EQUITY_MEAN_REVERSION";
void need(bool ok){if(!ok)throw std::invalid_argument("qt_equity_prior_continuation_unavailable");}
std::string text(const J& j){need(j.is_string());auto s=j.get<std::string>();need(!s.empty()&&s.size()<=4096);return s;}
void shape(const J& j,std::initializer_list<const char*> fields){need(j.is_object()&&j.size()==fields.size());for(auto f:fields)need(j.contains(f));}
void hex(const J& j){const auto s=text(j);need(s.size()==64&&s.find_first_not_of("0123456789abcdef")==std::string::npos);}
std::string hash(const J& j){auto c=canonical_qt_desk_input_json(j);need(c.is_ok());auto h=qt_sha256_hex(c.value());need(h.is_ok());return h.value();}
void sealed(const J& row){need(row.is_object());hex(row.at("content_digest"));need(hash(row.at("payload"))==text(row.at("content_digest")));}
void same_metadata(const J& a,const J& b){for(auto f:{"producer_id","policy_version"})need(text(a.at(f))==text(b.at(f)));}
void revision(const J& j){need(j.is_number_integer()&&!j.is_boolean()&&j.get<int64_t>()>0);}
// The MODEL's operands and screening conversion, unchanged
// (equity_model_action_frame.cpp:21-22).
double number(const J& j){const auto s=text(j);size_t n=0;const auto d=std::stod(s,&n);need(n==s.size()&&std::isfinite(d));return d;}
Decimal checked(double value){const double scaled=value*100000000.0+(value>=0?0.5:-0.5);need(std::isfinite(scaled)&&static_cast<long double>(scaled)>=static_cast<long double>(INT64_MIN)&&static_cast<long double>(scaled)<=static_cast<long double>(INT64_MAX));return Decimal(value);}
// A governed price operand, read exactly as the finalizer reads it
// (qt_equity_prior_finalization.cpp:35).
double price(const J& j){const auto s=text(j);static const std::regex grammar(R"(-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?)");need(s.size()<=64&&std::regex_match(s,grammar));double n=0;const auto p=std::from_chars(s.data(),s.data()+s.size(),n,std::chars_format::general);need(p.ec==std::errc{}&&p.ptr==s.data()+s.size()&&std::isfinite(n)&&n>0);return n;}
// An ISO-8601 instant in microseconds (to_jsonb timestamptz spelling; offsets Z, +HH, +HHMM, +HH:MM).
// Rows captured under another session time zone spell the same instant with another offset.
std::int64_t instant(const J& j){
 static const std::regex r(R"(([0-9]{4})-([0-9]{2})-([0-9]{2})[T ]([0-9]{2}):([0-9]{2}):([0-9]{2})(?:\.([0-9]{1,6}))?(Z|[+-][0-9]{2}(?::?[0-9]{2})?))");
 const auto s=text(j);std::smatch f;need(std::regex_match(s,f,r));using namespace std::chrono;
 const year_month_day ymd{year{std::stoi(f[1])},month{static_cast<unsigned>(std::stoi(f[2]))},day{static_cast<unsigned>(std::stoi(f[3]))}};need(ymd.ok());
 const auto hh=std::stoll(f[4]),mm=std::stoll(f[5]),ss=std::stoll(f[6]);need(hh<24&&mm<60&&ss<60);
 std::string fraction=f[7];while(fraction.size()<6)fraction+='0';
 std::int64_t micros=duration_cast<microseconds>(sys_days{ymd}.time_since_epoch()).count()+((hh*60+mm)*60+ss)*1000000+std::stoll(fraction);
 const std::string zone=f[8];
 if(zone!="Z"){const std::int64_t sign=zone[0]=='-'?-1:1;const auto hours=std::stoll(zone.substr(1,2));const auto minutes=zone.size()>3?std::stoll(zone.substr(zone.size()-2)):0;need(hours<24&&minutes<60);micros-=sign*(hours*60+minutes)*60*1000000;}
 return micros;
}
// Whole-row equality with the MODEL's captured copy; created_at as an instant.
void same_actions_row(const J& actual,const J& captured){
 need(actual.is_object()&&captured.is_object()&&actual.size()==captured.size());
 for(auto it=captured.begin();it!=captured.end();++it){need(actual.contains(it.key()));
  if(it.key()=="created_at")need(instant(actual.at("created_at"))==instant(it.value()));else need(actual.at(it.key())==it.value());}
}
// The source-id namespace of a quote: the text before the last '/'.
std::string ns(const std::string& id){const auto slash=id.rfind('/');need(slash!=std::string::npos&&slash>0);return id.substr(0,slash);}
const std::initializer_list<const char*> kMarket={"schema_version","calculation_version","book_id","source_day","model_publication_id","previous_day","valuation_time","day_mode","currency","cost_config","instruments","actions_source_id","actions_source_digest"};
const std::initializer_list<const char*> kReplayV1={"schema_version","mode","book_id","source_day","valuation_day","decision_id","finalization_id","finalization_digest","finalization_source_id","finalization_source_digest","model_publication_id","model_seed_digest","accounting_input_id","accounting_input_digest","attempt_id","observation_id","observation_digest","results_digest","basis_positions","action_admission"};
const std::initializer_list<const char*> kReplayV2={"schema_version","mode","book_id","source_day","valuation_day","decision_id","finalization_id","finalization_digest","finalization_source_id","finalization_source_digest","model_publication_id","model_seed_digest","accounting_input_id","accounting_input_digest","attempt_id","observation_id","observation_digest","results_digest","basis_positions","action_admission","action_frame","action_frame_digest"};
// Every instrument of a D market is priced at the previous close (the
// finalizer's S-frame marks, the kernel's prior-dated operands).
std::map<std::string,J> instruments(const J& payload){
 const auto& list=payload.at("instruments");need(list.is_array()&&list.size()<=4096);std::map<std::string,J> out;
 for(const auto& row:list){
  shape(row,{"symbol","asset_type","reference","mark","cost_evidence","cost_parameters"});need(row.at("asset_type")=="EQUITY");
  for(auto field:{"reference","mark"}){const auto& q=row.at(field);shape(q,{"source_id","source_digest","date","price_frame_id","price_model_number"});text(q.at("source_id"));hex(q.at("source_digest"));need(q.at("date")==payload.at("previous_day"));text(q.at("price_frame_id"));price(q.at("price_model_number"));}
  need(row.at("reference").at("price_frame_id")==row.at("mark").at("price_frame_id"));
  const auto& cost=row.at("cost_evidence");need(cost.is_object()&&cost.contains("date")&&cost.at("date")==payload.at("previous_day"));
  need(out.emplace(text(row.at("symbol")),row).second);
 }
 return out;
}
}

Result<void> validate_qt_equity_verified_prior_continuation(const QtEquityPriorContinuationInputs& in){try{
 const auto& d=in.decision;const auto& s=in.prior_decision;const auto& m=in.finalization_market;const auto& a=in.input_market;
 const auto& f=in.finalization;const auto& anchor=in.anchor;const auto& b=in.binding;const auto& act=in.actions_row;
 for(const auto* j:{&d,&s,&m,&a,&f,&anchor,&b,&act})need(j->is_object()&&j->dump().size()<=8u*1024*1024);
 need(in.candidate_action_sources.size()<=4096);
 const auto book=text(d.at("book_id")),day=text(d.at("source_day")),publication=text(d.at("model_publication_id"));text(d.at("decision_id"));
 // Same market is the unchanged old path (:122, :90), never a continuation.
 text(m.at("source_id"));text(a.at("source_id"));need(m.at("source_id")!=a.at("source_id"));
 for(const auto* row:{&m,&a,&f,&anchor,&act})sealed(*row);
 const auto& mp=m.at("payload");const auto& ap=a.at("payload");shape(mp,kMarket);shape(ap,kMarket);
 need(mp.at("schema_version")=="qt-equity-finalization-market/v1"&&m.at("model_publication_id").is_null()&&mp.at("model_publication_id").is_null());
 need(ap.at("schema_version")=="qt-equity-accounting-market/v1"&&a.at("model_publication_id")==publication&&ap.at("model_publication_id")==publication);

 // (b) A equals M on every valuation field, policy revision and metadata; only the model differs.
 for(auto k:{"calculation_version","book_id","source_day","previous_day","valuation_time","day_mode","currency","cost_config"})need(ap.at(k)==mp.at(k));
 for(auto k:{"book_id","source_day"})need(m.at(k)==mp.at(k)&&a.at(k)==ap.at(k));
 need(ap.at("book_id")==book&&ap.at("source_day")==day);text(m.at("source_version"));text(a.at("source_version"));
 revision(a.at("policy_revision"));need(a.at("policy_revision")==m.at("policy_revision"));same_metadata(a,m);

 // F finalized this S decision against M, and the anchor is F's derived v2 anchor.
 const auto fid=text(f.at("finalization_id"));const auto& fp=f.at("payload");need(fp.is_object());
 need(f.at("market_source_id")==m.at("source_id")&&fp.at("market_source_id")==m.at("source_id")&&fp.at("market_source_digest")==m.at("content_digest"));
 need(fp.at("actions_source_id")==mp.at("actions_source_id")&&fp.at("actions_source_digest")==mp.at("actions_source_digest"));
 need(fp.at("finalization_id")==fid&&fp.at("decision_id")==f.at("decision_id")&&f.at("source_version")=="qt-finalization/"+fid);
 need(f.at("decision_id")==s.at("decision_id")&&s.at("book_id")==book&&text(s.at("source_day"))<day&&text(s.at("model_publication_id")).size()>0);
 need(f.at("book_id")==book&&f.at("source_day")==s.at("source_day")&&f.at("valuation_day")==day&&ap.at("previous_day")==f.at("source_day"));
 need(f.at("policy_revision")==a.at("policy_revision"));same_metadata(f,a);
 const auto& np=anchor.at("payload");
 need(anchor.at("source_id")=="qt-finalization/"+fid&&anchor.at("source_version")==anchor.at("source_id")&&anchor.at("book_id")==book&&anchor.at("source_day")==f.at("source_day"));same_metadata(anchor,f);
 shape(np,{"schema_version","calculation_version","book_id","source_day","currency","policy_revision","previous_positions","previous_totals","finalization_id","finalization_digest"});
 need(np.at("schema_version")=="qt-equity-finalized-accounting/v2"&&np.at("book_id")==book&&np.at("source_day")==f.at("source_day")&&np.at("currency")==ap.at("currency"));
 need(np.at("finalization_id")==fid&&np.at("finalization_digest")==f.at("content_digest")&&np.at("policy_revision")==f.at("policy_revision"));
 const auto& basis=np.at("previous_positions");need(basis.is_array()&&basis.size()<=4096);std::set<std::string> held;
 for(const auto& p:basis){const auto& k=p.at("key");need(k.at("portfolio_id")==book);held.insert(text(k.at("symbol")));}

 // (a) P2's binding: this decision's model, this finalization and its anchor, and the anchor's basis.
 shape(b,{"publication_id","book_id","source_day","strategy_id","decision_id","finalization_id","finalization_digest","finalization_source_id","finalization_source_digest","actions_source_id","actions_source_digest","replay_reference","replay_reference_digest","created_at"});
 need(b.at("publication_id")==publication&&b.at("book_id")==book&&b.at("source_day")==day&&b.at("strategy_id")==kEngine);
 need(b.at("decision_id")==s.at("decision_id")&&b.at("finalization_id")==fid&&b.at("finalization_digest")==f.at("content_digest"));
 need(b.at("finalization_source_id")==anchor.at("source_id")&&b.at("finalization_source_digest")==anchor.at("content_digest"));
 const auto& rr=b.at("replay_reference");hex(b.at("replay_reference_digest"));need(rr.is_object()&&hash(rr)==text(b.at("replay_reference_digest")));
 const bool v2=rr.contains("schema_version")&&rr.at("schema_version")=="qt-equity-model-prior/v2";
 if(v2)shape(rr,kReplayV2);else shape(rr,kReplayV1);
 need(rr.at("schema_version")==(v2?"qt-equity-model-prior/v2":"qt-equity-model-prior/v1")&&rr.at("mode")=="verified_desk_prior"&&rr.at("action_admission")==(v2?"proved_action_adjusted_prior":"action_free_only"));
 need(rr.at("book_id")==book&&rr.at("source_day")==f.at("source_day")&&rr.at("valuation_day")==day&&rr.at("decision_id")==s.at("decision_id"));
 need(rr.at("finalization_id")==fid&&rr.at("finalization_digest")==f.at("content_digest")&&rr.at("finalization_source_id")==anchor.at("source_id")&&rr.at("finalization_source_digest")==anchor.at("content_digest"));
 need(rr.at("model_publication_id")==s.at("model_publication_id")&&rr.at("basis_positions")==basis);
 J applied=nullptr;
 if(v2){
  const auto& frame=rr.at("action_frame");hex(rr.at("action_frame_digest"));need(frame.is_object()&&hash(frame)==text(rr.at("action_frame_digest")));
  shape(frame,{"schema_version","owner","original_action_count","original_action_digest","successor_action_count","successor_action_digest","original_basis_positions","derived_model_basis_positions","actions_source","policy_identity","raw_capture","adjustments"});
  need(frame.at("schema_version")=="qt-equity-model-action-frame/v1"&&frame.at("original_basis_positions")==basis);
  need(frame.at("owner")==J{{"portfolio_id",book},{"strategy_id",kEngine},{"strategy_name",kOwner},{"source_day",f.at("source_day")},{"valuation_day",day}});
  applied=frame.at("actions_source");need(applied.is_object());const auto& events=applied.at("payload").at("events");need(events.is_array());
  need(frame.at("successor_action_digest")==hash(events)&&frame.at("successor_action_count").is_number_integer()&&frame.at("successor_action_count").get<int64_t>()==static_cast<int64_t>(events.size()));
  need(b.at("actions_source_id")==applied.at("source_id")&&b.at("actions_source_digest")==applied.at("content_digest"));
 }else need(b.at("actions_source_id").is_null()&&b.at("actions_source_digest").is_null());

 // (c) A's actions are exactly the row the MODEL applied, and no other candidate D action row exists.
 const auto& ep=act.at("payload");
 need(act.at("source_id")==ap.at("actions_source_id")&&act.at("content_digest")==ap.at("actions_source_digest")&&act.at("purpose")=="actions"&&act.at("source_version")==act.at("source_id"));
 need(act.at("book_id")==book&&act.at("source_day")==day&&act.at("policy_revision")==a.at("policy_revision"));same_metadata(act,a);
 shape(ep,{"schema_version","book_id","source_day","previous_day","valuation_time","events"});need(ep.at("schema_version")=="qt-equity-actions-source/v1");
 for(auto k:{"book_id","source_day","previous_day","valuation_time"})need(ep.at(k)==ap.at(k));
 const auto& events=ep.at("events");need(events.is_array()&&events.size()<=4096);
 if(v2)same_actions_row(act,applied);
 if(events.empty())need(ap.at("actions_source_id")==mp.at("actions_source_id")&&ap.at("actions_source_digest")==mp.at("actions_source_digest"));
 else need(v2);
 std::set<std::string> candidates;for(const auto& c:in.candidate_action_sources)need(!c.empty()&&c.size()<=4096&&candidates.insert(c).second);
 if(events.empty())need(candidates.empty());else need(candidates.size()==1&&*candidates.begin()==text(act.at("source_id")));

 // (d) Instruments: M's marks, with action symbols restated by the MODEL's arithmetic.
 const auto before=instruments(mp),after=instruments(ap);
 std::map<std::string,std::vector<J>> actions;std::pair<std::string,int> previous{"",-1};
 for(const auto& e:events){
  shape(e,{"key","type","ex_date","value_model_number","basis_provenance","basis_provenance_evidence","frame_before","frame_after","raw_close_model_number","eligible_quantity_exact"});
  const auto& k=e.at("key");shape(k,{"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"});
  need(k.at("portfolio_id")==book&&k.at("strategy_id")==kEngine&&k.at("strategy_name")==kOwner&&k.at("date")==day&&k.at("portfolio_type")=="qt"&&e.at("ex_date")==day);
  const auto symbol=text(k.at("symbol"));need(before.contains(symbol)&&held.contains(symbol));
  const auto type=text(e.at("type"));const bool dividend=type=="DIVIDEND";need(dividend||type=="SPLIT"||type=="ADR_SPLIT");
  // The MODEL's event order: by symbol, split before dividend, one of each.
  const std::pair<std::string,int> order{symbol,dividend?1:0};need(order>previous);previous=order;
  actions[symbol].push_back(e);
 }
 for(const auto& [symbol,row]:before){
  need(after.contains(symbol));J expected=row;
  if(actions.contains(symbol)){
   const auto& list=actions.at(symbol);auto frame=text(row.at("mark").at("price_frame_id"));
   for(const auto& e:list){need(e.at("frame_before")==frame);const auto next=text(e.at("frame_after"));need(next!=frame);frame=next;}
   for(auto field:{"reference","mark"}){
    // equity_model_action_frame.cpp:80 with the mark in the basis position: the
    // quotient of each step is screened to Decimal(8dp) before the next.
    double current=price(row.at(field).at("price_model_number"));Decimal restated;
    for(const auto& e:list){
     const bool dividend=e.at("type")=="DIVIDEND";const double value=number(e.at("value_model_number"));need(value>0);double close=0;
     if(dividend){close=number(e.at("raw_close_model_number"));need(close>0);text(e.at("eligible_quantity_exact"));}
     else need(value!=1&&e.at("raw_close_model_number").is_null()&&e.at("eligible_quantity_exact").is_null());
     const auto factor=dividend?1+value/close:value;need(std::isfinite(factor)&&factor>0);
     restated=checked(current/factor);need(restated.is_positive());current=restated.as_double();
    }
    expected[field]["price_frame_id"]=frame;expected[field]["price_model_number"]=restated.to_string();
   }
  }
  need(after.at(symbol)==expected);
 }
 // Spec default 6: an open on D, never a held owner, never an action symbol; priced by the
 // same governed source as M: its quotes share M's single source-id namespace, are dated at
 // the previous close, and use one frame (dates and frame are checked by instruments()).
 // Namespaces are read only when an open exists: M's quote ids need not carry a '/' (r3, A1 review finding 1).
 std::vector<std::string> opens;for(const auto& [symbol,row]:after)if(!before.contains(symbol))opens.push_back(symbol);
 if(!opens.empty()){
  std::set<std::string> namespaces;for(const auto& [symbol,row]:before)for(auto field:{"reference","mark"})namespaces.insert(ns(text(row.at(field).at("source_id"))));
  need(namespaces.size()==1);const auto governed=*namespaces.begin();
  for(const auto& symbol:opens){const auto& row=after.at(symbol);need(!held.contains(symbol)&&!actions.contains(symbol));
   for(auto field:{"reference","mark"})need(ns(text(row.at(field).at("source_id")))==governed);}
 }
 return Result<void>();
}catch(const std::exception&){return make_error<void>(ErrorCode::INVALID_DATA,"qt_equity_prior_continuation_unavailable","qt_equity_prior_continuation");}}
}
