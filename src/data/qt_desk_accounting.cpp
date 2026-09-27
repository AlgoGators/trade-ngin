#include "trade_ngin/data/qt_desk_accounting.hpp"
#include "trade_ngin/apps/qt_book_tail.hpp"
#include "trade_ngin/data/qt_equity_desk_accounting.hpp"
#include "trade_ngin/data/qt_desk_upstream.hpp"
#include "trade_ngin/apps/qt_desk_cycle.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include <algorithm>
#include <stdexcept>

namespace trade_ngin { namespace {
using J=nlohmann::json;
void need(bool p){if(!p)throw std::invalid_argument("qt_accounting_unavailable");}
std::string text(const J& j){need(j.is_string());auto s=j.get<std::string>();need(!s.empty());return s;}
J one(pqxx::work& tx,const std::string& sql){auto rows=tx.exec(sql);need(rows.size()==1&&!rows[0][0].is_null());return J::parse(rows[0][0].c_str());}
std::string digest(const J& j){auto bytes=canonical_qt_desk_input_json(j);need(bytes.is_ok());auto h=qt_sha256_hex(bytes.value());need(h.is_ok());return h.value();}
std::string exact(const std::string& column){return "CASE WHEN ("+column+")=round(("+column+")::numeric,8) THEN trim(trailing '.' from trim(trailing '0' from ("+column+")::numeric(28,8)::text)) ELSE NULL END";}
J sorted(J j){need(j.is_array());std::sort(j.begin(),j.end(),[](const J& a,const J& b){return a.dump()<b.dump();});return j;}
bool equity_input(pqxx::work& tx,const std::string& id){
    const auto row=one(tx,"SELECT payload FROM trading.qt_desk_accounting_inputs WHERE input_id="+tx.quote(id)+"::uuid FOR SHARE");
    const auto version=text(row.at("schema_version"));
    need(version=="qt-equity-accounting-input/v1"||version=="qt-equity-accounting-input-empty-owner/v2"||version=="qt-futures-accounting-input/v1"||version=="qt-futures-accounting-input/v2");
    return version=="qt-equity-accounting-input/v1"||version=="qt-equity-accounting-input-empty-owner/v2";
}
// live_results has both legacy DATE and TIMESTAMPTZ installations. Keep exact
// stored-day equality while avoiding session-zone casts in either direction.
struct LiveDateSql { std::string value; std::string day_text; };
LiveDateSql live_date_sql(pqxx::work& tx,const std::string& day){
    const auto columns=tx.exec("SELECT atttypid='date'::regtype,atttypid='timestamptz'::regtype FROM pg_attribute WHERE attrelid='trading.live_results'::regclass AND attname='date' AND NOT attisdropped");
    need(columns.size()==1);const bool date_only=columns[0][0].as<bool>();
    need(date_only||columns[0][1].as<bool>());
    return date_only?LiveDateSql{tx.quote(day)+"::date","date::text"}:
        LiveDateSql{tx.quote(day+"T00:00:00Z")+"::timestamptz","to_char(date AT TIME ZONE 'UTC','YYYY-MM-DD')"};
}
J admit(pqxx::work& tx,const J& d,const std::string& id){
    auto row=one(tx,"SELECT to_jsonb(i) FROM trading.qt_desk_accounting_inputs i WHERE input_id="+tx.quote(id)+"::uuid FOR SHARE");
    auto policy=one(tx,"SELECT to_jsonb(p) FROM trading.qt_source_policies p WHERE book_id="+tx.quote(text(d.at("book_id")))+" AND purpose='execution'");
    need(row.at("decision_id")==d.at("decision_id")&&policy.at("enabled")==true&&row.at("producer_id")==policy.at("producer_id")&&row.at("policy_version")==policy.at("policy_version"));
    text(row.at("source_version"));auto fresh=tx.exec("SELECT as_of<=clock_timestamp() AND clock_timestamp()<=valid_until FROM trading.qt_desk_accounting_inputs WHERE input_id="+tx.quote(id)+"::uuid");
    need(fresh.size()==1&&fresh[0][0].as<bool>()&&digest(row.at("payload"))==text(row.at("content_digest")));
    const auto& in=row.at("payload");for(auto name:{"decision_id","book_id","source_day"})need(in.at(name)==d.at(name));
    auto final=one(tx,"SELECT to_jsonb(f) FROM trading.qt_desk_finalization_sources f WHERE source_id="+tx.quote(text(in.at("prior_finalization_source_id")))+" FOR SHARE");
    need(final.at("book_id")==d.at("book_id")&&final.at("source_day")==in.at("previous_day")&&final.at("producer_id")==policy.at("producer_id")&&final.at("policy_version")==policy.at("policy_version"));
    text(final.at("source_version"));need(digest(final.at("payload"))==text(final.at("content_digest")));
    need((final.at("payload").at("schema_version")=="qt-finalized-accounting/v1"||final.at("payload").at("schema_version")=="qt-finalized-accounting/v2")&&
        final.at("payload").at("book_id")==d.at("book_id")&&final.at("payload").at("source_day")==in.at("previous_day")&&
        sorted(final.at("payload").at("previous_totals"))==sorted(in.at("previous_totals")));
    if(in.at("schema_version")=="qt-futures-accounting-input/v2")need(validate_qt_desk_upstream_input(tx,d,row,final).is_ok());else need(final.at("payload").at("schema_version")=="qt-finalized-accounting/v1");
    const auto book=tx.quote(text(d.at("book_id"))),prior=tx.quote(text(in.at("previous_day")));
    auto latest=tx.exec("SELECT max(date)::text FROM trading.positions WHERE portfolio_id="+book+" AND portfolio_type='qt' AND date<"+tx.quote(text(d.at("source_day")))+"::date");
    need(latest.size()==1&&!latest[0][0].is_null()&&latest[0][0].as<std::string>()==text(in.at("previous_day")));
    J positions=J::array();
    for(auto r:tx.exec("SELECT jsonb_build_object('key',jsonb_build_object('portfolio_id',portfolio_id,'strategy_id',strategy_id,'strategy_name',strategy_name,'date',date::text,'symbol',symbol,'portfolio_type',portfolio_type),'quantity_exact',"+exact("quantity")+",'average_price_exact',"+exact("average_price")+") FROM trading.positions WHERE portfolio_id="+book+" AND portfolio_type='qt' AND date="+prior+"::date FOR SHARE"))positions.push_back(J::parse(r[0].c_str()));
    need(sorted(positions)==sorted(in.at("previous_positions")));
    J totals=J::array();for(auto r:tx.exec("SELECT jsonb_build_object('strategy_id',strategy_id,'equity_exact',"+exact("current_portfolio_value")+",'total_pnl_exact',"+exact("total_pnl")+") FROM trading.live_results WHERE portfolio_id="+book+" AND portfolio_type='qt' AND date="+live_date_sql(tx,text(in.at("previous_day"))).value+" FOR SHARE"))totals.push_back(J::parse(r[0].c_str()));
    need(sorted(totals)==sorted(in.at("previous_totals")));return row;
}
}

Result<J> prepare_qt_desk_accounting(pqxx::work& tx,const J& d,const J& p,const std::string& id,const J& facts){
    try{
        if(equity_input(tx,id))return prepare_qt_equity_desk_accounting(tx,d,p,id,facts);
        auto input=admit(tx,d,id);auto result=run_book_tail(QtFuturesBookTailInputs{d,p.at("payload").at("selection_rows"),input.at("payload")});need(result.is_ok());
        auto output=result.value();output["input_digest"]=input.at("content_digest");
        output["observation"]["schema_version"]="qt-execution/v2";
        output["observation"]["accounting_input_id"]=id;
        output["evaluation_diagnostics"]=p.at("payload").at("evaluation");
        auto diagnostics=build_qt_desk_diagnostics(d,p.at("payload"),facts,input.at("payload"),output);need(diagnostics.is_ok());
        output["desk_diagnostics"]=diagnostics.value();
        output["desk_diagnostics"]["approved_override"]=nullptr;
        if(p.at("payload").at("requires_override")==true){
            auto request=one(tx,"SELECT to_jsonb(r) FROM trading.qt_override_requests r WHERE decision_id="+tx.quote(text(d.at("decision_id")))+"::uuid");
            output["desk_diagnostics"]["approved_override"]={{"request_id",request.at("request_id")},{"decision_id",d.at("decision_id")},{"required_approvals",request.at("required_approvals")},{"authorization","validated_by_confirmed_desk_processor"}};
        }
        const auto& observed=output.at("observation");
        tx.exec("INSERT INTO trading.qt_execution_observations(observation_id,decision_id,producer_id,policy_version,source_version,as_of,valid_until,content_digest,payload) SELECT input_id,decision_id,producer_id,policy_version,source_version,as_of,valid_until,"+tx.quote(digest(observed))+","+tx.quote(observed.dump())+"::jsonb FROM trading.qt_desk_accounting_inputs WHERE input_id="+tx.quote(id)+"::uuid");
        return output;
    }catch(const std::exception&){return make_error<J>(ErrorCode::INVALID_DATA,"qt_accounting_input_unavailable","qt_desk_accounting");}
}
Result<void> revalidate_qt_desk_accounting(pqxx::work& tx,const J& d,const std::string& id){
    try{if(equity_input(tx,id))return revalidate_qt_equity_desk_accounting(tx,d,id);admit(tx,d,id);return Result<void>();}catch(const std::exception&){return make_error<void>(ErrorCode::INVALID_DATA,"qt_accounting_input_changed","qt_desk_accounting");}
}
Result<void> verify_qt_desk_accounting_outputs(pqxx::work& tx,const J& d,const std::string& id){
    try{
        if(equity_input(tx,id))return verify_qt_equity_desk_accounting_outputs(tx,d,id);
        const auto result=one(tx,"SELECT to_jsonb(r) FROM trading.desk_run_results r WHERE decision_id="+tx.quote(text(d.at("decision_id")))+"::uuid");
        need(result.at("input_id")==id&&result.at("portfolio_id")==d.at("book_id")&&result.at("date")==d.at("source_day"));
        const auto& output=result.at("payload");need(digest(output)==text(result.at("content_digest")));
        const auto input=one(tx,"SELECT to_jsonb(i) FROM trading.qt_desk_accounting_inputs i WHERE input_id="+tx.quote(id)+"::uuid");
        need(input.at("decision_id")==d.at("decision_id")&&input.at("content_digest")==output.at("input_digest")&&digest(input.at("payload"))==text(input.at("content_digest")));
        // Historical proof must retain the same linked finalization closure as
        // fresh admission, without reapplying an input's now-expired lease.
        const auto& in=input.at("payload");
        for(auto name:{"decision_id","book_id","source_day"})need(in.at(name)==d.at(name));
        auto final=one(tx,"SELECT to_jsonb(f) FROM trading.qt_desk_finalization_sources f WHERE source_id="+tx.quote(text(in.at("prior_finalization_source_id"))));
        need(final.at("book_id")==d.at("book_id")&&final.at("source_day")==in.at("previous_day")&&final.at("producer_id")==input.at("producer_id")&&final.at("policy_version")==input.at("policy_version"));
        text(final.at("source_version"));need(digest(final.at("payload"))==text(final.at("content_digest")));
        need((final.at("payload").at("schema_version")=="qt-finalized-accounting/v1"||final.at("payload").at("schema_version")=="qt-finalized-accounting/v2")&&final.at("payload").at("book_id")==d.at("book_id")&&final.at("payload").at("source_day")==in.at("previous_day")&&sorted(final.at("payload").at("previous_totals"))==sorted(in.at("previous_totals")));
        if(in.at("schema_version")=="qt-futures-accounting-input/v2")need(validate_qt_desk_upstream_input(tx,d,input,final).is_ok());else need(final.at("payload").at("schema_version")=="qt-finalized-accounting/v1");
        const auto observed=one(tx,"SELECT to_jsonb(o) FROM trading.qt_execution_observations o WHERE observation_id="+tx.quote(id)+"::uuid");
        need(observed.at("payload")==output.at("observation")&&observed.at("producer_id")==input.at("producer_id")&&observed.at("policy_version")==input.at("policy_version")&&observed.at("source_version")==input.at("source_version"));
        auto copied=tx.exec("SELECT i.as_of=o.as_of AND i.valid_until=o.valid_until FROM trading.qt_desk_accounting_inputs i JOIN trading.qt_execution_observations o ON o.observation_id=i.input_id WHERE i.input_id="+tx.quote(id)+"::uuid");need(copied.size()==1&&copied[0][0].as<bool>());
        auto successor=verified_qt_desk_finalization(tx,d);need(successor.is_ok());
        J executions=J::array();
        for(const auto& r:output.at("live_results")){
            const auto scope="portfolio_id="+tx.quote(text(r.at("portfolio_id")))+" AND strategy_id="+tx.quote(text(r.at("strategy_id")))+" AND date="+tx.quote(text(r.at("date")))+"::date AND portfolio_type='qt'";
            const auto live_date=live_date_sql(tx,text(r.at("date")));
            const auto live_scope="portfolio_id="+tx.quote(text(r.at("portfolio_id")))+" AND strategy_id="+tx.quote(text(r.at("strategy_id")))+" AND date="+live_date.value+" AND portfolio_type='qt'";
            auto live=one(tx,"SELECT jsonb_build_object('strategy_id',strategy_id,'portfolio_id',portfolio_id,'date',"+live_date.day_text+",'portfolio_type',portfolio_type,'daily_pnl_exact',"+exact("daily_pnl")+",'daily_transaction_costs_exact',"+exact("daily_transaction_costs")+",'total_pnl_exact',"+exact("total_pnl")+",'current_portfolio_value_exact',"+exact("current_portfolio_value")+") FROM trading.live_results WHERE "+live_scope);
            auto expected=r;std::string realized="0",unrealized="0";
            if(!successor.value().is_null()){
                bool found=false;for(const auto& row:successor.value().at("live_results"))if(row.at("strategy_id")==r.at("strategy_id")){
                    need(!found);found=true;expected=row;realized=text(row.at("daily_realized_pnl_exact"));unrealized=text(row.at("daily_unrealized_pnl_exact"));
                    expected.erase("daily_realized_pnl_exact");expected.erase("daily_unrealized_pnl_exact");
                }need(found);
            }
            need(live==expected);
            auto pnl=tx.exec("SELECT daily_realized_pnl="+tx.quote(realized)+"::numeric AND daily_unrealized_pnl="+tx.quote(unrealized)+"::numeric FROM trading.live_results WHERE "+live_scope);need(pnl.size()==1&&pnl[0][0].as<bool>());
            auto equity=tx.exec("SELECT "+exact("equity")+" FROM trading.equity_curve WHERE portfolio_id="+tx.quote(text(r.at("portfolio_id")))+" AND strategy_id="+tx.quote(text(r.at("strategy_id")))+" AND timestamp="+tx.quote(text(r.at("date"))+"T00:00:00Z")+"::timestamptz AND portfolio_type='qt'");
            need(equity.size()==1&&!equity[0][0].is_null()&&equity[0][0].as<std::string>()==text(expected.at("current_portfolio_value_exact")));
            for(auto sql:tx.exec("SELECT to_jsonb(e) FROM trading.executions e WHERE "+scope)){
                const auto e=J::parse(sql[0].c_str());need(e.at("is_partial")==false);
                auto time_ok=tx.exec("SELECT execution_time="+tx.quote(text(r.at("date"))+"T00:00:00Z")+"::timestamptz FROM trading.executions WHERE "+scope+" AND exec_id="+tx.quote(text(e.at("exec_id"))));
                need(time_ok.size()==1&&time_ok[0][0].as<bool>());
                J item={{"key",{{"portfolio_id",e.at("portfolio_id")},{"strategy_id",e.at("strategy_id")},{"strategy_name",e.at("strategy_name")},{"date",e.at("date")},{"symbol",e.at("symbol")},{"portfolio_type",e.at("portfolio_type")}}},
                    {"exec_id",e.at("exec_id")},{"order_id",e.at("order_id")},{"side",e.at("side")}};
                // Read decimal text through the exact SQL guard rather than a JSON
                // double round trip. Duplicate IDs are refused by one().
                auto numbers=one(tx,"SELECT jsonb_build_object('quantity_exact',"+exact("quantity")+",'price_exact',"+exact("price")+",'commissions_fees_exact',"+exact("commissions_fees")+",'implicit_price_impact_exact',"+exact("implicit_price_impact")+",'slippage_market_impact_exact',"+exact("slippage_market_impact")+",'total_transaction_costs_exact',"+exact("total_transaction_costs")+",'execution_time',to_char(execution_time AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"')) FROM trading.executions WHERE "+scope+" AND exec_id="+tx.quote(text(e.at("exec_id"))));
                item.update(numbers);executions.push_back(item);
            }
        }
        need(sorted(executions)==sorted(output.at("executions")));return Result<void>();
    }catch(const std::exception&){return make_error<void>(ErrorCode::INVALID_DATA,"qt_accounting_outputs_changed","qt_desk_accounting");}
}
Result<void> store_qt_desk_accounting(pqxx::work& tx,const J& d,const std::string& id,const J& out){
    try{
        if(equity_input(tx,id))return store_qt_equity_desk_accounting(tx,d,id,out);
        // Replace only this complete confirmed QT day's engine scope; positions
        // are published by the enclosing processor, not by a second connection.
        for(const auto& r:out.at("live_results")){
            const auto scope="portfolio_id="+tx.quote(text(r.at("portfolio_id")))+" AND strategy_id="+tx.quote(text(r.at("strategy_id")))+" AND date="+tx.quote(text(r.at("date")))+"::date AND portfolio_type='qt'";
            const auto live_date=live_date_sql(tx,text(r.at("date")));
            const auto live_scope="portfolio_id="+tx.quote(text(r.at("portfolio_id")))+" AND strategy_id="+tx.quote(text(r.at("strategy_id")))+" AND date="+live_date.value+" AND portfolio_type='qt'";
            tx.exec("DELETE FROM trading.executions WHERE "+scope);
            tx.exec("DELETE FROM trading.live_results WHERE "+live_scope);
            tx.exec("INSERT INTO trading.live_results(portfolio_id,strategy_id,date,portfolio_type,daily_pnl,daily_realized_pnl,daily_unrealized_pnl,daily_transaction_costs,total_pnl,current_portfolio_value) VALUES("+
                tx.quote(text(r.at("portfolio_id")))+","+tx.quote(text(r.at("strategy_id")))+","+live_date.value+",'qt',"+
                tx.quote(text(r.at("daily_pnl_exact")))+",0,0,"+tx.quote(text(r.at("daily_transaction_costs_exact")))+","+
                tx.quote(text(r.at("total_pnl_exact")))+","+tx.quote(text(r.at("current_portfolio_value_exact")))+")");
            tx.exec("INSERT INTO trading.equity_curve(portfolio_id,strategy_id,timestamp,portfolio_type,equity) VALUES("+
                tx.quote(text(r.at("portfolio_id")))+","+tx.quote(text(r.at("strategy_id")))+","+tx.quote(text(r.at("date"))+"T00:00:00Z")+"::timestamptz,'qt',"+
                tx.quote(text(r.at("current_portfolio_value_exact")))+") ON CONFLICT(portfolio_id,strategy_id,timestamp,portfolio_type) DO UPDATE SET equity=EXCLUDED.equity");
        }
        for(const auto& r:out.at("executions")){
            const auto& k=r.at("key");std::string values;
            for(auto name:{"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"}){if(!values.empty())values+=",";values+=tx.quote(text(k.at(name)));}
            for(auto name:{"exec_id","order_id","side","quantity_exact","price_exact","execution_time","commissions_fees_exact","implicit_price_impact_exact","slippage_market_impact_exact","total_transaction_costs_exact"})values+=","+tx.quote(text(r.at(name)));
            tx.exec("INSERT INTO trading.executions(portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,exec_id,order_id,side,quantity,price,execution_time,commissions_fees,implicit_price_impact,slippage_market_impact,total_transaction_costs,is_partial) VALUES("+values+",false)");
        }
        tx.exec("INSERT INTO trading.desk_run_results(decision_id,input_id,portfolio_id,date,content_digest,payload) VALUES("+
            tx.quote(text(d.at("decision_id")))+"::uuid,"+tx.quote(id)+"::uuid,"+tx.quote(text(d.at("book_id")))+","+tx.quote(text(d.at("source_day")))+"::date,"+tx.quote(digest(out))+","+tx.quote(out.dump())+"::jsonb)");
        return Result<void>();
    }catch(const std::exception&){return make_error<void>(ErrorCode::INVALID_DATA,"qt_accounting_storage_failed","qt_desk_accounting");}
}
}
