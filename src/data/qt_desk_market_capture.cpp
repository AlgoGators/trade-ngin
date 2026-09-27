#include "trade_ngin/data/qt_desk_market_capture.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/qt_desk_upstream.hpp"
#include "trade_ngin/data/qt_desk_publication.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/git_version.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/live/execution_manager.hpp"
#include <charconv>
#include <algorithm>
#include <regex>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
namespace trade_ngin { namespace {
using J=nlohmann::json;
void require(bool valid){if(!valid)throw std::invalid_argument("market_capture_unavailable");}
std::string string(const J& v){require(v.is_string());auto s=v.get<std::string>();require(!s.empty()&&s.size()<4096);return s;}
J one(pqxx::work& tx,const std::string& sql){auto r=tx.exec(sql);require(r.size()==1&&!r[0][0].is_null());return J::parse(r[0][0].c_str());}
std::string digest(const J& v){auto b=canonical_qt_desk_input_json(v);require(b.is_ok());auto h=qt_sha256_hex(b.value());require(h.is_ok());return h.value();}
std::string numeric(double d){require(std::isfinite(d));if(d==0)return "0";char b[96];auto r=std::to_chars(b,b+sizeof b,d,std::chars_format::general);require(r.ec==std::errc{});return {b,r.ptr};}
Timestamp day(const std::string& s){require(s.size()==10&&s[4]=='-'&&s[7]=='-');auto ymd=std::chrono::year_month_day(std::chrono::year(std::stoi(s.substr(0,4))),std::chrono::month(std::stoi(s.substr(5,2))),std::chrono::day(std::stoi(s.substr(8,2))));require(ymd.ok());return std::chrono::sys_days(ymd);}
void safe_bar_number(double d){const long double scaled=static_cast<long double>(d*1e8+0.5);require(std::isfinite(d)&&d>0&&scaled<=static_cast<long double>(std::numeric_limits<int64_t>::max()));}
}
Result<J> capture_qt_desk_market_source(pqxx::connection& c,const J& request){try{
 require(request.is_object()&&request.size()==5);for(auto f:{"source_id","decision_id","prior_decision_id","as_of","valid_until"})string(request.at(f));
 J source;
 {pqxx::work tx(c);tx.exec("SET LOCAL TIME ZONE 'UTC'");
 auto d=one(tx,"SELECT to_jsonb(d) FROM trading.qt_decisions d WHERE decision_id="+tx.quote(string(request.at("decision_id")))+"::uuid");
 auto p=one(tx,"SELECT to_jsonb(p) FROM trading.qt_previews p WHERE preview_id="+tx.quote(string(d.at("preview_id")))+"::uuid");require(validate_qt_desk_decision_snapshot(d,p).is_ok());
 auto prior=one(tx,"SELECT to_jsonb(d) FROM trading.qt_decisions d WHERE decision_id="+tx.quote(string(request.at("prior_decision_id")))+"::uuid");require(prior.at("book_id")==d.at("book_id"));
 auto prior_input=one(tx,"SELECT to_jsonb(i) FROM trading.qt_desk_accounting_inputs i JOIN trading.desk_run_results r ON r.input_id=i.input_id JOIN trading.qt_desk_receipts receipt ON receipt.decision_id=r.decision_id AND receipt.status='processed' WHERE r.decision_id="+tx.quote(string(prior.at("decision_id")))+"::uuid");
 require(digest(prior_input.at("payload"))==string(prior_input.at("content_digest")));
 const auto current=day(string(d.at("source_day"))),previous=day(string(prior.at("source_day")));require(current-previous==std::chrono::hours(24));
 auto policy=one(tx,"SELECT to_jsonb(p) FROM trading.qt_source_policies p WHERE purpose='execution' AND book_id="+tx.quote(string(d.at("book_id")))+" FOR SHARE");require(policy.at("enabled")==true);
 std::set<std::string> symbols;for(const auto& row:p.at("payload").at("selection_rows")){require(row.at("asset_type")=="FUTURE");symbols.insert(string(row.at("key").at("symbol")));}
 auto output=one(tx,"SELECT to_jsonb(r) FROM trading.desk_run_results r WHERE decision_id="+tx.quote(string(prior.at("decision_id")))+"::uuid");require(digest(output.at("payload"))==string(output.at("content_digest"))&&output.at("payload").at("input_digest")==prior_input.at("content_digest"));
 for(const auto& fill:output.at("payload").at("observation").at("fills"))symbols.insert(string(fill.at("key").at("symbol")));
 require(!symbols.empty()&&symbols.size()<=1000);std::vector<std::string> ordered(symbols.begin(),symbols.end());
 auto snapshot=PostgresDatabase::read_market_data_snapshot(tx,ordered,previous,current-std::chrono::seconds(1),AssetClass::FUTURES,DataFrequency::DAILY,"ohlcv");require(snapshot.is_ok()&&static_cast<size_t>(snapshot.value().rows.size())==symbols.size());
 // Validate before the existing Bar constructor performs Decimal(double).
 J raw_rows=J::array();std::set<std::string> seen;
 for(const auto& row:snapshot.value().rows){for(auto field:{"time","symbol","open","high","low","close","volume"})require(!row[field].is_null());auto symbol=row["symbol"].as<std::string>();require(symbols.contains(symbol)&&seen.insert(symbol).second);
  for(auto f:{"open","high","low","close"})safe_bar_number(row[f].as<double>());
  const auto volume=row["volume"].as<double>();require(std::isfinite(volume)&&volume>0);
  require(row["low"].as<double>()<=row["open"].as<double>()&&row["low"].as<double>()<=row["close"].as<double>()&&row["high"].as<double>()>=row["open"].as<double>()&&row["high"].as<double>()>=row["close"].as<double>());
  auto raw_time=row["time"].as<std::string>();require(raw_time.size()>=22&&raw_time.ends_with("+00"));auto time=raw_time.substr(0,raw_time.size()-3)+"Z";time[10]='T';
  J value={{"symbol",symbol},{"source_time",time},{"source_time_sql",raw_time}};for(auto f:{"open","high","low","close","volume"})value[f]=row[f].as<std::string>();raw_rows.push_back(value);
 }
 auto bars=DataConversionUtils::arrow_table_to_bars(snapshot.value().table);require(bars.is_ok()&&bars.value().size()==symbols.size());
 transaction_cost::TransactionCostManager::Config cfg;ExecutionManager execution(cfg);J instruments=J::array(),consumed=J::array(),asset_configs=J::array();
 std::map<std::string,Bar> by_symbol;for(const auto& bar:bars.value()){require(std::chrono::floor<std::chrono::days>(bar.timestamp)==previous);require(by_symbol.emplace(bar.symbol,bar).second);}
 J costs={{"explicit_fee_per_contract",numeric(cfg.explicit_fee_per_contract)},{"min_adv",numeric(cfg.impact_config.min_adv)},{"min_participation",numeric(cfg.impact_config.min_participation)},{"max_participation",numeric(cfg.impact_config.max_participation)}};
 for(const auto& symbol:ordered){const auto& bar=by_symbol.at(symbol);const auto price=bar.close.as_double();require(price>0);execution.update_market_data(symbol,bar.volume,price);
  auto& manager=execution.get_transaction_cost_manager();transaction_cost::AssetLookupObservation lookup;auto asset=manager.get_asset_config(symbol,&lookup);require(lookup.path.has_value()&&*lookup.path!=transaction_cost::AssetLookupPath::fallback&&asset.asset_type==AssetType::FUTURE);
  auto adv=manager.get_adv(symbol),vol=manager.get_volatility_multiplier(symbol);require(std::isfinite(adv)&&adv>0&&std::isfinite(vol)&&vol>0);
  J raw;for(const auto& r:raw_rows)if(r.at("symbol")==symbol)raw=r;auto history_digest=digest(J::array({raw}));
  J item={{"symbol",symbol},{"instrument_type","FUTURE"},{"price_model_number",numeric(price)},{"price_time",string(prior.at("source_day"))+"T00:00:00Z"},{"source_id","postgres:futures_data.ohlcv_1d/"+history_digest},{"adv_model_number",numeric(adv)},{"volatility_multiplier_model_number",numeric(vol)},{"history_source_id","legacy-latest-bar/"+history_digest},{"history_digest",history_digest},{"history_observation_count",1},{"history_complete",true},{"asset_lookup",*lookup.path==transaction_cost::AssetLookupPath::exact_symbol?"exact_symbol":"pre_dot_root"}};
  item["baseline_spread_ticks"]=numeric(asset.baseline_spread_ticks);item["min_spread_ticks"]=numeric(asset.min_spread_ticks);item["max_spread_ticks"]=numeric(asset.max_spread_ticks);item["spread_cost_multiplier"]=numeric(asset.spread_cost_multiplier);item["max_impact_bps"]=numeric(asset.max_impact_bps);item["tick_size"]=numeric(asset.tick_size);item["point_value"]=numeric(asset.point_value);item["max_total_implicit_bps"]=numeric(asset.max_total_implicit_bps);instruments.push_back(item); J asset_fields={{"symbol",symbol},{"instrument_type","FUTURE"},{"asset_lookup",item.at("asset_lookup")}};for(auto f:{"baseline_spread_ticks","min_spread_ticks","max_spread_ticks","spread_cost_multiplier","max_impact_bps","tick_size","point_value","max_total_implicit_bps"})asset_fields[f]=item.at(f);asset_configs.push_back(asset_fields);
  raw["consumed_close_model_number"]=numeric(price);raw["consumed_volume_model_number"]=numeric(bar.volume);raw["volatility_state"]="insufficient_returns_neutral";consumed.push_back(raw);
 }
 J effective={{"cost_config",costs},{"adv_lookback_days",cfg.impact_config.adv_lookback_days},{"volatility_lookback_days",cfg.spread_config.lookback_days},{"volatility_lambda",numeric(cfg.spread_config.lambda)},{"volatility_min_multiplier",numeric(cfg.spread_config.min_multiplier)},{"volatility_max_multiplier",numeric(cfg.spread_config.max_multiplier)},{"instruments",asset_configs},{"engine_build",TRADE_NGIN_GIT_SHA}};
 const auto data_hash=digest(raw_rows),cost_hash=digest(effective);
 J payload={{"schema_version","qt-accounting-market/v1"},{"book_id",d.at("book_id")},{"source_day",d.at("source_day")},{"previous_day",prior.at("source_day")},{"valuation_time",string(d.at("source_day"))+"T00:00:00Z"},{"currency",prior_input.at("payload").at("currency")},{"model_publication_id",d.at("model_publication_id")},{"dataset_source_id","postgres:futures_data.ohlcv_1d/"+data_hash},{"dataset_digest",data_hash},{"cost_config_source_id","legacy-futures-default-cost/"+cost_hash},{"cost_config_digest",cost_hash},{"engine_build",TRADE_NGIN_GIT_SHA},{"cost_config",costs},{"instruments",instruments},{"capture",{{"schema_version","qt-market-reader-capture/v1"},{"convention","legacy-futures-fresh-manager-one-bar/v1"},{"table","futures_data.ohlcv_1d"},{"rows",consumed},{"effective_configuration",effective}}}};
 source={{"source_id",request.at("source_id")},{"source_version","qt-market-capture/"+string(request.at("source_id"))},{"producer_id",policy.at("producer_id")},{"policy_version",policy.at("policy_version")},{"as_of",request.at("as_of")},{"valid_until",request.at("valid_until")},{"payload",payload}};tx.commit();}
 return publish_qt_desk_market_source(c,source);
 }catch(const std::exception&){return make_error<J>(ErrorCode::INVALID_DATA,"qt_market_capture_unavailable","qt_desk_market_capture");}}
namespace {
void capture_shape(const J& value,std::initializer_list<const char*> fields){require(value.is_object()&&value.size()==fields.size());for(auto f:fields)require(value.contains(f));}
double captured_number(const J& value){auto s=string(value);static const std::regex grammar(R"(-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?)");require(s.size()<=64&&std::regex_match(s,grammar));double d=0;auto r=std::from_chars(s.data(),s.data()+s.size(),d,std::chars_format::general);require(r.ec==std::errc{}&&r.ptr==s.data()+s.size()&&std::isfinite(d));return d;}
int64_t capture_time(const J& row,const std::string& previous){
 auto s=string(row.at("source_time")),sql=string(row.at("source_time_sql"));
 static const std::regex grammar(R"([0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]{1,6})?Z)");require(std::regex_match(s,grammar)&&s.substr(0,10)==previous);
 auto expected=s.substr(0,s.size()-1)+"+00";expected[10]=' ';require(expected==sql);
 auto hour=std::stoi(s.substr(11,2)),minute=std::stoi(s.substr(14,2)),second=std::stoi(s.substr(17,2));require(hour<24&&minute<60&&second<60);
 int64_t micros=0;if(s.size()>20){auto fraction=s.substr(20,s.size()-21);while(fraction.size()<6)fraction+='0';micros=std::stoll(fraction);}return ((hour*60+minute)*60+second)*1000000LL+micros;
}
}
Result<void> validate_qt_desk_market_capture(const J& row){try{
 const auto& market=row.at("payload");const auto version=string(row.at("source_version"));const bool claimed=version.starts_with("qt-market-capture/");require(claimed==market.contains("capture"));if(!claimed)return Result<void>();
 const auto id=string(row.at("source_id"));static const std::regex uuid(R"([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})");require(std::regex_match(id,uuid)&&version=="qt-market-capture/"+id);
 const auto& capture=market.at("capture");capture_shape(capture,{"schema_version","convention","table","rows","effective_configuration"});require(capture.at("schema_version")=="qt-market-reader-capture/v1"&&capture.at("convention")=="legacy-futures-fresh-manager-one-bar/v1"&&capture.at("table")=="futures_data.ohlcv_1d");
 std::map<std::string,J> instruments,raw_rows,expected_assets;require(market.at("instruments").is_array()&&capture.at("rows").is_array()&&capture.at("rows").size()<=4096);
 for(const auto& item:market.at("instruments"))require(instruments.emplace(string(item.at("symbol")),item).second);
 for(const auto& value:capture.at("rows")){
  capture_shape(value,{"symbol","source_time","source_time_sql","open","high","low","close","volume","consumed_close_model_number","consumed_volume_model_number","volatility_state"});const auto symbol=string(value.at("symbol"));require(instruments.contains(symbol));const auto& item=instruments.at(symbol);capture_time(value,string(market.at("previous_day")));
  J raw;for(auto f:{"symbol","source_time","source_time_sql","open","high","low","close","volume"})raw[f]=value.at(f);require(raw_rows.emplace(symbol,raw).second);
  std::map<std::string,double> values;for(auto f:{"open","high","low","close","volume"}){auto d=captured_number(raw.at(f));require(d>0);values[f]=d;}
  require(values.at("low")<=std::min(values.at("open"),values.at("close"))&&values.at("high")>=std::max(values.at("open"),values.at("close")));
  for(auto f:{"open","high","low","close"}){safe_bar_number(values.at(f));require(Decimal(values.at(f)).raw_value()>0);}const auto close=Decimal(values.at("close")).as_double();
  require(captured_number(value.at("consumed_close_model_number"))==close&&captured_number(item.at("price_model_number"))==close&&captured_number(value.at("consumed_volume_model_number"))==values.at("volume")&&captured_number(item.at("adv_model_number"))==values.at("volume"));
  require(value.at("volatility_state")=="insufficient_returns_neutral"&&captured_number(item.at("volatility_multiplier_model_number"))==1&&item.at("history_observation_count")==1);
  const auto history=digest(J::array({raw}));require(item.at("history_digest")==history&&item.at("history_source_id")=="legacy-latest-bar/"+history&&item.at("source_id")=="postgres:futures_data.ohlcv_1d/"+history);
  J asset;for(auto f:{"symbol","instrument_type","asset_lookup","baseline_spread_ticks","min_spread_ticks","max_spread_ticks","spread_cost_multiplier","max_impact_bps","tick_size","point_value","max_total_implicit_bps"})asset[f]=item.at(f);expected_assets.emplace(symbol,asset);
 }
 require(raw_rows.size()==instruments.size());J ordered=J::array();for(const auto& [_,value]:raw_rows)ordered.push_back(value);const auto previous=string(market.at("previous_day"));std::sort(ordered.begin(),ordered.end(),[&](const J& a,const J& b){return std::pair{capture_time(a,previous),string(a.at("symbol"))}<std::pair{capture_time(b,previous),string(b.at("symbol"))};});
 const auto data=digest(ordered);require(market.at("dataset_digest")==data&&market.at("dataset_source_id")=="postgres:futures_data.ohlcv_1d/"+data);
 const auto& effective=capture.at("effective_configuration");capture_shape(effective,{"cost_config","adv_lookback_days","volatility_lookback_days","volatility_lambda","volatility_min_multiplier","volatility_max_multiplier","instruments","engine_build"});require(effective.at("engine_build")==market.at("engine_build")&&effective.at("cost_config")==market.at("cost_config"));
 for(auto f:{"adv_lookback_days","volatility_lookback_days"})require(effective.at(f).is_number_integer()&&effective.at(f).get<int64_t>()>0);
 require(captured_number(effective.at("volatility_lambda"))>=0&&captured_number(effective.at("volatility_min_multiplier"))>0&&captured_number(effective.at("volatility_min_multiplier"))<=captured_number(effective.at("volatility_max_multiplier")));
 std::map<std::string,J> actual_assets;require(effective.at("instruments").is_array());for(const auto& item:effective.at("instruments"))require(actual_assets.emplace(string(item.at("symbol")),item).second);require(actual_assets==expected_assets);
 const auto cost=digest(effective);require(market.at("cost_config_digest")==cost&&market.at("cost_config_source_id")=="legacy-futures-default-cost/"+cost);return Result<void>();
 }catch(const std::exception&){return make_error<void>(ErrorCode::INVALID_DATA,"qt_market_capture_proof_unavailable","qt_desk_market_capture");}}

}
