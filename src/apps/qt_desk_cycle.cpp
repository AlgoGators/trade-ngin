#include "trade_ngin/apps/qt_desk_cycle.hpp"
#include "trade_ngin/apps/book_execution_phase.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <limits>
#include <charconv>
#include <regex>
#include <stdexcept>

namespace trade_ngin { namespace {
using J=nlohmann::json;
void need(bool ok){if(!ok)throw std::invalid_argument("qt_accounting_input_unavailable");}
std::string text(const J& j){need(j.is_string());auto s=j.get<std::string>();need(!s.empty()&&s.size()<=4096);return s;}
Decimal dec(const J& j){auto p=parse_qt_quantity_exact(text(j));need(p.is_ok());return p.value();}
double model_number(const J& j){auto s=text(j);static const std::regex grammar(R"(-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?)");need(s.size()<=64&&std::regex_match(s,grammar));double n=0;auto r=std::from_chars(s.data(),s.data()+s.size(),n,std::chars_format::general);need(r.ec==std::errc{}&&r.ptr==s.data()+s.size()&&std::isfinite(n));return n;}
double positive(const J& j){auto d=dec(j).as_double();need(d>0);return d;}
double nonnegative(const J& j){auto d=dec(j).as_double();need(d>=0);return d;}
Decimal cash_decimal(double value){
    need(std::isfinite(value));
    // Preserve Decimal(double)'s existing rounding operations, but check the
    // rounded double in a wider type before the potentially undefined cast.
    const double scaled=value*100000000.0+(value>=0?0.5:-0.5);
    need(std::isfinite(scaled)&&static_cast<long double>(scaled)>=static_cast<long double>(INT64_MIN)&&
         static_cast<long double>(scaled)<=static_cast<long double>(INT64_MAX));
    return Decimal::from_raw(static_cast<int64_t>(scaled));
}
Decimal sum(Decimal a,Decimal b){auto x=a.raw_value(),y=b.raw_value();
    need(!((y>0&&x>INT64_MAX-y)||(y<0&&x<INT64_MIN-y)));return Decimal::from_raw(x+y);}
std::string identity(J k,const J& d,const std::string& day){
    need(k.is_object()&&k.size()==6&&k.at("portfolio_id")==d.at("book_id")&&k.at("date")==day);
    for(auto name:{"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"})text(k.at(name));
    need(k.at("portfolio_type")=="qt"||k.at("portfolio_type")=="qt_proposal");
    k["date"]=d.at("source_day");k["portfolio_type"]="qt";return k.dump();
}
void day(const std::string& s){need(s.size()==10&&s[4]=='-'&&s[7]=='-');
    for(size_t n=0;n<s.size();++n)if(n!=4&&n!=7)need(s[n]>='0'&&s[n]<='9');
    using namespace std::chrono;
    const year_month_day value{year{std::stoi(s.substr(0,4))},month{static_cast<unsigned>(std::stoi(s.substr(5,2)))},std::chrono::day{static_cast<unsigned>(std::stoi(s.substr(8,2)))}};
    need(value.ok());
}
}

Result<J> produce_qt_futures_accounting(const J& d,const J& selection,const J& in){
    try {
        const bool v2=in.at("schema_version")=="qt-futures-accounting-input/v2";need(v2||in.at("schema_version")=="qt-futures-accounting-input/v1");
        if(v2){text(in.at("market_source_id"));text(in.at("market_source_digest"));text(in.at("prior_finalization_digest"));}
        auto positive_model=[v2](const J& value){auto n=v2?model_number(value):positive(value);need(n>0);return n;};
        auto nonnegative_model=[v2](const J& value){auto n=v2?model_number(value):nonnegative(value);need(n>=0);return n;};
        for(auto f:{"decision_id","book_id","source_day"})need(in.at(f)==d.at(f));
        auto today=text(d.at("source_day")),prior=text(in.at("previous_day"));day(today);day(prior);need(prior<today);
        const auto source=text(in.at("accounting_source_id")),currency=text(in.at("currency"));
        const auto stamp=text(in.at("timestamp"));need(stamp==today+"T00:00:00Z");
        // This is a reference to governed finalization evidence, never a boolean
        // assertion inferred from the presence of a live_results row.
        text(in.at("prior_finalization_source_id"));
        need(selection.is_array()&&!selection.empty()&&selection.size()<=4096);
        std::map<std::string,J> previous,selected,markets,totals;
        std::set<std::string> engines;
        for(const auto& r:in.at("previous_positions")){
            need(r.at("key").at("portfolio_type")=="qt");
            auto id=identity(r.at("key"),d,prior);need(previous.emplace(id,r).second);
            auto q=dec(r.at("quantity_exact"));need(q.raw_value()%100000000==0);
            need(dec(r.at("average_price_exact")).raw_value()>0);
        }
        for(const auto& r:selection){
            need(r.at("asset_type")=="FUTURE"&&r.at("editable").is_boolean());
            auto id=identity(r.at("key"),d,today);need(selected.emplace(id,r).second);
            need(dec(r.at("quantity_exact")).raw_value()%100000000==0);
            engines.insert(text(r.at("key").at("strategy_id")));
        }
        for(const auto& [id,r]:previous)need(selected.contains(id)); // explicit zero closes only
        for(const auto& r:in.at("previous_totals")){
            auto engine=text(r.at("strategy_id"));need(engines.contains(engine)&&totals.emplace(engine,r).second);
            positive(r.at("equity_exact"));dec(r.at("total_pnl_exact"));
        }need(totals.size()==engines.size());
        transaction_cost::TransactionCostManager::Config config;
        const auto& c=in.at("cost_config");config.explicit_fee_per_contract=nonnegative_model(c.at("explicit_fee_per_contract"));
        config.impact_config.min_adv=positive_model(c.at("min_adv"));
        config.impact_config.min_participation=nonnegative_model(c.at("min_participation"));
        config.impact_config.max_participation=positive_model(c.at("max_participation"));
        need(config.impact_config.min_participation<=config.impact_config.max_participation&&config.impact_config.max_participation<=1);
        transaction_cost::TransactionCostManager costs(config);
        for(const auto& r:in.at("instruments")){
            const auto symbol=text(r.at("symbol"));need(markets.emplace(symbol,r).second);
            text(r.at("source_id"));positive_model(r.at(v2?"price_model_number":"price_exact"));positive_model(r.at(v2?"adv_model_number":"adv_exact"));positive_model(r.at(v2?"volatility_multiplier_model_number":"volatility_multiplier_exact"));
            transaction_cost::AssetCostConfig a;a.symbol=symbol;
            a.baseline_spread_ticks=nonnegative_model(r.at("baseline_spread_ticks"));a.min_spread_ticks=nonnegative_model(r.at("min_spread_ticks"));
            a.max_spread_ticks=nonnegative_model(r.at("max_spread_ticks"));need(a.min_spread_ticks<=a.max_spread_ticks);
            a.spread_cost_multiplier=nonnegative_model(r.at("spread_cost_multiplier"));a.max_impact_bps=nonnegative_model(r.at("max_impact_bps"));
            a.tick_size=positive_model(r.at("tick_size"));a.point_value=positive_model(r.at("point_value"));
            a.max_total_implicit_bps=nonnegative_model(r.at("max_total_implicit_bps"));costs.register_asset_config(a);
        }
        J fills=J::array(),executions=J::array(),distance=J::array();
        std::map<std::string,Decimal> charges;
        std::set<std::string> symbols;Decimal cash;
        for(const auto& [id,row]:selected){
            auto k=J::parse(id);const auto symbol=text(k.at("symbol")),engine=text(k.at("strategy_id"));symbols.insert(symbol);
            need(markets.contains(symbol));const auto& m=markets.at(symbol);
            auto qty=dec(row.at("quantity_exact"));auto prev=previous.contains(id)?dec(previous.at(id).at("quantity_exact")):Decimal(0);
            auto delta=sum(qty,-prev);const auto reference=positive_model(m.at(v2?"price_model_number":"price_exact"));auto price=v2?cash_decimal(reference):dec(m.at("price_exact"));need(price.raw_value()>0);
            // Preserve the explicitly supplied system basis; new symbols require
            // an execution and acquire the actual prior-close reference price.
            auto basis=row.at("average_price_exact").is_null()?price:dec(row.at("average_price_exact"));need(basis.raw_value()>0);
            need(!row.at("average_price_exact").is_null()||!delta.is_zero());
            if(delta.is_zero())need(previous.contains(id)&&basis==dec(previous.at(id).at("average_price_exact")));
            if(row.at("editable")==false)need(delta.is_zero()&&previous.contains(id)&&basis==dec(previous.at(id).at("average_price_exact")));
            Decimal charge;J execution_id=nullptr;
            if(!delta.is_zero()){
                transaction_cost::CostChargeObservation observed;
                auto charged=charge_book_execution(costs,GovernedExecutionCharge{symbol,delta,reference,
                    positive_model(m.at(v2?"adv_model_number":"adv_exact")),positive_model(m.at(v2?"volatility_multiplier_model_number":"volatility_multiplier_exact")),AssetType::FUTURE},&observed);
                need(charged.is_ok());const auto& cost=charged.value();
                const auto commission=cost.commissions_fees,slippage=cost.slippage_market_impact;
                charge=cost.total_transaction_costs;
                // Owner identity is retained even when opposite components net to zero.
                auto hash=qt_sha256_hex(id);need(hash.is_ok());std::string exec="QT_"+hash.value().substr(0,40);execution_id=exec;
                executions.push_back({{"key",k},{"exec_id",exec},{"order_id",exec},
                    {"side",delta.is_positive()?"BUY":"SELL"},{"quantity_exact",delta.abs().to_string()},
                    {"price_exact",price.to_string()},{"execution_time",stamp},
                    {"commissions_fees_exact",commission.to_string()},
                    {"implicit_price_impact_exact",cost.implicit_price_impact.to_string()},
                    {"slippage_market_impact_exact",slippage.to_string()},{"total_transaction_costs_exact",charge.to_string()}});
            }
            charges[engine]=sum(charges[engine],charge);cash=sum(cash,charge);
            fills.push_back({{"key",k},{"observation_kind",delta.is_zero()?"carried":"executed"},
                {"selected_quantity_exact",qty.to_string()},{"average_price_exact",basis.to_string()},
                {"actual_cash_cost_exact",charge.to_string()},{"currency",currency},{"execution_id",execution_id},
                {"accounting_source_id",source},{"daily_unrealized_pnl_exact","0"},{"daily_realized_pnl_exact","0"},{"last_update",stamp}});
            distance.push_back({{"key",k},{"selected_quantity_exact",qty.to_string()},
                {"previous_quantity_exact",prev.to_string()},{"execution_delta_exact",delta.to_string()},
                {"selection_moved_by","unchanged"}});
        }
        need(markets.size()==symbols.size());
        J live=J::array();for(const auto& [engine,t]:totals){
            const auto charge=charges[engine],equity=sum(dec(t.at("equity_exact")),-charge);
            need(equity.raw_value()>0);
            live.push_back({{"strategy_id",engine},{"portfolio_id",d.at("book_id")},{"date",today},{"portfolio_type","qt"},
                {"daily_pnl_exact",(-charge).to_string()},{"daily_transaction_costs_exact",charge.to_string()},
                {"total_pnl_exact",sum(dec(t.at("total_pnl_exact")),-charge).to_string()},
                {"current_portfolio_value_exact",equity.to_string()}});
        }
        J result={{"position_count",fills.size()},{"currency_totals",J::array({{{"currency",currency},
            {"actual_cash_cost_exact",cash.to_string()},{"daily_unrealized_pnl_exact","0"},{"daily_realized_pnl_exact","0"}}})}};
        return J{{"schema_version","qt-futures-accounting/v1"},{"observation",{{"schema_version","qt-execution/v1"},
            {"decision_id",d.at("decision_id")},{"book_id",d.at("book_id")},{"source_day",today},{"fills",fills},{"results",result}}},
            {"executions",executions},{"live_results",live},{"distance",distance},
            {"layers_applied",J::array()},{"selection_policy","exact-confirmed-choice"}};
    }catch(const std::exception&){return make_error<J>(ErrorCode::INVALID_DATA,"qt_accounting_input_unavailable","qt_desk_cycle");}
}
}
