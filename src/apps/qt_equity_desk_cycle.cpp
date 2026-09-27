#include "trade_ngin/apps/qt_equity_desk_cycle.hpp"
#include "trade_ngin/apps/book_execution_phase.hpp"
#include "trade_ngin/apps/qt_equity_position_transition.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include <charconv>
#include <cmath>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string_view>

namespace trade_ngin { namespace {
using J=nlohmann::json;
using namespace transaction_cost;
void need(bool ok) { if(!ok) throw std::invalid_argument("qt_equity_accounting_input_unavailable"); }
void shape(const J& value,std::initializer_list<std::string_view> names) {
    need(value.is_object()&&value.size()==names.size());
    for(const auto name:names) need(value.contains(std::string(name)));
}
std::string text(const J& value) {
    need(value.is_string());const auto s=value.get<std::string>();
    need(!s.empty()&&s.size()<=4096&&s.find_first_not_of(" \t\r\n")!=std::string::npos);
    return s;
}
bool flag(const J& value) { need(value.is_boolean());return value.get<bool>(); }
void array(const J& value) { need(value.is_array()&&value.size()<=4096); }
Decimal exact(const J& value) {
    const auto parsed=parse_qt_quantity_exact(text(value));need(parsed.is_ok());return parsed.value();
}
Decimal add(Decimal a,Decimal b) {
    const auto x=a.raw_value(),y=b.raw_value();
    need(!((y>0&&x>INT64_MAX-y)||(y<0&&x<INT64_MIN-y)));
    return Decimal::from_raw(x+y);
}
Decimal sub(Decimal a,Decimal b) {
    const auto x=a.raw_value(),y=b.raw_value();
    need(!((y<0&&x>INT64_MAX+y)||(y>0&&x<INT64_MIN+y)));
    return Decimal::from_raw(x-y);
}
Decimal cash(double value) {
    need(std::isfinite(value));const auto scaled=value*100000000.0+(value>=0?0.5:-0.5);
    need(std::isfinite(scaled)&&static_cast<long double>(scaled)>=static_cast<long double>(INT64_MIN)&&
         static_cast<long double>(scaled)<=static_cast<long double>(INT64_MAX));
    return Decimal::from_raw(static_cast<int64_t>(scaled));
}
double number(const J& value) {
    const auto s=text(value);static const std::regex grammar(R"(-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?)");
    need(s.size()<=64&&std::regex_match(s,grammar));double n=0;
    const auto parsed=std::from_chars(s.data(),s.data()+s.size(),n,std::chars_format::general);
    need(parsed.ec==std::errc{}&&parsed.ptr==s.data()+s.size()&&std::isfinite(n));return n;
}
double positive(const J& value) {const auto n=number(value);need(n>0);return n;}
double nonnegative(const J& value) {const auto n=number(value);need(n>=0);return n;}
std::string uuid(const J& value) {
    const auto s=text(value);static const std::regex grammar("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}");
    need(std::regex_match(s,grammar));return s;
}
void digest(const J& value) {
    const auto s=text(value);need(s.size()==64&&s.find_first_not_of("0123456789abcdef")==std::string::npos);
}
Timestamp day(const std::string& s) {
    need(s.size()==10&&s[4]=='-'&&s[7]=='-');
    for(size_t i=0;i<s.size();++i)if(i!=4&&i!=7)need(s[i]>='0'&&s[i]<='9');
    using namespace std::chrono;
    const year_month_day date{year{std::stoi(s.substr(0,4))},month{static_cast<unsigned>(std::stoi(s.substr(5,2)))},
                             std::chrono::day{static_cast<unsigned>(std::stoi(s.substr(8,2)))}};
    need(date.ok());const auto days=sys_days{date}.time_since_epoch();
    // Check before duration conversion so a syntactically valid remote year
    // cannot overflow the native clock representation.
    const auto ticks=static_cast<long double>(days.count())*86400.0L*
        Timestamp::duration::period::den/Timestamp::duration::period::num;
    need(ticks>=static_cast<long double>(INT64_MIN)&&ticks<=static_cast<long double>(INT64_MAX)-
         86400.0L*Timestamp::duration::period::den/Timestamp::duration::period::num);
    return Timestamp{duration_cast<Timestamp::duration>(days)};
}
Timestamp utc(const J& value) {
    const auto s=text(value);need(s.size()>=20&&s.size()<=30&&s[10]=='T'&&s[13]==':'&&s[16]==':'&&s.back()=='Z');
    for(const auto i:{11,12,14,15,17,18})need(s[i]>='0'&&s[i]<='9');
    const auto hours=std::stoi(s.substr(11,2)),minutes=std::stoi(s.substr(14,2)),seconds=std::stoi(s.substr(17,2));
    need(hours<24&&minutes<60&&seconds<60);int64_t fraction=0;
    if(s.size()!=20) {
        need(s[19]=='.'&&s.size()>=22);size_t digits=0;
        for(size_t i=20;i+1<s.size();++i) {need(s[i]>='0'&&s[i]<='9');fraction=fraction*10+s[i]-'0';++digits;}
        need(digits<=9);while(digits++<9)fraction*=10;
    }
    using namespace std::chrono;
    return day(s.substr(0,10))+std::chrono::hours{hours}+std::chrono::minutes{minutes}+
           std::chrono::seconds{seconds}+duration_cast<Timestamp::duration>(nanoseconds{fraction});
}
J normalize(const J& key,const J& d,const std::string& date,bool proposal=false) {
    shape(key,{"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"});
    for(auto it=key.begin();it!=key.end();++it)text(it.value());
    need(key.at("portfolio_id")==d.at("book_id")&&key.at("date")==date&&
         (key.at("portfolio_type")=="qt"||(proposal&&key.at("portfolio_type")=="qt_proposal")));
    auto result=key;result["date"]=d.at("source_day");result["portfolio_type"]="qt";return result;
}
ComponentPositionKey typed_key(const J& key) {
    return {text(key.at("portfolio_id")),text(key.at("strategy_id")),text(key.at("strategy_name")),
            text(key.at("date")),text(key.at("symbol")),text(key.at("portfolio_type"))};
}
void sourced(const J& row) {text(row.at("source_id"));digest(row.at("source_digest"));}
void priced(const J& row,const std::string& prior) {
    shape(row,{"source_id","source_digest","date","price_frame_id","price_model_number"});
    sourced(row);need(row.at("date")==prior);text(row.at("price_frame_id"));positive(row.at("price_model_number"));
}
AssetCostConfig asset(const J& row) {
    const auto& c=row.at("cost_parameters");
    shape(c,{"tick_constrained","commission_per_unit","min_commission_per_order","max_commission_per_order",
        "max_commission_pct","sec_fee_per_million","finra_taf_per_share","finra_taf_cap_per_trade",
        "apply_regulatory_fees","baseline_spread_ticks","min_spread_ticks","max_spread_ticks",
        "spread_cost_multiplier","max_impact_bps","tick_size","point_value","max_total_implicit_bps"});
    AssetCostConfig result;result.symbol=text(row.at("symbol"));result.asset_type=AssetType::EQUITY;
    result.tick_constrained=flag(c.at("tick_constrained"));
    result.commission_per_unit=nonnegative(c.at("commission_per_unit"));
    result.min_commission_per_order=nonnegative(c.at("min_commission_per_order"));
    result.max_commission_per_order=nonnegative(c.at("max_commission_per_order"));
    result.max_commission_pct=number(c.at("max_commission_pct"));
    result.sec_fee_per_million=nonnegative(c.at("sec_fee_per_million"));
    result.finra_taf_per_share=nonnegative(c.at("finra_taf_per_share"));
    result.finra_taf_cap_per_trade=nonnegative(c.at("finra_taf_cap_per_trade"));
    result.apply_regulatory_fees=flag(c.at("apply_regulatory_fees"));
    result.baseline_spread_ticks=nonnegative(c.at("baseline_spread_ticks"));
    result.min_spread_ticks=nonnegative(c.at("min_spread_ticks"));
    result.max_spread_ticks=nonnegative(c.at("max_spread_ticks"));need(result.min_spread_ticks<=result.max_spread_ticks);
    result.spread_cost_multiplier=nonnegative(c.at("spread_cost_multiplier"));
    result.max_impact_bps=nonnegative(c.at("max_impact_bps"));result.tick_size=positive(c.at("tick_size"));
    result.point_value=positive(c.at("point_value"));need(result.point_value==1);
    result.max_total_implicit_bps=number(c.at("max_total_implicit_bps"));return result;
}
struct EngineSums {Decimal gross,cost,unrealized;};
}

Result<nlohmann::json> produce_qt_equity_accounting(
    const J& d,const J& selection,const J& in,std::vector<QtEquityCostTrace>* cost_trace) {
    try {
        need(in.dump().size()<=1048576&&selection.dump().size()<=1048576);
        shape(in,{"schema_version","calculation_version","decision_id","book_id","source_day",
            "accounting_input_id","market_source_id","market_source_digest","accounting_source_id",
            "prior_finalization_source_id","prior_finalization_digest","previous_day","timestamp","day_mode",
            "currency","previous_positions","previous_totals","instruments","cost_config","actions"});
        const bool empty_owner=in.at("schema_version")=="qt-equity-accounting-input-empty-owner/v2";
        need((empty_owner||in.at("schema_version")=="qt-equity-accounting-input/v1")&&
             in.at("calculation_version")=="qt-equity-main08b15c/v1"&&in.at("day_mode")=="open"&&in.at("currency")=="USD");
        for(auto f:{"decision_id","book_id","source_day"})need(in.at(f)==d.at(f));
        uuid(d.at("decision_id"));text(d.at("book_id"));const auto today=text(d.at("source_day")),prior=text(in.at("previous_day"));
        const auto timestamp=day(today),previous_timestamp=day(prior);need(previous_timestamp<timestamp);
        const auto stamp=text(in.at("timestamp"));need(stamp==today+"T00:00:00Z");
        const auto input_id=uuid(in.at("accounting_input_id"));uuid(in.at("market_source_id"));digest(in.at("market_source_digest"));
        digest(in.at("prior_finalization_digest"));const auto finalization=text(in.at("prior_finalization_source_id"));
        need(finalization.size()==52&&finalization.substr(0,16)=="qt-finalization/");uuid(finalization.substr(16));
        const auto source=text(in.at("accounting_source_id"));
        array(selection);need(empty_owner ? selection.empty() : !selection.empty());
        for(auto f:{"previous_positions","previous_totals","instruments","actions"})array(in.at(f));
        if(empty_owner) {
            need(in.at("previous_positions").empty()&&in.at("instruments").empty()&&in.at("actions").empty());
            need(in.at("previous_totals").size()==1);
        }
        std::map<std::string,J> previous,selected,markets,totals;
        std::map<std::string,Decimal> previous_unrealized;
        std::map<std::string,EngineSums> sums;
        std::set<std::string> engines,symbols;
        for(const auto& row:in.at("previous_positions")) {
            shape(row,{"key","quantity_exact","average_price_exact","daily_realized_pnl_exact","daily_unrealized_pnl_exact","last_update","basis_evidence"});
            const auto key=normalize(row.at("key"),d,prior);const auto id=key.dump();need(previous.emplace(id,row).second);
            const auto quantity=exact(row.at("quantity_exact")),basis=exact(row.at("average_price_exact"));
            need(basis.raw_value()>=0&&(quantity.is_zero()||basis.raw_value()>0));exact(row.at("daily_realized_pnl_exact"));
            const auto observed_at=utc(row.at("last_update"));need(previous_timestamp<=observed_at&&observed_at<=timestamp);
            const auto& evidence=row.at("basis_evidence");shape(evidence,{"source_id","source_digest","price_frame_id","formed_day"});
            sourced(evidence);text(evidence.at("price_frame_id"));need(day(text(evidence.at("formed_day")))<=previous_timestamp);
            const auto engine=text(key.at("strategy_id"));previous_unrealized[engine]=add(previous_unrealized[engine],exact(row.at("daily_unrealized_pnl_exact")));
        }
        for(const auto& row:selection) {
            need(row.is_object()&&row.at("asset_type")=="EQUITY");flag(row.at("editable"));
            const auto key=normalize(row.at("key"),d,today,true);const auto id=key.dump();need(selected.emplace(id,row).second);
            const auto quantity=exact(row.at("quantity_exact"));need(previous.contains(id)||!quantity.is_zero());
            if(!row.at("average_price_exact").is_null())need(exact(row.at("average_price_exact")).raw_value()>=0);
            engines.insert(text(key.at("strategy_id")));symbols.insert(text(key.at("symbol")));
        }
        for(const auto& [id,row]:previous)need(selected.contains(id));
        if(empty_owner) {
            const auto engine=text(in.at("previous_totals")[0].at("strategy_id"));
            need(engine=="LIVE_EQUITY_MEAN_REVERSION");engines.insert(engine);
        }
        for(const auto& row:in.at("previous_totals")) {
            shape(row,{"strategy_id","initial_capital_exact","equity_exact","total_pnl_exact","total_realized_pnl_exact","total_transaction_costs_exact","total_unrealized_pnl_exact"});
            const auto engine=text(row.at("strategy_id"));need(engines.contains(engine)&&totals.emplace(engine,row).second);
            const auto capital=exact(row.at("initial_capital_exact")),equity=exact(row.at("equity_exact")),pnl=exact(row.at("total_pnl_exact"));
            const auto realized=exact(row.at("total_realized_pnl_exact")),cost=exact(row.at("total_transaction_costs_exact")),unreal=exact(row.at("total_unrealized_pnl_exact"));
            need(capital.is_positive()&&equity.is_positive()&&!cost.is_negative()&&pnl==add(sub(realized,cost),unreal)&&
                 equity==add(capital,pnl)&&unreal==previous_unrealized[engine]);
        }
        need(totals.size()==engines.size());
        const auto& config=in.at("cost_config");shape(config,{"explicit_fee_per_contract","min_adv","min_participation","max_participation"});
        TransactionCostManager::Config settings;settings.explicit_fee_per_contract=nonnegative(config.at("explicit_fee_per_contract"));
        settings.impact_config.min_adv=positive(config.at("min_adv"));settings.impact_config.min_participation=nonnegative(config.at("min_participation"));
        settings.impact_config.max_participation=positive(config.at("max_participation"));
        need(settings.impact_config.min_participation<=settings.impact_config.max_participation&&settings.impact_config.max_participation<=1);
        TransactionCostManager costs(settings);
        for(const auto& row:in.at("instruments")) {
            shape(row,{"symbol","asset_type","reference","mark","cost_evidence","cost_parameters"});
            const auto symbol=text(row.at("symbol"));need(row.at("asset_type")=="EQUITY"&&symbols.contains(symbol)&&markets.emplace(symbol,row).second);
            priced(row.at("reference"),prior);priced(row.at("mark"),prior);
            need(row.at("reference").at("price_frame_id")==row.at("mark").at("price_frame_id"));
            const auto& evidence=row.at("cost_evidence");shape(evidence,{"source_id","source_digest","date","adv_model_number","volatility_multiplier_model_number"});
            sourced(evidence);need(evidence.at("date")==prior);positive(evidence.at("adv_model_number"));positive(evidence.at("volatility_multiplier_model_number"));
            costs.register_asset_config(asset(row));
        }
        need(markets.size()==symbols.size());
        std::map<std::string,std::vector<J>> events;
        for(const auto& event:in.at("actions")) {
            shape(event,{"key","source_id","source_digest","type","ex_date","value_model_number","basis_provenance",
                "basis_provenance_evidence","frame_before","frame_after","raw_close_model_number","eligible_quantity_exact"});
            const auto key=normalize(event.at("key"),d,today);const auto id=key.dump();
            need(selected.contains(id)&&previous.contains(id));sourced(event);events[id].push_back(event);
        }
        J fills=J::array(),executions=J::array(),distance=J::array(),adjustments=J::array();
        std::vector<QtEquityCostTrace> traced;
        Decimal total_cost,total_gross,total_unreal;
        for(const auto& [id,row]:selected) {
            const auto key=J::parse(id);const auto owner=typed_key(key);const auto& market=markets.at(owner.symbol);
            const auto quantity=exact(row.at("quantity_exact"));std::optional<Position> old;
            auto frame=text(market.at("reference").at("price_frame_id"));std::string formed;
            if(previous.contains(id)) {
                const auto& p=previous.at(id);old=Position{owner.symbol,exact(p.at("quantity_exact")),exact(p.at("average_price_exact")),
                    exact(p.at("daily_unrealized_pnl_exact")),exact(p.at("daily_realized_pnl_exact")),utc(p.at("last_update"))};
                frame=text(p.at("basis_evidence").at("price_frame_id"));formed=text(p.at("basis_evidence").at("formed_day"));
            }
            std::vector<CorpActionEvent> actions;std::string last;std::set<std::pair<std::string,std::string>> unique;
            for(const auto& event:events[id]) {
                const auto ex_date=text(event.at("ex_date")),type=text(event.at("type"));
                need(day(ex_date)<=timestamp&&ex_date>=last&&unique.emplace(ex_date,type).second);last=ex_date;
                need(text(event.at("frame_before"))==frame);const auto provenance=text(event.at("basis_provenance"));
                CorpActionEvent action;action.symbol=owner.symbol;action.ex_date=ex_date;action.value=positive(event.at("value_model_number"));
                action.basis_provenance_evidence=text(event.at("basis_provenance_evidence"));
                if(type=="SPLIT")action.type=CorpActionType::SPLIT;
                else if(type=="ADR_SPLIT")action.type=CorpActionType::ADR_SPLIT;
                else {need(type=="DIVIDEND");action.type=CorpActionType::DIVIDEND;}
                if(provenance=="formed_on_or_before_ex_date") {
                    need(formed<=ex_date);action.basis_provenance=CorpActionEvent::BasisProvenance::FORMED_ON_OR_BEFORE_EX_DATE;
                    frame=text(event.at("frame_after"));need(frame!=text(event.at("frame_before")));
                } else {
                    need(provenance=="formed_after_ex_date"&&formed>ex_date&&event.at("frame_after")==frame);
                    action.basis_provenance=CorpActionEvent::BasisProvenance::FORMED_AFTER_EX_DATE;
                }
                if(action.type==CorpActionType::DIVIDEND) {
                    action.close_at_ex_date=positive(event.at("raw_close_model_number"));
                    const auto eligible=exact(event.at("eligible_quantity_exact"));need(eligible.is_positive());action.qty_at_ex_date=eligible.as_double();
                } else need(event.at("raw_close_model_number").is_null()&&event.at("eligible_quantity_exact").is_null());
                actions.push_back(action);
            }
            need(frame==text(market.at("reference").at("price_frame_id")));
            const auto reference=positive(market.at("reference").at("price_model_number")),mark=positive(market.at("mark").at("price_model_number"));
            auto transition=produce_qt_equity_position_transition(owner,old,quantity,reference,mark,timestamp,actions);need(transition.is_ok());
            const auto& actual=transition.value();const auto change=actual.quantity_change;
            if(!flag(row.at("editable"))) {
                need(old.has_value()&&change.is_zero()&&!row.at("average_price_exact").is_null());
                const auto saved_basis=exact(row.at("average_price_exact"));
                need(saved_basis==actual.position.average_price ||
                     (!actual.adjustments.empty()&&saved_basis==old->average_price));
            }
            Decimal charge;J execution_id=nullptr;
            if(!change.is_zero()) {
                need(change.raw_value()!=INT64_MIN);CostChargeObservation observed;
                const auto& evidence=market.at("cost_evidence");
                const auto charged=charge_book_execution(costs,GovernedExecutionCharge{owner.symbol,change,reference,
                    positive(evidence.at("adv_model_number")),positive(evidence.at("volatility_multiplier_model_number")),AssetType::EQUITY},&observed);
                need(charged.is_ok());const auto& outcome=charged.value();
                const auto commission=outcome.commissions_fees,implicit=outcome.implicit_price_impact,slippage=outcome.slippage_market_impact;
                charge=outcome.total_transaction_costs;
                auto hash=qt_sha256_hex(text(d.at("decision_id"))+"/"+id);need(hash.is_ok());const auto exec="QT_EQ_"+hash.value().substr(0,40);execution_id=exec;
                executions.push_back({{"key",key},{"exec_id",exec},{"order_id",exec},{"side",change.is_positive()?"BUY":"SELL"},
                    {"quantity_exact",change.abs().to_string()},{"price_exact",cash(reference).to_string()},{"execution_time",stamp},
                    {"commissions_fees_exact",commission.to_string()},{"implicit_price_impact_exact",implicit.to_string()},
                    {"slippage_market_impact_exact",slippage.to_string()},{"total_transaction_costs_exact",charge.to_string()}});
                if(cost_trace)traced.push_back({owner,outcome.raw,observed});
            }
            for(size_t i=0;i<actual.adjustments.size();++i) {
                const auto& a=actual.adjustments[i];
                const auto action_type=std::string(CorporateActionsApplier::type_to_string(a.type));
                const J* original=nullptr;
                for(const auto& event:events[id])if(event.at("type")==action_type&&event.at("ex_date")==a.event_date) {
                    need(original==nullptr);original=&event;
                }
                need(original!=nullptr);
                // The imported dividend audit quantity is eligibility quantity,
                // not the live share holding; do not add it as cash income.
                adjustments.push_back({{"key",key},{"type",action_type},
                    {"source_id",original->at("source_id")},{"source_digest",original->at("source_digest")},
                    {"basis_provenance",original->at("basis_provenance")},{"basis_provenance_evidence",original->at("basis_provenance_evidence")},
                    {"frame_before",original->at("frame_before")},{"frame_after",original->at("frame_after")},
                    {"event_date",a.event_date},{"quantity_before_exact",cash(a.quantity_before).to_string()},
                    {"quantity_after_exact",cash(a.quantity_after).to_string()},{"average_price_before_exact",cash(a.avg_price_before).to_string()},
                    {"average_price_after_exact",cash(a.avg_price_after).to_string()},{"event_value_model_number",original->at("value_model_number")},
                    {"ratio_change_model_number",J(a.ratio_change).dump()}});
            }
            const auto gross=actual.gross_trade_realized_pnl,unreal=actual.position.unrealized_pnl;
            auto& engine=sums[owner.strategy_id];engine.gross=add(engine.gross,gross);engine.cost=add(engine.cost,charge);engine.unrealized=add(engine.unrealized,unreal);
            total_cost=add(total_cost,charge);total_gross=add(total_gross,gross);total_unreal=add(total_unreal,unreal);
            fills.push_back({{"key",key},{"observation_kind",change.is_zero()?"carried":"executed"},
                {"selected_quantity_exact",quantity.to_string()},{"average_price_exact",actual.position.average_price.to_string()},
                {"actual_cash_cost_exact",charge.to_string()},{"currency","USD"},{"execution_id",execution_id},
                {"accounting_source_id",source},{"daily_realized_pnl_exact",gross.to_string()},
                {"daily_unrealized_pnl_exact",unreal.to_string()},{"last_update",stamp}});
            distance.push_back({{"key",key},{"selected_quantity_exact",quantity.to_string()},
                {"previous_quantity_exact",old?old->quantity.to_string():"0"},
                {"restated_previous_quantity_exact",actual.restated_previous.quantity.to_string()},
                {"execution_delta_exact",change.to_string()},{"selection_moved_by","unchanged"}});
        }
        J live=J::array(),curve=J::array();
        for(const auto& [engine,anchor]:totals) {
            const auto& current=sums[engine];const auto realized=add(exact(anchor.at("total_realized_pnl_exact")),current.gross);
            const auto cumulative_cost=add(exact(anchor.at("total_transaction_costs_exact")),current.cost);
            const auto daily_unreal=sub(current.unrealized,exact(anchor.at("total_unrealized_pnl_exact")));
            const auto pnl=add(sub(realized,cumulative_cost),current.unrealized),capital=exact(anchor.at("initial_capital_exact"));
            const auto equity=add(capital,pnl),daily_pnl=add(sub(current.gross,current.cost),daily_unreal);need(equity.is_positive());
            live.push_back({{"strategy_id",engine},{"portfolio_id",d.at("book_id")},{"date",today},{"portfolio_type","qt"},
                {"initial_capital_exact",capital.to_string()},{"daily_realized_pnl_exact",current.gross.to_string()},
                {"daily_unrealized_pnl_exact",daily_unreal.to_string()},{"daily_pnl_exact",daily_pnl.to_string()},
                {"daily_transaction_costs_exact",current.cost.to_string()},{"total_realized_pnl_exact",realized.to_string()},
                {"total_unrealized_pnl_exact",current.unrealized.to_string()},{"total_transaction_costs_exact",cumulative_cost.to_string()},
                {"total_pnl_exact",pnl.to_string()},{"current_portfolio_value_exact",equity.to_string()}});
            curve.push_back({{"portfolio_id",d.at("book_id")},{"strategy_id",engine},{"timestamp",stamp},{"portfolio_type","qt"},{"equity_exact",equity.to_string()}});
        }
        J results={{"position_count",fills.size()},{"currency_totals",J::array({{{"currency","USD"},
            {"actual_cash_cost_exact",total_cost.to_string()},{"daily_realized_pnl_exact",total_gross.to_string()},
            {"daily_unrealized_pnl_exact",total_unreal.to_string()}}})}};
        J output={{"schema_version",empty_owner ? "qt-equity-accounting-empty-owner/v2" : "qt-equity-accounting/v1"},{"calculation_version",in.at("calculation_version")},
            {"observation",{{"schema_version","qt-execution/v2"},{"decision_id",d.at("decision_id")},
                {"accounting_input_id",input_id},{"book_id",d.at("book_id")},{"source_day",today},{"fills",fills},{"results",results}}},
            {"executions",executions},{"live_results",live},{"equity_curve",curve},{"distance",distance},
            {"corporate_action_adjustments",adjustments},{"layers_applied",adjustments.empty()?J::array():J::array({"corporate-action-basis-restatement"})},
            {"selection_policy","exact-confirmed-choice"}};
        Result<J> success(std::move(output));if(cost_trace)*cost_trace=std::move(traced);return success;
    } catch(const std::exception&) {
        return make_error<J>(ErrorCode::INVALID_DATA,"qt_equity_accounting_input_unavailable","qt_equity_desk_cycle");
    }
}
}
