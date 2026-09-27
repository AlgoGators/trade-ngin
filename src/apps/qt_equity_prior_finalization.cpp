#include "trade_ngin/apps/qt_equity_prior_finalization.hpp"
#include "trade_ngin/apps/qt_equity_accounting_output.hpp"
#include "trade_ngin/apps/qt_equity_position_transition.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/git_version.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>

namespace trade_ngin { namespace {
using J=nlohmann::json;
void need(bool ok){if(!ok)throw std::invalid_argument("qt_equity_finalization_unavailable");}
void shape(const J& j,std::initializer_list<const char*> fields){need(j.is_object()&&j.size()==fields.size());for(auto f:fields)need(j.contains(f));}
std::string text(const J& j){need(j.is_string());auto s=j.get<std::string>();need(!s.empty()&&s.size()<=4096&&s.find_first_not_of(" \t\r\n")!=std::string::npos);return s;}
std::string uuid(const J& j){const auto s=text(j);static const std::regex r("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}");need(std::regex_match(s,r));return s;}
void digest(const J& j){const auto s=text(j);need(s.size()==64&&s.find_first_not_of("0123456789abcdef")==std::string::npos);}
std::string hash(const J& j,bool output=false){auto bytes=output?canonical_qt_desk_source_json(j):canonical_qt_desk_input_json(j);need(bytes.is_ok());auto h=qt_sha256_hex(bytes.value());need(h.is_ok());return h.value();}
Decimal dec(const J& j){auto n=parse_qt_quantity_exact(text(j));need(n.is_ok());return n.value();}
Decimal add(Decimal a,Decimal b){const auto x=a.raw_value(),y=b.raw_value();need(!((y>0&&x>INT64_MAX-y)||(y<0&&x<INT64_MIN-y)));return Decimal::from_raw(x+y);}
Decimal sub(Decimal a,Decimal b){const auto x=a.raw_value(),y=b.raw_value();need(!((y<0&&x>INT64_MAX+y)||(y>0&&x<INT64_MIN+y)));return Decimal::from_raw(x-y);}
Timestamp day(const std::string& s){
 need(s.size()==10&&s[4]=='-'&&s[7]=='-');for(size_t i=0;i<s.size();++i)if(i!=4&&i!=7)need(s[i]>='0'&&s[i]<='9');
 using namespace std::chrono;const year_month_day date{year{std::stoi(s.substr(0,4))},month{static_cast<unsigned>(std::stoi(s.substr(5,2)))},std::chrono::day{static_cast<unsigned>(std::stoi(s.substr(8,2)))}};need(date.ok());
 const auto days=sys_days{date}.time_since_epoch();const auto ticks=static_cast<long double>(days.count())*86400.0L*Timestamp::duration::period::den/Timestamp::duration::period::num;
 need(ticks>=static_cast<long double>(INT64_MIN)&&ticks<=static_cast<long double>(INT64_MAX));return Timestamp{duration_cast<Timestamp::duration>(days)};
}
double price(const J& row,const std::string& date,const std::string& frame){
 shape(row,{"source_id","source_digest","date","price_frame_id","price_model_number"});text(row.at("source_id"));digest(row.at("source_digest"));need(row.at("date")==date&&row.at("price_frame_id")==frame);
 const auto s=text(row.at("price_model_number"));static const std::regex grammar(R"(-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?)");need(s.size()<=64&&std::regex_match(s,grammar));double n=0;const auto p=std::from_chars(s.data(),s.data()+s.size(),n,std::chars_format::general);need(p.ec==std::errc{}&&p.ptr==s.data()+s.size()&&std::isfinite(n)&&n>0);return n;
}
void array(const J& j){need(j.is_array()&&j.size()<=4096);}
J sorted(J j){array(j);std::sort(j.begin(),j.end(),[](const J& a,const J& b){return a.dump()<b.dump();});return j;}
std::map<std::string,J> by(const J& j,const char* field){array(j);std::map<std::string,J> m;for(const auto& row:j)need(m.emplace(text(row.at(field)),row).second);return m;}
ComponentPositionKey typed(const J& key,const std::string& day){return {text(key.at("portfolio_id")),text(key.at("strategy_id")),text(key.at("strategy_name")),day,text(key.at("symbol")),"qt"};}
}
Result<J> produce_qt_equity_prior_finalization(const J& d,const J& in,const J& out,const J& market,const J& actions,const J& before,const J& provenance){
 try {
  for(const auto* j:{&d,&in,&out,&market,&actions,&before,&provenance})need(j->dump().size()<=1048576);
  const auto source_day=text(d.at("source_day")),valuation_day=text(market.at("source_day")),book=text(d.at("book_id"));uuid(d.at("decision_id"));
  const auto original_time=day(source_day),valuation_time=day(valuation_day);need(original_time<valuation_time);
  const auto stamp=valuation_day+"T00:00:00Z";need(market.at("valuation_time")==stamp&&market.at("previous_day")==source_day&&market.at("book_id")==book);
  shape(market,{"schema_version","calculation_version","book_id","source_day","model_publication_id","previous_day","valuation_time","day_mode","currency","cost_config","instruments","actions_source_id","actions_source_digest"});
  const bool empty_owner=in.at("schema_version")=="qt-equity-accounting-input-empty-owner/v2";
  need(market.at("schema_version")==(empty_owner?"qt-equity-accounting-market-empty-owner/v2":"qt-equity-accounting-market/v1")&&market.at("calculation_version")=="qt-equity-main08b15c/v1"&&market.at("day_mode")=="open"&&market.at("currency")=="USD");uuid(market.at("model_publication_id"));
  need(in.at("schema_version")==(empty_owner?"qt-equity-accounting-input-empty-owner/v2":"qt-equity-accounting-input/v1")&&in.at("calculation_version")==market.at("calculation_version")&&in.at("currency")==market.at("currency")&&in.at("day_mode")=="open"&&in.at("timestamp")==source_day+"T00:00:00Z");
  need(out.at("schema_version")==(empty_owner?"qt-equity-accounting-empty-owner/v2":"qt-equity-accounting/v1")&&out.at("calculation_version")==in.at("calculation_version"));
  const auto& observed=out.at("observation");need(observed.at("schema_version")=="qt-execution/v2");for(auto f:{"decision_id","book_id","source_day"})need(in.at(f)==d.at(f)&&observed.at(f)==d.at(f));
  need(observed.at("accounting_input_id")==in.at("accounting_input_id"));uuid(in.at("accounting_input_id"));
  shape(provenance,{"finalization_id","original_accounting_input_id","original_run_result_digest","original_observation_digest","predecessor_finalization_source_id","predecessor_finalization_digest","market_source_id","market_source_digest","actions_source_id","actions_source_digest","unchanged_execution_digest","policy_identity","finalizer_authority"});
  const auto& authority=provenance.at("finalizer_authority");shape(authority,{"schema_version","book_id","source_day","model_publication_id","snapshot_id","source_version","as_of","valid_until","content_digest","producer_id","policy_version","policy_revision","policy_updated_at","evaluator_build","evaluator_sha256","evaluator_bundle_sha256","allowed_override_codes"});
  need(authority==out.at("producer_authority")&&authority.at("schema_version")=="qt-input-authority/v1"&&authority.at("evaluator_build")==std::string(TRADE_NGIN_GIT_SHA));for(auto f:{"book_id","source_day"})need(authority.at(f)==d.at(f));if(d.contains("model_publication_id"))need(authority.at("model_publication_id")==d.at("model_publication_id"));digest(authority.at("evaluator_sha256"));digest(authority.at("evaluator_bundle_sha256"));
  if(empty_owner) {
   // Recompute the original using its retained authority and the actual pure
   // accounting kernel. A consistently rehashed stored output is not authority.
   auto original=recompute_qt_equity_accounting_output(d,J::array(),in,out.at("producer_authority"));
   need(original.is_ok()&&original.value()==out);
   for(auto field:{"previous_positions","instruments","actions"})need(in.at(field).is_array()&&in.at(field).empty());
   for(auto field:{"executions","corporate_action_adjustments","distance","layers_applied"})need(out.at(field).is_array()&&out.at(field).empty());
   need(market.at("instruments").is_array()&&market.at("instruments").empty());
  }
  uuid(provenance.at("finalization_id"));uuid(provenance.at("market_source_id"));need(provenance.at("original_accounting_input_id")==in.at("accounting_input_id")&&provenance.at("original_run_result_digest")==hash(out,true)&&provenance.at("original_observation_digest")==hash(observed)&&provenance.at("unchanged_execution_digest")==hash(out.at("executions")));
  need(provenance.at("predecessor_finalization_source_id")==in.at("prior_finalization_source_id")&&provenance.at("predecessor_finalization_digest")==in.at("prior_finalization_digest")&&provenance.at("market_source_digest")==hash(market));
  shape(actions,{"schema_version","book_id","source_day","previous_day","valuation_time","events"});need(actions.at("schema_version")=="qt-equity-actions-source/v1");for(auto f:{"book_id","source_day","previous_day","valuation_time"})need(actions.at(f)==market.at(f));array(actions.at("events"));need(actions.at("events").empty());
  need(provenance.at("actions_source_id")==market.at("actions_source_id")&&provenance.at("actions_source_digest")==market.at("actions_source_digest")&&market.at("actions_source_digest")==hash(actions));text(market.at("actions_source_id"));
  const auto& policy=provenance.at("policy_identity");shape(policy,{"book_id","purpose","version","producer_id","policy_version"});need(policy.at("book_id")==book&&policy.at("purpose")=="execution"&&policy.at("version").is_number_integer()&&policy.at("version").get<int64_t>()>0);text(policy.at("producer_id"));text(policy.at("policy_version"));
  auto original_markets=by(in.at("instruments"),"symbol"),marks=by(market.at("instruments"),"symbol"),prior=by(in.at("previous_totals"),"strategy_id"),live=by(out.at("live_results"),"strategy_id");need(!live.empty()&&live.size()==prior.size());
  std::map<std::string,std::string> frames;array(out.at("corporate_action_adjustments"));for(const auto& a:out.at("corporate_action_adjustments"))frames[a.at("key").dump()]=text(a.at("frame_after"));
  J original_positions=J::array(),after_positions=J::array(),components=J::array();std::set<std::string> owners,symbols,engines;std::map<std::string,Decimal> unrealized,original_unrealized,gross,charges;
  if(empty_owner) {
   need(prior.size()==1&&live.size()==1&&prior.contains("LIVE_EQUITY_MEAN_REVERSION")&&live.contains("LIVE_EQUITY_MEAN_REVERSION"));
   // The engine comes from the proved original financial rows. These are the
   // identity contributions of an actually empty position/fill sum, not capital
   // defaults or an invented observation.
   for(const auto& [engine,row]:live){engines.insert(engine);unrealized.emplace(engine,Decimal{});original_unrealized.emplace(engine,Decimal{});gross.emplace(engine,Decimal{});charges.emplace(engine,Decimal{});}
  }
  auto fills=observed.at("fills");array(fills);need(empty_owner?fills.empty():!fills.empty());std::sort(fills.begin(),fills.end(),[](const J& a,const J& b){return a.at("key").dump()<b.at("key").dump();});
  for(const auto& fill:fills){
   const auto& key=fill.at("key");shape(key,{"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"});need(key.at("portfolio_id")==book&&key.at("date")==source_day&&key.at("portfolio_type")=="qt");const auto owner=typed(key,valuation_day);need(owners.insert(key.dump()).second);
   need(live.contains(owner.strategy_id)&&original_markets.contains(owner.symbol)&&marks.contains(owner.symbol));symbols.insert(owner.symbol);engines.insert(owner.strategy_id);
   const auto quantity=dec(fill.at("selected_quantity_exact")),basis=dec(fill.at("average_price_exact"));need(!quantity.is_negative()&&!basis.is_negative()&&(quantity.is_zero()||basis.is_positive())&&fill.at("currency")=="USD"&&fill.at("last_update")==in.at("timestamp"));
   const auto& m=marks.at(owner.symbol);need(m.at("asset_type")=="EQUITY");const auto frame=frames.contains(key.dump())?frames.at(key.dump()):text(original_markets.at(owner.symbol).at("mark").at("price_frame_id"));
   const auto reference=price(m.at("reference"),source_day,frame),mark=price(m.at("mark"),source_day,frame);
   Position previous{owner.symbol,quantity,basis,dec(fill.at("daily_unrealized_pnl_exact")),dec(fill.at("daily_realized_pnl_exact")),original_time};
   const auto transition=produce_qt_equity_position_transition(owner,previous,quantity,reference,mark,valuation_time,{});need(transition.is_ok());const auto& actual=transition.value();need(actual.quantity_change.is_zero()&&actual.gross_trade_realized_pnl.is_zero()&&actual.adjustments.empty()&&actual.position.quantity==quantity&&actual.position.average_price==basis);
   J p={{"key",key},{"quantity_exact",quantity.to_string()},{"average_price_exact",basis.to_string()},{"daily_realized_pnl_exact",fill.at("daily_realized_pnl_exact")},{"daily_unrealized_pnl_exact",fill.at("daily_unrealized_pnl_exact")},{"last_update",fill.at("last_update")}};original_positions.push_back(p);p["daily_unrealized_pnl_exact"]=actual.position.unrealized_pnl.to_string();p["last_update"]=stamp;after_positions.push_back(p);
   unrealized[owner.strategy_id]=add(unrealized[owner.strategy_id],actual.position.unrealized_pnl);original_unrealized[owner.strategy_id]=add(original_unrealized[owner.strategy_id],previous.unrealized_pnl);gross[owner.strategy_id]=add(gross[owner.strategy_id],previous.realized_pnl);const auto charge=dec(fill.at("actual_cash_cost_exact"));need(!charge.is_negative());charges[owner.strategy_id]=add(charges[owner.strategy_id],charge);
   components.push_back({{"key",key},{"quantity_exact",quantity.to_string()},{"average_price_exact",basis.to_string()},{"price_frame_id",frame},{"close_price_model_number",m.at("mark").at("price_model_number")},{"close_source_id",m.at("mark").at("source_id")},{"close_source_digest",m.at("mark").at("source_digest")},{"prior_unrealized_pnl_exact",previous.unrealized_pnl.to_string()},{"current_unrealized_pnl_exact",actual.position.unrealized_pnl.to_string()}});
  }
  need(engines.size()==live.size()&&symbols.size()==original_markets.size());
  J after_live=J::array(),after_curve=J::array(),expected_curve=J::array(),totals=J::array();
  for(const auto& [engine,row]:live){
   const auto& p=prior.at(engine);need(row.at("portfolio_id")==book&&row.at("date")==source_day&&row.at("portfolio_type")=="qt");
   const auto capital=dec(p.at("initial_capital_exact")),realized=dec(row.at("total_realized_pnl_exact")),cost=dec(row.at("total_transaction_costs_exact"));need(capital.is_positive()&&!cost.is_negative());
   need(dec(row.at("initial_capital_exact"))==capital&&realized==add(dec(p.at("total_realized_pnl_exact")),gross.at(engine))&&cost==add(dec(p.at("total_transaction_costs_exact")),charges.at(engine))&&dec(row.at("daily_realized_pnl_exact"))==gross.at(engine)&&dec(row.at("daily_transaction_costs_exact"))==charges.at(engine));
   const auto original_flow=sub(original_unrealized.at(engine),dec(p.at("total_unrealized_pnl_exact"))),original_pnl=add(sub(realized,cost),original_unrealized.at(engine));need(dec(row.at("total_unrealized_pnl_exact"))==original_unrealized.at(engine)&&dec(row.at("daily_unrealized_pnl_exact"))==original_flow&&dec(row.at("daily_pnl_exact"))==add(sub(gross.at(engine),charges.at(engine)),original_flow)&&dec(row.at("total_pnl_exact"))==original_pnl&&dec(row.at("current_portfolio_value_exact"))==add(capital,original_pnl));
   const auto flow=sub(unrealized.at(engine),dec(p.at("total_unrealized_pnl_exact"))),pnl=add(sub(realized,cost),unrealized.at(engine)),equity=add(capital,pnl);need(equity.is_positive());auto updated=row;updated["total_unrealized_pnl_exact"]=unrealized.at(engine).to_string();updated["daily_unrealized_pnl_exact"]=flow.to_string();updated["daily_pnl_exact"]=add(sub(gross.at(engine),charges.at(engine)),flow).to_string();updated["total_pnl_exact"]=pnl.to_string();updated["current_portfolio_value_exact"]=equity.to_string();after_live.push_back(updated);
   J curve={{"portfolio_id",book},{"strategy_id",engine},{"timestamp",in.at("timestamp")},{"portfolio_type","qt"},{"equity_exact",row.at("current_portfolio_value_exact")}};expected_curve.push_back(curve);curve["equity_exact"]=equity.to_string();after_curve.push_back(curve);
   totals.push_back({{"strategy_id",engine},{"initial_capital_exact",capital.to_string()},{"equity_exact",equity.to_string()},{"total_pnl_exact",pnl.to_string()},{"total_realized_pnl_exact",realized.to_string()},{"total_transaction_costs_exact",cost.to_string()},{"total_unrealized_pnl_exact",unrealized.at(engine).to_string()}});
  }
  shape(before,{"positions","live_results","equity_curve"});need(sorted(before.at("positions"))==sorted(original_positions)&&sorted(before.at("live_results"))==sorted(out.at("live_results"))&&sorted(before.at("equity_curve"))==sorted(expected_curve)&&sorted(out.at("equity_curve"))==sorted(expected_curve));
  J result={{"schema_version",empty_owner?"qt-equity-desk-finalization-empty-owner/v2":"qt-equity-desk-finalization/v1"},{"calculation_version","equity-prior-close-mark/v1"},{"decision_id",d.at("decision_id")},{"book_id",book},{"source_day",source_day},{"valuation_day",valuation_day},{"valuation_time",stamp},{"currency","USD"},{"components",components},{"engine_totals",totals},{"before_financial",{{"positions",original_positions},{"live_results",out.at("live_results")},{"equity_curve",expected_curve}}},{"after_financial",{{"positions",after_positions},{"live_results",after_live},{"equity_curve",after_curve}}}};
  for(auto it=provenance.begin();it!=provenance.end();++it)result[it.key()]=it.value();return result;
 }catch(const std::exception&){return make_error<J>(ErrorCode::INVALID_DATA,"qt_equity_finalization_unavailable","qt_equity_prior_finalization");}
}
}
