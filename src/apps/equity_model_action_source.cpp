#include "trade_ngin/apps/equity_model_action_source.hpp"
#include "trade_ngin/live/corporate_actions_classification.hpp"
#include <algorithm>
#include <stdexcept>
namespace trade_ngin { namespace {
using J=nlohmann::json;
void need(bool b){if(!b)throw std::invalid_argument("equity_model_action_source_unavailable");}
std::string text(const J& j){need(j.is_string());auto s=j.get<std::string>();need(!s.empty()&&s.size()<=4096);return s;}
J rows(pqxx::work& tx,const std::string& sql){
    J out=J::array();std::size_t bytes=0;
    for(const auto& row:tx.exec(sql)){need(out.size()<4096&&!row[0].is_null());const std::string s=row[0].c_str();need(s.size()<=2097152&&bytes<=2097152-s.size());bytes+=s.size();out.push_back(J::parse(s));}
    return out;
}
J one(pqxx::work& tx,const std::string& sql){auto r=rows(tx,sql);need(r.size()==1);return r[0];}
std::string bounded(const std::string& value){return "CASE WHEN octet_length(("+value+")::text)<=2097152 THEN "+value+" ELSE NULL END";}
}
Result<EquityModelActionFrame> capture_equity_model_action_frame(pqxx::work& tx,
    const VerifiedEquityModelPrior& prior,const EquityModelPriorOwner& o,
    const J& original,const J& finalization){try{
    const auto held=equity_model_action_held_symbols(prior.positions);
    J raw={{"bars",J::array()},{"aliases",J::array()},{"terminations",J::array()},{"restating_metadata",J::array()}};
    if(!held.empty()){
        const auto symbols=tx.quote(held),start=tx.quote(o.source_day),end=tx.quote(o.valuation_day);
        // The proved S successor already incorporates its original S actions.
        // Only events strictly after S may form a new D MODEL basis. Older
        // actions cannot be fed back into the copied holdings a second time.
        raw["bars"]=rows(tx,"SELECT jsonb_build_object('symbol',symbol,'ex_date',(time AT TIME ZONE 'UTC')::date::text,'raw_close_model_number',close::text,'split_factor_model_number',COALESCE(split_factor,1)::text,'dividend_cash_model_number',COALESCE(div_cash,0)::text) FROM equities_data.ohlcv_1d WHERE symbol=ANY("+symbols+") AND time>=(("+start+"::date+INTERVAL '1 day') AT TIME ZONE 'UTC') AND time<(("+end+"::date+INTERVAL '1 day') AT TIME ZONE 'UTC') AND (COALESCE(div_cash,0)<>0 OR COALESCE(split_factor,1) NOT IN (0,1)) ORDER BY time,symbol LIMIT 4097 FOR SHARE");
        raw["aliases"]=rows(tx,"SELECT "+bounded("to_jsonb(a)")+" FROM equities_data.ticker_aliases a WHERE historical_ticker=ANY("+symbols+") OR current_symbol=ANY("+symbols+") ORDER BY historical_ticker,current_symbol LIMIT 4097 FOR SHARE");
        raw["terminations"]=rows(tx,"SELECT jsonb_build_object('symbol',symbol,'delisting_date',delisting_date::text) FROM equities_data.ohlcv_1d WHERE symbol=ANY("+symbols+") AND delisting_date>="+start+"::date AND delisting_date<="+end+"::date ORDER BY symbol,time LIMIT 4097 FOR SHARE");
        const auto terms=rows(tx,"SELECT jsonb_build_object('date',date,'action',action,'ticker',ticker,'value',value,'contraticker',contraticker,'contraname',contraname,'name',name) FROM equities_data.corporate_action a WHERE ticker=ANY("+symbols+") AND date>"+start+" AND date<="+end+" ORDER BY date,ticker,action LIMIT 4097 FOR SHARE");
        for(const auto& term:terms){const auto label=text(term.at("action"));const auto kind=classify_action(label);
            if(kind==CorpActionClass::INFORMATIONAL)continue;
            if(kind==CorpActionClass::PRICE_RESTATING&&(label=="split"||label=="adrratiosplit"||label=="dividend")){
                raw["restating_metadata"].push_back(term);continue;
            }
            raw["terminations"].push_back(term); // unsupported labels remain explicit refusal operands
        }
        need(raw["aliases"].empty()&&raw["terminations"].empty());
    }
    // A nonempty D source is a distinct governed role from the unchanged empty
    // D source used to mark/finalize S. Never choose the first valid candidate.
    const auto policy=one(tx,"SELECT "+bounded("to_jsonb(p)")+" FROM trading.qt_source_policies p WHERE book_id="+tx.quote(o.portfolio_id)+" AND purpose='execution' FOR SHARE");
    need(policy.at("enabled")==true&&policy.at("version").is_number_integer()&&policy.at("version").get<int64_t>()>0);
    const auto candidates=rows(tx,"SELECT "+bounded("to_jsonb(a)")+" FROM trading.qt_equity_desk_evidence_sources a WHERE book_id="+tx.quote(o.portfolio_id)+" AND source_day="+tx.quote(o.valuation_day)+"::date AND purpose='actions' AND payload->>'previous_day'="+tx.quote(o.source_day)+" AND producer_id="+tx.quote(text(policy.at("producer_id")))+" AND policy_version="+tx.quote(text(policy.at("policy_version")))+" AND policy_revision="+tx.quote(policy.at("version").get<int64_t>())+" AND jsonb_array_length(payload->'events')>0 ORDER BY source_id LIMIT 4097 FOR SHARE");
    need(candidates.size()<=1);
    if(candidates.empty()&&original.empty()&&raw.at("bars").empty()&&raw.at("restating_metadata").empty()){
        EquityModelActionFrame unchanged;unchanged.positions=prior.positions;unchanged.replay_reference=prior.replay_reference;return unchanged;
    }

    J source;
    if(!candidates.empty())source=candidates[0];
    else{
        const auto market=one(tx,"SELECT "+bounded("to_jsonb(m)")+" FROM trading.qt_desk_market_sources m WHERE source_id="+tx.quote(text(finalization.at("market_source_id")))+"::uuid FOR SHARE");
        need(market.at("book_id")==o.portfolio_id&&market.at("source_day")==o.valuation_day);
        source=one(tx,"SELECT "+bounded("to_jsonb(a)")+" FROM trading.qt_equity_desk_evidence_sources a WHERE source_id="+tx.quote(text(market.at("payload").at("actions_source_id")))+" FOR SHARE");
        need(source.at("content_digest")==market.at("payload").at("actions_source_digest")&&source.at("payload").at("events").empty());
    }
    const auto timing=tx.exec("SELECT a.created_at<=clock_timestamp() AND p.updated_at<=a.created_at AND (a.created_at AT TIME ZONE 'UTC')::date=a.source_day FROM trading.qt_equity_desk_evidence_sources a JOIN trading.qt_source_policies p ON p.book_id=a.book_id AND p.purpose='execution' WHERE a.source_id=$1",pqxx::params{text(source.at("source_id"))});
    need(timing.size()==1&&!timing[0][0].is_null()&&timing[0][0].as<bool>());
    return derive_equity_model_action_frame(prior,o,original,source,policy,raw);
}catch(const std::exception&){return make_error<EquityModelActionFrame>(ErrorCode::INVALID_DATA,"equity_model_action_source_unavailable","equity_model_action_source");}}
}
