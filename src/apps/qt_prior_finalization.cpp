#include "trade_ngin/apps/qt_prior_finalization.hpp"
#include "trade_ngin/live/pnl_manager_base.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <regex>
#include <stdexcept>

namespace trade_ngin { namespace {
using J=nlohmann::json;
void need(bool ok){if(!ok)throw std::invalid_argument("qt_finalization_unavailable");}
std::string text(const J& value){need(value.is_string());auto s=value.get<std::string>();need(!s.empty()&&s.size()<=4096);return s;}
Decimal dec(const J& value){auto result=parse_qt_quantity_exact(text(value));need(result.is_ok());return result.value();}
double number(const J& value){auto s=text(value);static const std::regex grammar(R"(-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?)");need(s.size()<=64&&std::regex_match(s,grammar));double n=0;auto result=std::from_chars(s.data(),s.data()+s.size(),n,std::chars_format::general);need(result.ec==std::errc{}&&result.ptr==s.data()+s.size()&&std::isfinite(n));return n;}
std::string model(double value){need(std::isfinite(value));if(value==0)return "0";char bytes[64];auto result=std::to_chars(bytes,bytes+sizeof(bytes),value,std::chars_format::general);need(result.ec==std::errc{});return std::string(bytes,result.ptr);}
Decimal cash(double value){need(std::isfinite(value));const double scaled=value*100000000.0+(value>=0?0.5:-0.5);need(std::isfinite(scaled)&&static_cast<long double>(scaled)>=static_cast<long double>(INT64_MIN)&&static_cast<long double>(scaled)<=static_cast<long double>(INT64_MAX));return Decimal::from_raw(static_cast<int64_t>(scaled));}
Decimal add(Decimal a,Decimal b){auto x=a.raw_value(),y=b.raw_value();need(!((y>0&&x>INT64_MAX-y)||(y<0&&x<INT64_MIN-y)));return Decimal::from_raw(x+y);}
J sorted(J rows){need(rows.is_array());std::sort(rows.begin(),rows.end(),[](const J& a,const J& b){return a.dump()<b.dump();});return rows;}
std::map<std::string,J> by(const J& rows,const char* field){need(rows.is_array()&&rows.size()<=4096);std::map<std::string,J> result;for(const auto& r:rows)need(result.emplace(text(r.at(field)),r).second);return result;}
void day(const std::string& s){need(s.size()==10&&s[4]=='-'&&s[7]=='-');for(size_t n=0;n<s.size();++n)if(n!=4&&n!=7)need(s[n]>='0'&&s[n]<='9');using namespace std::chrono;need(year_month_day{year{std::stoi(s.substr(0,4))},month{static_cast<unsigned>(std::stoi(s.substr(5,2)))},std::chrono::day{static_cast<unsigned>(std::stoi(s.substr(8,2)))}}.ok());}
}

Result<J> produce_qt_prior_finalization(const J& d,const J& in,const J& out,const J& market,const J& before,const J& provenance){
 try{
  const auto source_day=text(d.at("source_day")),valuation_day=text(market.at("source_day"));day(source_day);day(valuation_day);need(source_day<valuation_day);
  const auto stamp=valuation_day+"T00:00:00Z";need(market.at("valuation_time")==stamp&&market.at("previous_day")==source_day&&market.at("book_id")==d.at("book_id")&&market.at("currency")==in.at("currency"));
  const bool first=in.at("schema_version")=="qt-futures-accounting-input-first-day/v1";
  const bool v2=in.at("schema_version")=="qt-futures-accounting-input/v2";need(first||v2||in.at("schema_version")=="qt-futures-accounting-input/v1");
  need(market.at("schema_version")=="qt-accounting-market/v1"&&out.at("schema_version")=="qt-futures-accounting/v1");
  const auto& observation=out.at("observation");for(auto f:{"decision_id","book_id","source_day"})need(in.at(f)==d.at(f)&&observation.at(f)==d.at(f));
  need(in.at("timestamp")==source_day+"T00:00:00Z");
  if(first){need(in.at("opening_day")==source_day&&provenance.at("first_day_anchor_id")==in.at("first_day_anchor_id")&&provenance.at("first_day_anchor_digest")==in.at("first_day_anchor_digest"));}
  else need(provenance.at("predecessor_finalization_source_id")==in.at("prior_finalization_source_id"));
  const auto& policy=provenance.at("policy_identity");need(policy.at("book_id")==d.at("book_id")&&policy.at("purpose")=="execution"&&policy.at("version").is_number_integer()&&policy.at("version").get<int64_t>()>0);text(policy.at("producer_id"));text(policy.at("policy_version"));
  auto references=by(in.at("instruments"),"symbol"),marks=by(market.at("instruments"),"symbol"),prior=by(in.at("previous_totals"),"strategy_id"),live=by(out.at("live_results"),"strategy_id");
  need(!live.empty()&&live.size()==prior.size());
  J expected_positions=J::array(),expected_live=J::array(),expected_equity=J::array(),after_positions=J::array(),components=J::array();
  std::map<std::string,double> gross;std::map<std::string,Decimal> costs;std::set<std::string> keys,symbols;
  PnLManagerBase calculator(0);
  auto ordered_fills=observation.at("fills");need(ordered_fills.is_array());std::sort(ordered_fills.begin(),ordered_fills.end(),[](const J& a,const J& b){return a.at("key").dump()<b.at("key").dump();});
  for(const auto& fill:ordered_fills){
   const auto& key=fill.at("key");need(key.is_object()&&key.size()==6&&key.at("portfolio_id")==d.at("book_id")&&key.at("date")==source_day&&key.at("portfolio_type")=="qt");for(auto f:{"portfolio_id","strategy_id","strategy_name","symbol","date","portfolio_type"})text(key.at(f));need(keys.insert(key.dump()).second);
   const auto symbol=text(key.at("symbol")),engine=text(key.at("strategy_id"));need(live.contains(engine)&&references.contains(symbol)&&marks.contains(symbol));symbols.insert(symbol);
   const auto& reference=references.at(symbol);const auto& mark=marks.at(symbol);need(mark.at("instrument_type")=="FUTURE"&&mark.at("price_time")==source_day+"T00:00:00Z");
   auto quantity=dec(fill.at("selected_quantity_exact")),basis=dec(fill.at("average_price_exact")),charge=dec(fill.at("actual_cash_cost_exact"));need(quantity.raw_value()%100000000==0&&basis.raw_value()>0&&charge.raw_value()>=0&&fill.at("currency")==in.at("currency"));
   const auto ref=(v2||first)?number(reference.at("price_model_number")):dec(reference.at("price_exact")).as_double();const auto point=(v2||first)?number(reference.at("point_value")):dec(reference.at("point_value")).as_double();const auto close=number(mark.at("price_model_number"));need(ref>0&&close>0&&point>0&&number(mark.at("point_value"))==point);
   const double pnl=calculator.calculate_daily_pnl(quantity.as_double(),ref,close,point);auto rounded=cash(pnl);gross[engine]+=pnl;need(std::isfinite(gross[engine]));costs[engine]=add(costs[engine],charge);
   J position={{"key",key},{"quantity_exact",fill.at("selected_quantity_exact")},{"average_price_exact",fill.at("average_price_exact")},{"daily_realized_pnl_exact",fill.at("daily_realized_pnl_exact")},{"daily_unrealized_pnl_exact",fill.at("daily_unrealized_pnl_exact")},{"last_update",fill.at("last_update")}};
   Decimal opening_realized,opening_unrealized;
   if(first)for(const auto& opening:in.at("previous_positions"))if(opening.at("key")==key){opening_realized=dec(opening.at("daily_realized_pnl_exact"));opening_unrealized=dec(opening.at("daily_unrealized_pnl_exact"));}
   need(dec(position.at("daily_realized_pnl_exact"))==opening_realized&&dec(position.at("daily_unrealized_pnl_exact"))==opening_unrealized&&position.at("last_update")==in.at("timestamp"));expected_positions.push_back(position);position["daily_realized_pnl_exact"]=add(opening_realized,rounded).to_string();position["last_update"]=stamp;after_positions.push_back(position);
   components.push_back({{"key",key},{"quantity_exact",quantity.to_string()},{"average_price_exact",basis.to_string()},{"reference_price_model_number",model(ref)},{"settlement_price_model_number",model(close)},{"point_value_model_number",model(point)},{"reference_source_id",text(reference.at("source_id"))},{"settlement_source_id",text(mark.at("source_id"))},{"gross_pnl_model_number",model(pnl)},{"gross_pnl_exact",rounded.to_string()}});
  }
  need(!keys.empty()&&symbols.size()==references.size());
  J after_live=J::array(),after_equity=J::array(),totals=J::array();
  for(const auto& [engine,row]:live){
   need(prior.contains(engine)&&gross.contains(engine));const auto& p=prior.at(engine);const auto charge=costs.at(engine),prior_equity=dec(p.at("equity_exact")),prior_pnl=dec(p.at("total_pnl_exact"));need(prior_equity.raw_value()>0);
   const auto opening_daily=first?dec(p.at("daily_pnl_exact")):Decimal(0),opening_cost=first?dec(p.at("daily_transaction_costs_exact")):Decimal(0),opening_realized=first?dec(p.at("daily_realized_pnl_exact")):Decimal(0),opening_unrealized=first?dec(p.at("daily_unrealized_pnl_exact")):Decimal(0);
   need(row.at("portfolio_id")==d.at("book_id")&&row.at("date")==source_day&&row.at("portfolio_type")=="qt"&&dec(row.at("daily_transaction_costs_exact"))==add(opening_cost,charge)&&dec(row.at("daily_pnl_exact"))==add(opening_daily,-charge)&&dec(row.at("total_pnl_exact"))==add(prior_pnl,-charge)&&dec(row.at("current_portfolio_value_exact"))==add(prior_equity,-charge));
   if(first)need(dec(row.at("daily_realized_pnl_exact"))==opening_realized&&dec(row.at("daily_unrealized_pnl_exact"))==opening_unrealized&&dec(row.at("total_transaction_costs_exact"))==add(dec(p.at("total_transaction_costs_exact")),charge));
   J original=row;original["daily_realized_pnl_exact"]=opening_realized.to_string();original["daily_unrealized_pnl_exact"]=opening_unrealized.to_string();expected_live.push_back(original);
   J equity={{"portfolio_id",d.at("book_id")},{"strategy_id",engine},{"timestamp",source_day+"T00:00:00Z"},{"portfolio_type","qt"},{"equity_exact",row.at("current_portfolio_value_exact")}};expected_equity.push_back(equity);
   const auto rounded=cash(gross.at(engine)),net=add(rounded,-charge),value=add(prior_equity,net),total=add(prior_pnl,net);need(value.raw_value()>0);
   original["daily_realized_pnl_exact"]=add(opening_realized,rounded).to_string();original["daily_pnl_exact"]=add(opening_daily,net).to_string();original["total_pnl_exact"]=total.to_string();original["current_portfolio_value_exact"]=value.to_string();after_live.push_back(original);equity["equity_exact"]=value.to_string();after_equity.push_back(equity);
   totals.push_back({{"strategy_id",engine},{"prior_equity_exact",prior_equity.to_string()},{"prior_total_pnl_exact",prior_pnl.to_string()},{"gross_pnl_model_number",model(gross.at(engine))},{"gross_pnl_exact",rounded.to_string()},{"actual_cash_cost_exact",charge.to_string()},{"net_pnl_exact",net.to_string()},{"equity_exact",value.to_string()},{"total_pnl_exact",total.to_string()}});
  }
  need(before.is_object()&&before.size()==3&&sorted(before.at("positions"))==sorted(expected_positions)&&sorted(before.at("live_results"))==sorted(expected_live)&&sorted(before.at("equity_curve"))==sorted(expected_equity));
  J result={{"schema_version","qt-desk-finalization/v1"},{"calculation_version","futures-prior-close-mark/v1"},{"decision_id",d.at("decision_id")},{"book_id",d.at("book_id")},{"source_day",source_day},{"valuation_day",valuation_day},{"valuation_time",stamp},{"currency",in.at("currency")},{"components",components},{"engine_totals",totals},{"before_financial",{{"positions",expected_positions},{"live_results",expected_live},{"equity_curve",expected_equity}}},{"after_financial",{{"positions",after_positions},{"live_results",after_live},{"equity_curve",after_equity}}}};
  for(auto f:{"finalization_id","original_accounting_input_id","original_run_result_digest","original_observation_digest","market_source_id","market_source_digest","unchanged_execution_digest"}){result[f]=text(provenance.at(f));}
  if(first){result["first_day_anchor_id"]=text(provenance.at("first_day_anchor_id"));result["first_day_anchor_digest"]=text(provenance.at("first_day_anchor_digest"));}
  else{result["predecessor_finalization_source_id"]=text(provenance.at("predecessor_finalization_source_id"));result["predecessor_finalization_digest"]=text(provenance.at("predecessor_finalization_digest"));}
  result["policy_identity"]=policy;return result;
 }catch(const std::exception&){return make_error<J>(ErrorCode::INVALID_DATA,"qt_finalization_unavailable","qt_prior_finalization");}
}
}
