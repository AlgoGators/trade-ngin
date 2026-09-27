#include "trade_ngin/apps/qt_equity_position_transition.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace trade_ngin {
namespace {
void need(bool ok) { if(!ok) throw std::invalid_argument("qt_equity_transition_unavailable"); }
void text(const std::string& value) {
    need(!value.empty() && value.size()<=4096 &&
         value.find_first_not_of(" \t\r\n")!=std::string::npos);
}
Timestamp day(const std::string& value) {
    need(value.size()==10 && value[4]=='-' && value[7]=='-');
    for(size_t i=0;i<value.size();++i) if(i!=4 && i!=7) need(value[i]>='0' && value[i]<='9');
    const auto y=std::chrono::year{std::stoi(value.substr(0,4))};
    const auto m=std::chrono::month{static_cast<unsigned>(std::stoi(value.substr(5,2)))};
    const auto d=std::chrono::day{static_cast<unsigned>(std::stoi(value.substr(8,2)))};
    const auto date=std::chrono::year_month_day{y,m,d}; need(date.ok());
    return std::chrono::sys_days{date};
}
Decimal cash(double value) {
    need(std::isfinite(value));
    const auto scaled=value*100000000.0+(value>=0?0.5:-0.5);
    need(std::isfinite(scaled) &&
         static_cast<long double>(scaled)>=static_cast<long double>(INT64_MIN) &&
         static_cast<long double>(scaled)<=static_cast<long double>(INT64_MAX));
    return Decimal::from_raw(static_cast<int64_t>(scaled));
}
Quantity difference(Quantity selected,Quantity previous) {
    const auto a=selected.raw_value(),b=previous.raw_value();
    need(!((b<0 && a>INT64_MAX+b)||(b>0 && a<INT64_MIN+b)));
    return Quantity::from_raw(a-b);
}
double price(const std::optional<double>& value) {
    need(value.has_value() && std::isfinite(*value) && *value>0);
    return *value;
}

void screen_actions(const ComponentPositionKey& key,const Position& previous,
                    const std::vector<CorpActionEvent>& actions) {
    need(actions.size()<=4096);
    std::set<std::tuple<std::string,std::string,CorpActionType>> identities;
    std::string last;
    auto quantity=previous.quantity.as_double(),basis=previous.average_price.as_double();
    for(const auto& action:actions) {
        need(action.symbol==key.symbol && day(action.ex_date)<=day(key.date) &&
             action.ex_date>=last);
        last=action.ex_date;
        need(identities.emplace(action.symbol,action.ex_date,action.type).second);
        need(action.type==CorpActionType::SPLIT || action.type==CorpActionType::ADR_SPLIT ||
             action.type==CorpActionType::DIVIDEND);
        need(action.basis_provenance==CorpActionEvent::BasisProvenance::FORMED_ON_OR_BEFORE_EX_DATE ||
             action.basis_provenance==CorpActionEvent::BasisProvenance::FORMED_AFTER_EX_DATE);
        text(action.basis_provenance_evidence);
        need(std::isfinite(action.value) && action.value>0);
        if(action.type==CorpActionType::DIVIDEND)
            need(std::isfinite(action.close_at_ex_date) && action.close_at_ex_date>0);
        if(action.basis_provenance==CorpActionEvent::BasisProvenance::FORMED_AFTER_EX_DATE)
            continue;
        // The legacy applier skips a flat position; there is no basis to restate.
        if(previous.quantity.is_zero()) continue;
        double factor=action.value;
        if(action.type==CorpActionType::DIVIDEND) {
            // Unknown zero/negative cash-eligibility fallback is outside this
            // strict prerequisite. A producer must supply separately proved data.
            need(std::isfinite(action.qty_at_ex_date) && action.qty_at_ex_date>0);
            factor=1.0+action.value/action.close_at_ex_date;
        }
        need(std::isfinite(factor) && factor>0);
        if(action.type!=CorpActionType::DIVIDEND) quantity*=factor;
        basis/=factor;
        const auto rounded_quantity=cash(quantity),rounded_basis=cash(basis);
        need(!rounded_quantity.is_zero() && rounded_basis.raw_value()>0);
        // Match the applier's Decimal conversion between stacked events.
        quantity=rounded_quantity.as_double(); basis=rounded_basis.as_double();
    }
}
}

Result<QtEquityPositionTransition> produce_qt_equity_position_transition(
    const ComponentPositionKey& key,const std::optional<Position>& previous,
    Quantity selected,std::optional<double> reference_price,
    std::optional<double> mark_price,const Timestamp& timestamp,
    const std::vector<CorpActionEvent>& actions) {
    try {
        for(const auto* field:{&key.portfolio_id,&key.strategy_id,&key.strategy_name,
                              &key.date,&key.symbol,&key.portfolio_type}) text(*field);
        need(key.portfolio_type=="qt" && timestamp==day(key.date));
        Position restated=previous.value_or(Position{});
        if(previous) need(restated.symbol==key.symbol);
        else restated.symbol=key.symbol;
        need(restated.average_price.raw_value()>=0 &&
             (restated.quantity.is_zero() || restated.average_price.raw_value()>0));
        screen_actions(key,restated,actions);
        std::unordered_map<std::string,Position> owned{{key.symbol,restated}};
        auto adjustments=CorporateActionsApplier::apply(owned,actions);
        restated=owned.at(key.symbol);
        need(restated.quantity.is_zero() || restated.average_price.raw_value()>0);
        const auto change=difference(selected,restated.quantity);

        Position position=restated;
        position.quantity=selected; // Retain every selected Decimal8 atom exactly.
        position.realized_pnl=Decimal(0);
        position.unrealized_pnl=Decimal(0);
        position.last_update=timestamp;
        Decimal realized(0);
        if(!change.is_zero()) {
            // Main generate_execution stores Decimal8 fill_price before the
            // strategy reads it for basis and realized P&L arithmetic.
            const auto fill=cash(price(reference_price)).as_double();
            need(fill>0);
            const auto old_quantity=restated.quantity.as_double();
            const auto fill_quantity=std::abs(change.as_double());
            const auto old_basis=restated.average_price.as_double();
            const bool buy=change.is_positive();
            // Pinned main BaseStrategy::on_execution: a flip realizes only
            // the quantity closed against the existing basis, before costs.
            if((old_quantity>0 && !buy)||(old_quantity<0 && buy)) {
                const auto closed=std::min(std::abs(old_quantity),fill_quantity);
                realized=cash((buy?old_basis-fill:fill-old_basis)*closed);
            }
            double basis=old_basis;
            if(buy) {
                const auto new_quantity=old_quantity+fill_quantity;
                if(old_quantity>=0)
                    basis=(old_basis*old_quantity+fill*fill_quantity)/new_quantity;
                // Decimal8 selected sign remains exact even when double
                // cancellation rounds a tiny retained short position to zero.
                else if(!selected.is_negative()) basis=fill;
            } else {
                const auto new_quantity=old_quantity-fill_quantity;
                if(old_quantity<=0)
                    basis=(old_basis*std::abs(old_quantity)+fill*fill_quantity)/std::abs(new_quantity);
                else if(!selected.is_positive()) basis=fill;
            }
            position.average_price=cash(basis);
            position.realized_pnl=realized;
        }
        if(!selected.is_zero()) {
            need(position.average_price.raw_value()>0);
            const auto mark=price(mark_price);
            // Existing equity cost-basis measurement, with one share per unit.
            position.unrealized_pnl=cash(selected.as_double()*(mark-position.average_price.as_double()));
        }
        return QtEquityPositionTransition{key,restated,position,change,realized,std::move(adjustments)};
    } catch(const std::exception&) {
        return make_error<QtEquityPositionTransition>(ErrorCode::INVALID_DATA,
            "qt_equity_transition_unavailable","qt_equity_position_transition");
    }
}
}
