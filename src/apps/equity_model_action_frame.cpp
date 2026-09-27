#include "trade_ngin/apps/equity_model_action_frame.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include <cmath>
#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <stdexcept>
namespace trade_ngin { namespace {
using J=nlohmann::json;
void need(bool b){if(!b)throw std::invalid_argument("equity_model_action_frame_unavailable");}
std::string text(const J& j){need(j.is_string());auto s=j.get<std::string>();need(!s.empty()&&s.size()<=4096);return s;}
void shape(const J& j,std::initializer_list<const char*> keys){need(j.is_object()&&j.size()==keys.size());for(auto k:keys)need(j.contains(k));}
std::string hash(const J& j){auto c=canonical_qt_desk_input_json(j);need(c.is_ok());auto h=qt_sha256_hex(c.value());need(h.is_ok());return h.value();}
void hex(const J& j){const auto s=text(j);need(s.size()==64&&s.find_first_not_of("0123456789abcdef")==std::string::npos);}
std::chrono::sys_days day(const std::string& s){need(s.size()==10&&s[4]=='-'&&s[7]=='-');for(size_t i=0;i<s.size();++i)if(i!=4&&i!=7)need(s[i]>='0'&&s[i]<='9');const std::chrono::year_month_day d{std::chrono::year{std::stoi(s.substr(0,4))},std::chrono::month{static_cast<unsigned>(std::stoi(s.substr(5,2)))},std::chrono::day{static_cast<unsigned>(std::stoi(s.substr(8,2)))}};need(int(d.year())>0&&d.ok());return std::chrono::sys_days{d};}
Decimal exact(const J& j){auto q=parse_qt_quantity_exact(text(j));need(q.is_ok());return q.value();}
double number(const J& j){const auto s=text(j);size_t n=0;const auto d=std::stod(s,&n);need(n==s.size()&&std::isfinite(d));return d;}
Decimal checked(double value){const double scaled=value*100000000.0+(value>=0?0.5:-0.5);need(std::isfinite(scaled)&&static_cast<long double>(scaled)>=static_cast<long double>(INT64_MIN)&&static_cast<long double>(scaled)<=static_cast<long double>(INT64_MAX));return Decimal(value);}
void scope(const J& k,const EquityModelPriorOwner& o,const std::string& date){shape(k,{"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"});need(k.at("portfolio_id")==o.portfolio_id&&k.at("strategy_id")==o.strategy_id&&k.at("strategy_name")==o.strategy_name&&k.at("date")==date&&k.at("portfolio_type")=="qt");text(k.at("symbol"));}
}
Result<EquityModelActionFrame> derive_equity_model_action_frame(
 const VerifiedEquityModelPrior& prior,const EquityModelPriorOwner& owner,
 const J& original,const J& source,const J& policy,const J& raw) {try{
 need(!owner.portfolio_id.empty()&&owner.portfolio_id.size()<=100&&owner.strategy_id=="LIVE_EQUITY_MEAN_REVERSION"&&owner.strategy_name=="EQUITY_MEAN_REVERSION");need(day(owner.source_day)<day(owner.valuation_day));
 const auto& old=prior.replay_reference;
 need(old.at("schema_version")=="qt-equity-model-prior/v1"&&old.at("mode")=="verified_desk_prior"&&old.at("action_admission")=="action_free_only");
 need(old.at("book_id")==owner.portfolio_id&&old.at("source_day")==owner.source_day&&old.at("valuation_day")==owner.valuation_day&&old.at("basis_positions")==prior.basis_positions);
 need(original.is_array()&&original.size()<=4096&&prior.basis_positions.is_array()&&prior.basis_positions.size()<=4096&&prior.basis_positions.size()==prior.positions.size());
 shape(raw,{"bars","aliases","terminations","restating_metadata"});for(auto f:{"bars","aliases","terminations","restating_metadata"})need(raw.at(f).is_array()&&raw.at(f).size()<=4096);need(raw.at("aliases").empty()&&raw.at("terminations").empty());
 need(original.dump().size()<=2u*1024*1024&&raw.dump().size()<=2u*1024*1024&&source.dump().size()<=2u*1024*1024);
 std::map<std::string,J> rows;std::map<std::string,std::string> frames;
 for(const auto& p:prior.basis_positions){shape(p,{"key","quantity_exact","average_price_exact","daily_realized_pnl_exact","daily_unrealized_pnl_exact","last_update","basis_evidence"});scope(p.at("key"),owner,owner.source_day);const auto symbol=text(p.at("key").at("symbol"));need(rows.emplace(symbol,p).second&&prior.positions.contains(symbol));const auto& value=prior.positions.at(symbol);need(value.symbol==symbol&&exact(p.at("quantity_exact"))==value.quantity&&exact(p.at("average_price_exact"))==value.average_price&&exact(p.at("daily_realized_pnl_exact"))==value.realized_pnl&&exact(p.at("daily_unrealized_pnl_exact"))==value.unrealized_pnl);need(value.average_price.raw_value()>=0&&(value.quantity.is_zero()||value.average_price.is_positive()));
  const auto& e=p.at("basis_evidence");shape(e,{"source_id","source_digest","price_frame_id","formed_day"});text(e.at("source_id"));hex(e.at("source_digest"));frames[symbol]=text(e.at("price_frame_id"));need(day(text(e.at("formed_day")))<=day(owner.source_day));
 }
 for(const auto& adjustment:original){scope(adjustment.at("key"),owner,owner.source_day);need(rows.contains(text(adjustment.at("key").at("symbol"))));const auto type=text(adjustment.at("type"));need(type=="SPLIT"||type=="ADR_SPLIT"||type=="DIVIDEND");hex(adjustment.at("source_digest"));text(adjustment.at("source_id"));}
 const auto& payload=source.at("payload");shape(payload,{"schema_version","book_id","source_day","previous_day","valuation_time","events"});need(payload.at("schema_version")=="qt-equity-actions-source/v1"&&payload.at("book_id")==owner.portfolio_id&&payload.at("source_day")==owner.valuation_day&&payload.at("previous_day")==owner.source_day&&payload.at("valuation_time")==owner.valuation_day+"T00:00:00Z");
 need(payload.at("events").is_array()&&payload.at("events").size()<=4096);
 EquityModelActionFrame result;result.positions=prior.positions;result.replay_reference=old;
 // Preserve the established no-action bytes. Authority/lease admission remains
 // the SQL caller's responsibility, including detecting any newly added event.
 if(original.empty()&&payload.at("events").empty()&&raw.at("bars").empty()&&raw.at("restating_metadata").empty())return result;
 shape(source,{"source_id","purpose","book_id","source_day","producer_id","policy_version","policy_revision","source_version","content_digest","payload","created_at"});
 need(source.at("purpose")=="actions"&&source.at("book_id")==owner.portfolio_id&&source.at("source_day")==owner.valuation_day);text(source.at("source_id"));need(source.at("source_version")==source.at("source_id"));hex(source.at("content_digest"));need(source.at("content_digest")==hash(payload));text(source.at("created_at"));
 need(policy.is_object()&&policy.at("book_id")==owner.portfolio_id&&policy.at("purpose")=="execution"&&policy.at("enabled")==true);
 need(policy.at("version").is_number_integer()&&!policy.at("version").is_boolean()&&policy.at("version").get<int64_t>()>0&&policy.at("version")==source.at("policy_revision"));for(auto f:{"producer_id","policy_version"})need(text(policy.at(f))==text(source.at(f)));
 std::map<std::string,J> bars;
 for(const auto& row:raw.at("bars")){shape(row,{"symbol","ex_date","raw_close_model_number","split_factor_model_number","dividend_cash_model_number"});const auto symbol=text(row.at("symbol"));need(rows.contains(symbol)&&row.at("ex_date")==owner.valuation_day&&bars.emplace(symbol,row).second);need(number(row.at("raw_close_model_number"))>0);const auto split=number(row.at("split_factor_model_number")),div=number(row.at("dividend_cash_model_number"));need(split>0&&div>=0&&(split!=1||div!=0));}
 std::set<std::pair<std::string,std::string>> metadata_slots;std::map<std::pair<std::string,std::string>,std::string> metadata_types;
 for(const auto& row:raw.at("restating_metadata")){
  shape(row,{"date","action","ticker","value","contraticker","contraname","name"});
  const auto symbol=text(row.at("ticker")),label=text(row.at("action"));need(row.at("date")==owner.valuation_day&&bars.contains(symbol));
  for(auto field:{"value","contraticker","contraname","name"})need(row.at(field).is_null()||(row.at(field).is_string()&&row.at(field).get<std::string>().size()<=4096));
  need(label=="split"||label=="adrratiosplit"||label=="dividend");const bool dividend=label=="dividend";
  need(metadata_slots.emplace(symbol,dividend?"dividend":"split").second);
  metadata_types[{symbol,dividend?"dividend":"split"}]=dividend?"DIVIDEND":label=="adrratiosplit"?"ADR_SPLIT":"SPLIT";
  const auto& bar=bars.at(symbol);need(dividend?number(bar.at("dividend_cash_model_number"))>0:number(bar.at("split_factor_model_number"))!=1);
 }
 std::map<std::string,std::vector<CorpActionEvent>> events;std::set<std::pair<std::string,std::string>> seen;
 std::pair<std::string,int> previous{"",-1};
 for(const auto& row:payload.at("events")){
  shape(row,{"key","type","ex_date","value_model_number","basis_provenance","basis_provenance_evidence","frame_before","frame_after","raw_close_model_number","eligible_quantity_exact"});scope(row.at("key"),owner,owner.valuation_day);const auto symbol=text(row.at("key").at("symbol"));need(rows.contains(symbol)&&bars.contains(symbol)&&row.at("ex_date")==owner.valuation_day);
  CorpActionEvent e;e.symbol=symbol;e.ex_date=owner.valuation_day;const auto type=text(row.at("type"));const bool dividend=type=="DIVIDEND";need(dividend||type=="SPLIT"||type=="ADR_SPLIT");const auto slot=dividend?"dividend":"split";need(seen.emplace(symbol,slot).second);if(metadata_types.contains({symbol,slot}))need(metadata_types.at({symbol,slot})==type);const std::pair<std::string,int> order{symbol,dividend?1:0};need(order>previous);previous=order;
  need(row.at("basis_provenance")=="formed_on_or_before_ex_date"&&row.at("basis_provenance_evidence")==rows.at(symbol).at("basis_evidence").at("source_id"));need(text(row.at("frame_before"))==frames.at(symbol));const auto after=text(row.at("frame_after"));need(after!=frames.at(symbol));frames[symbol]=after;
  e.basis_provenance=CorpActionEvent::BasisProvenance::FORMED_ON_OR_BEFORE_EX_DATE;e.basis_provenance_evidence=text(row.at("basis_provenance_evidence"));e.value=number(row.at("value_model_number"));need(e.value>0&&prior.positions.at(symbol).quantity.is_positive());
  e.type=dividend?CorpActionType::DIVIDEND:(type=="ADR_SPLIT"?CorpActionType::ADR_SPLIT:CorpActionType::SPLIT);
  const auto& bar=bars.at(symbol);
  if(dividend){need(e.value==number(bar.at("dividend_cash_model_number")));e.close_at_ex_date=number(row.at("raw_close_model_number"));need(e.close_at_ex_date>0&&e.close_at_ex_date==number(bar.at("raw_close_model_number")));const auto eligible=exact(row.at("eligible_quantity_exact"));need(eligible.is_positive()&&eligible==prior.positions.at(symbol).quantity);e.qty_at_ex_date=eligible.as_double();}
  else{need(e.value!=1&&e.value==number(bar.at("split_factor_model_number"))&&row.at("raw_close_model_number").is_null()&&row.at("eligible_quantity_exact").is_null());}
  events[symbol].push_back(e);
 }
 for(const auto& [symbol,bar]:bars){need((number(bar.at("split_factor_model_number"))!=1)==seen.contains({symbol,"split"}));need((number(bar.at("dividend_cash_model_number"))!=0)==seen.contains({symbol,"dividend"}));}
 J adjustments=J::array();
 for(const auto& [symbol,list]:events){auto q=prior.positions.at(symbol).quantity,basis=prior.positions.at(symbol).average_price;
  // Screening matches the existing Decimal conversion between stacked events;
  // arithmetic and actual mutation remain owned by the unchanged applier.
  for(const auto& e:list){const auto factor=e.type==CorpActionType::DIVIDEND?1+e.value/e.close_at_ex_date:e.value;need(std::isfinite(factor)&&factor>0);if(e.type!=CorpActionType::DIVIDEND)q=checked(q.as_double()*factor);basis=checked(basis.as_double()/factor);need(q.is_positive()&&basis.is_positive());}
  std::unordered_map<std::string,Position> owned{{symbol,result.positions.at(symbol)}};auto applied=CorporateActionsApplier::apply(owned,list);need(applied.size()==list.size()&&owned.at(symbol).quantity==q&&owned.at(symbol).average_price==basis);result.positions.at(symbol)=owned.at(symbol);
  for(const auto& a:applied)adjustments.push_back({{"symbol",a.symbol},{"event_date",a.event_date},{"type",CorporateActionsApplier::type_to_string(a.type)},{"quantity_before_exact",checked(a.quantity_before).to_string()},{"quantity_after_exact",checked(a.quantity_after).to_string()},{"average_price_before_exact",checked(a.avg_price_before).to_string()},{"average_price_after_exact",checked(a.avg_price_after).to_string()}});
 }
 J derived=J::array();for(const auto& [symbol,p]:rows){auto row=p;row.erase("basis_evidence");row["key"]["date"]=owner.valuation_day;row["key"]["portfolio_type"]="system";row["quantity_exact"]=result.positions.at(symbol).quantity.to_string();row["average_price_exact"]=result.positions.at(symbol).average_price.to_string();row["basis_frame_id"]=frames.at(symbol);row["original_basis_evidence"]=p.at("basis_evidence");derived.push_back(row);}
 J policy_identity;for(auto f:{"book_id","purpose","version","producer_id","policy_version"})policy_identity[f]=policy.at(f);
 result.original_action_count=static_cast<int>(original.size());result.successor_action_count=static_cast<int>(payload.at("events").size());result.original_action_digest=hash(original);result.successor_action_digest=hash(payload.at("events"));
 result.document={{"schema_version","qt-equity-model-action-frame/v1"},{"owner",{{"portfolio_id",owner.portfolio_id},{"strategy_id",owner.strategy_id},{"strategy_name",owner.strategy_name},{"source_day",owner.source_day},{"valuation_day",owner.valuation_day}}},{"original_action_count",result.original_action_count},{"original_action_digest",result.original_action_digest},{"successor_action_count",result.successor_action_count},{"successor_action_digest",result.successor_action_digest},{"original_basis_positions",prior.basis_positions},{"derived_model_basis_positions",derived},{"actions_source",source},{"policy_identity",policy_identity},{"raw_capture",raw},{"adjustments",adjustments}};
 need(result.document.dump().size()<=8u*1024*1024);result.digest=hash(result.document);result.replay_reference["schema_version"]="qt-equity-model-prior/v2";result.replay_reference["action_admission"]="proved_action_adjusted_prior";result.replay_reference["action_frame"]=result.document;result.replay_reference["action_frame_digest"]=result.digest;
 return result;
}catch(const std::exception&){return make_error<EquityModelActionFrame>(ErrorCode::INVALID_DATA,"equity_model_action_frame_unavailable","equity_model_action_frame");}}
}

namespace trade_ngin {
std::vector<std::string> equity_model_action_held_symbols(const std::unordered_map<std::string,Position>& positions){std::vector<std::string> held;for(const auto& [symbol,position]:positions)if(!position.quantity.is_zero())held.push_back(symbol);std::sort(held.begin(),held.end());return held;}
}
