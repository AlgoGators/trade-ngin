#include "trade_ngin/apps/qt_report_quantity_projection.hpp"
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace trade_ngin {
namespace {
using Json=nlohmann::json;
QtReportEligibility unavailable() {
    return {"unavailable",{"qt_report_row_mapping_changed"},std::nullopt,std::nullopt};
}
bool hash(std::string_view value) {
    return value.size()==64 && value.find_first_not_of("0123456789abcdef")==std::string_view::npos;
}
bool uuid(std::string_view value) {
    if(value.size()!=36) return false;
    for(size_t i=0;i<value.size();++i) {
        if(i==8||i==13||i==18||i==23) {if(value[i]!='-') return false;}
        else if(std::string_view("0123456789abcdef").find(value[i])==std::string_view::npos) return false;
    }
    return true;
}
bool same_position(const Position& a,const Position& b) {
    return a.symbol==b.symbol && a.quantity==b.quantity && a.average_price==b.average_price &&
        a.unrealized_pnl==b.unrealized_pnl && a.realized_pnl==b.realized_pnl && a.last_update==b.last_update;
}
Json key_json(const ComponentPositionKey& key) {
    return {{"portfolio_id",key.portfolio_id},{"strategy_id",key.strategy_id},
        {"strategy_name",key.strategy_name},{"date",key.date},{"symbol",key.symbol},
        {"portfolio_type",key.portfolio_type}};
}
std::string utc_day(Timestamp timestamp) {
    const auto seconds=std::chrono::system_clock::to_time_t(timestamp);
    std::tm parsed{};
#ifdef _WIN32
    if(gmtime_s(&parsed,&seconds)!=0) throw std::invalid_argument("date");
#else
    if(gmtime_r(&seconds,&parsed)==nullptr) throw std::invalid_argument("date");
#endif
    std::ostringstream result; result<<std::put_time(&parsed,"%Y-%m-%d");return result.str();
}
// Checked like qt_desk_processor.cpp add(): Decimal::operator+ relies on signed wraparound.
Quantity sum(const Quantity& a,const Quantity& b) {
    const auto x=a.raw_value(),y=b.raw_value();
    if((y>0&&x>INT64_MAX-y)||(y<0&&x<INT64_MIN-y)) throw std::invalid_argument("quantity_overflow");
    return Quantity::from_raw(x+y);
}
} // namespace

ReportPositionSnapshot build_qt_saved_report_snapshot(
    const std::vector<ComponentPositionCandidate>& before,const std::vector<ComponentPositionCandidate>& after,
    const std::vector<std::string>& strategy_names,const std::string& portfolio_id,
    const std::string& strategy_id,Timestamp report_date) {
    std::map<ComponentPositionKey,const ComponentPositionCandidate*> old;
    for(const auto& row:before) if(!old.emplace(row.key,&row).second) throw std::invalid_argument("duplicate_before_key");
    StrategyPositionRows grouped; std::unordered_map<std::string,Position> combined;
    std::unordered_map<std::string,size_t> counts;
    for(const auto& name:strategy_names){grouped[name];counts[name]=0;}
    for(const auto& row:before) ++counts.at(row.key.strategy_name);
    // Saved keys must cover every before key; a saved key absent before is treated as before = 0.
    std::set<ComponentPositionKey> saved_keys;
    for(const auto& row:after) {
        if(!grouped.contains(row.key.strategy_name)) throw std::invalid_argument("after_key_outside_report_strategies");
        saved_keys.insert(row.key);
    }
    for(const auto& [key,row]:old) if(!saved_keys.contains(key)) throw std::invalid_argument("before_key_not_in_after");
    for(const auto& name:strategy_names) for(const auto& row:after) {
        if(row.key.strategy_name!=name) continue;
        const auto previous=old.find(row.key);
        const bool was=previous!=old.end()&&!previous->second->position.quantity.is_zero();
        if(row.position.quantity.is_zero()&&!was) continue;
        if(!grouped.at(name).emplace(row.key.symbol,row.position).second) throw std::invalid_argument("duplicate_report_row");
        if(row.position.quantity.is_zero()) continue;
        auto [entry,inserted]=combined.emplace(row.key.symbol,row.position);
        if(!inserted) entry->second.quantity=sum(entry->second.quantity,row.position.quantity);
    }
    return {std::move(grouped),std::move(combined),portfolio_id,strategy_id,strategy_names,"qt",report_date,std::move(counts)};
}

QtReportEligibility build_qt_report_quantity_projection(
    const std::vector<ComponentPositionCandidate>& before,const std::vector<ComponentPositionCandidate>& after,
    const ReportPositionSnapshot& snapshot,std::string_view decision,std::string_view published) {
    try {
        // Saved keys are a superset of before keys: after.empty() is the only size refusal left.
        if(!uuid(decision)||!hash(published)||after.empty()||snapshot.portfolio_type!="qt"||
           snapshot.portfolio_id.empty()||snapshot.strategy_id.empty()) return unavailable();
        const auto date=utc_day(snapshot.date);
        std::set<std::string> strategies(snapshot.strategy_names.begin(),snapshot.strategy_names.end());
        if(strategies.empty()||strategies.size()!=snapshot.strategy_names.size()||strategies.contains("")||
           snapshot.by_strategy.size()!=strategies.size()||snapshot.evidence_counts.size()!=strategies.size()) return unavailable();
        const auto whole=[](const ComponentPositionCandidate& row){
            return row.instrument.type!=AssetType::FUTURE||row.position.quantity.raw_value()%Quantity(1).raw_value()==0;};
        std::map<ComponentPositionKey,const ComponentPositionCandidate*> old,saved;
        std::map<std::string,size_t> counts;
        // Every row, before or saved, is in the report's scope with a typed instrument of its own.
        const auto scoped=[&](const ComponentPositionCandidate& row){
            const auto& k=row.key;
            return k.portfolio_id==snapshot.portfolio_id&&k.strategy_id==snapshot.strategy_id&&k.date==date&&
               k.portfolio_type=="qt"&&strategies.contains(k.strategy_name)&&!k.symbol.empty()&&
               row.position.symbol==k.symbol&&row.instrument.symbol==k.symbol&&
               (row.instrument.type==AssetType::EQUITY||row.instrument.type==AssetType::FUTURE)&&whole(row);
        };
        for(const auto& row:before) {
            if(!scoped(row)||!old.emplace(row.key,&row).second) return unavailable();
            ++counts[row.key.strategy_name];
        }
        for(const auto& row:after) {
            // A saved key absent before is treated as before = 0; its instrument comes from the saved row.
            const auto previous=old.find(row.key);
            if(!scoped(row)||(previous!=old.end()&&row.instrument!=previous->second->instrument)||
               !saved.emplace(row.key,&row).second) return unavailable();
        }
        for(const auto& [k,row]:old) if(!saved.contains(k)) return unavailable(); // a before key missing from after
        const auto was_nonzero=[&](const ComponentPositionKey& k){
            const auto previous=old.find(k); return previous!=old.end()&&!previous->second->position.quantity.is_zero();};
        for(const auto& strategy:snapshot.strategy_names)
            if(snapshot.evidence_counts.at(strategy)!=counts[strategy]) return unavailable();
        // Shown rows: saved nonzero, or closed today (nonzero before, zero after). Sorted key order.
        CurrentReportQuantityProjection projection{{},std::string(decision),std::string(published),{}};
        Json keys=Json::array(); size_t shown=0;
        std::unordered_map<std::string,Position> expected_combined;
        for(const auto& strategy:snapshot.strategy_names) for(const auto& [k,row]:saved) {
            if(k.strategy_name!=strategy) continue;
            const bool now=!row->position.quantity.is_zero(),was=was_nonzero(k);
            if(!now&&!was) continue;
            const auto& grouped=snapshot.by_strategy.at(strategy); const auto displayed=grouped.find(k.symbol);
            if(displayed==grouped.end()||!same_position(displayed->second,row->position)||
               !projection.quantity_exact.emplace(CurrentReportRowKey{k.strategy_name,k.symbol},row->position.quantity.to_string()).second)
                return unavailable();
            ++shown;
            if(now){auto [entry,inserted]=expected_combined.emplace(k.symbol,row->position);if(!inserted)entry->second.quantity=sum(entry->second.quantity,row->position.quantity);}
        }
        size_t snapshot_rows=0;
        for(const auto& [strategy,rows]:snapshot.by_strategy) snapshot_rows+=rows.size();
        if(snapshot_rows!=shown||snapshot.combined.size()!=expected_combined.size()) return unavailable();
        for(const auto& [symbol,position]:expected_combined)
            if(!snapshot.combined.contains(symbol)||!same_position(position,snapshot.combined.at(symbol))) return unavailable();
        for(const auto& [k,row]:saved) if(projection.quantity_exact.contains({k.strategy_name,k.symbol})&&
            (!row->position.quantity.is_zero()||was_nonzero(k))) keys.push_back(key_json(k));
        Json selected=Json::array();
        for(const auto& row:after) selected.push_back({{"key",key_json(row.key)},{"quantity_exact",row.position.quantity.to_string()}});
        const auto selected_digest=qt_digest_v1({{"selection_rows",selected}});
        const auto manifest_digest=qt_digest_v1({{"component_keys",keys}});
        if(selected_digest.is_error()||selected_digest.value()!=published||manifest_digest.is_error()) return unavailable();
        projection.row_manifest_digest=manifest_digest.value();
        if(!valid_qt_report_display_rows(snapshot.by_strategy,projection)) return unavailable();
        return {"eligible",{},manifest_digest.value(),std::move(projection)};
    } catch(const std::exception&) {return unavailable();}
}

bool valid_qt_report_display_rows(
    const std::unordered_map<std::string,std::unordered_map<std::string,Position>>& rows,
    const CurrentReportQuantityProjection& display) {
    if(!uuid(display.decision_id)||!hash(display.published_book_digest)||!hash(display.row_manifest_digest)) return false;
    size_t count=0;
    for(const auto& [strategy,positions]:rows) for(const auto& [symbol,position]:positions) {
        if(position.symbol!=symbol) return false;
        ++count;
        const auto found=display.quantity_exact.find({strategy,symbol});
        if(found==display.quantity_exact.end()) return false;
        const auto exact=parse_qt_quantity_exact(found->second);
        if(exact.is_error()||exact.value()!=position.quantity) return false;
    }
    return count==display.quantity_exact.size();
}
} // namespace trade_ngin
