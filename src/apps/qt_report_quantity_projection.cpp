#include "trade_ngin/apps/qt_report_quantity_projection.hpp"
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
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
} // namespace

QtReportEligibility build_qt_report_quantity_projection(
    const std::vector<ComponentPositionCandidate>& before,
    const std::vector<ComponentPositionCandidate>& after,
    const ReportPositionSnapshot& snapshot,std::string_view decision,std::string_view published) {
    try {
        if(!uuid(decision)||!hash(published)||before.empty()||snapshot.portfolio_type!="qt"||
           snapshot.portfolio_id.empty()||snapshot.strategy_id.empty()||before.size()!=after.size()) return unavailable();
        const auto date=utc_day(snapshot.date);
        std::set<std::string> strategies(snapshot.strategy_names.begin(),snapshot.strategy_names.end());
        if(strategies.empty()||strategies.size()!=snapshot.strategy_names.size()||strategies.contains("")||
           snapshot.by_strategy.size()!=strategies.size()||snapshot.evidence_counts.size()!=strategies.size()) return unavailable();
        std::map<ComponentPositionKey,const ComponentPositionCandidate*> before_by_key,after_by_key;
        std::map<CurrentReportRowKey,ComponentPositionKey> manifest;
        std::map<std::string,size_t> counts;
        size_t rendered=0;
        for(const auto& row:before) {
            const auto& k=row.key;
            if(k.portfolio_id!=snapshot.portfolio_id||k.strategy_id!=snapshot.strategy_id||
               k.date!=date||k.portfolio_type!="qt"||!strategies.contains(k.strategy_name)||k.symbol.empty()||
               row.position.symbol!=k.symbol||row.instrument.symbol!=k.symbol||
               (row.instrument.type!=AssetType::EQUITY&&row.instrument.type!=AssetType::FUTURE)||
               (row.instrument.type==AssetType::FUTURE&&row.position.quantity.raw_value()%Quantity(1).raw_value()!=0)||
               !before_by_key.emplace(k,&row).second) return unavailable();
            ++counts[k.strategy_name];
            const auto& grouped=snapshot.by_strategy.at(k.strategy_name);
            const auto displayed=grouped.find(k.symbol);
            if(row.position.quantity.is_zero()) {
                if(displayed!=grouped.end()) return unavailable();
            } else {
                if(displayed==grouped.end()||!same_position(displayed->second,row.position)||
                   !manifest.emplace(CurrentReportRowKey{k.strategy_name,k.symbol},k).second) return unavailable();
                ++rendered;
            }
        }
        size_t snapshot_rows=0;
        std::unordered_map<std::string,Position> expected_combined;
        for(const auto& strategy:snapshot.strategy_names) {
            if(snapshot.evidence_counts.at(strategy)!=counts[strategy]) return unavailable();
            for(const auto& [symbol,position]:snapshot.by_strategy.at(strategy)) {
                ++snapshot_rows;
                if(position.quantity.is_zero()||position.symbol!=symbol||!manifest.contains({strategy,symbol})) return unavailable();
                auto [entry,inserted]=expected_combined.emplace(symbol,position);
                if(!inserted) entry->second.quantity+=position.quantity;
            }
        }
        if(snapshot_rows!=rendered||snapshot.combined.size()!=expected_combined.size()) return unavailable();
        for(const auto& [symbol,position]:expected_combined)
            if(!snapshot.combined.contains(symbol)||!same_position(position,snapshot.combined.at(symbol))) return unavailable();
        Json selected=Json::array(),keys=Json::array();
        CurrentReportQuantityProjection projection{{},std::string(decision),std::string(published),{}};
        for(const auto& row:after) {
            const auto previous=before_by_key.find(row.key);
            if(previous==before_by_key.end()||!after_by_key.emplace(row.key,&row).second||
               row.position.symbol!=row.key.symbol||row.instrument!=previous->second->instrument||
               (row.instrument.type==AssetType::FUTURE&&row.position.quantity.raw_value()%Quantity(1).raw_value()!=0)||
               (previous->second->position.quantity.is_zero()&&!row.position.quantity.is_zero())) return unavailable();
            selected.push_back({{"key",key_json(row.key)},{"quantity_exact",row.position.quantity.to_string()}});
            if(!previous->second->position.quantity.is_zero()) {
                projection.quantity_exact.emplace(CurrentReportRowKey{row.key.strategy_name,row.key.symbol},row.position.quantity.to_string());
                keys.push_back(key_json(row.key));
            }
        }
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
        if(position.quantity.is_zero()||position.symbol!=symbol) return false;
        ++count;
        const auto found=display.quantity_exact.find({strategy,symbol});
        if(found==display.quantity_exact.end()) return false;
        const auto exact=parse_qt_quantity_exact(found->second);
        if(exact.is_error()) return false;
    }
    return count==display.quantity_exact.size();
}
} // namespace trade_ngin
