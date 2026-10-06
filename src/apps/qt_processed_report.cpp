#include "trade_ngin/apps/qt_processed_report.hpp"
#include "trade_ngin/apps/qt_empty_owner_report.hpp"
#include "trade_ngin/data/qt_desk_publication.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include <chrono>
#include <algorithm>
#include <limits>
#include <regex>
#include <set>
#include <stdexcept>

namespace trade_ngin {
namespace {
using Json=nlohmann::json;
[[noreturn]] void reject(){throw std::invalid_argument("report_snapshot_stale");}
void fields(const Json& value,std::initializer_list<const char*> expected){
    if(!value.is_object()||value.size()!=expected.size())reject();
    for(auto name:expected)if(!value.contains(name))reject();
}
std::string text(const Json& value){if(!value.is_string())reject();return value.get<std::string>();}
std::string utc_day(Timestamp value){
    const auto day=std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(value)};
    char buffer[11];std::snprintf(buffer,sizeof(buffer),"%04d-%02u-%02u",int(day.year()),unsigned(day.month()),unsigned(day.day()));
    return buffer;
}
Timestamp timestamp(const Json& value){
    const auto input=text(value);
    static const std::regex format(R"(^([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})(?:\.([0-9]{1,6}))?Z$)");
    std::smatch match;if(!std::regex_match(input,match,format))reject();
    const std::chrono::year_month_day date{std::chrono::year(std::stoi(match[1])),
        std::chrono::month(static_cast<unsigned>(std::stoi(match[2]))),
        std::chrono::day(static_cast<unsigned>(std::stoi(match[3])))};
    const int hours=std::stoi(match[4]),minutes=std::stoi(match[5]),seconds=std::stoi(match[6]);
    if(!date.ok()||int(date.year())<1||hours>23||minutes>59||seconds>59)reject();
    auto fraction=match[7].str();fraction.append(6-fraction.size(),'0');
    const auto micros=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::sys_days(date).time_since_epoch()+std::chrono::hours(hours)+
        std::chrono::minutes(minutes)+std::chrono::seconds(seconds)).count()+std::stoll(fraction);
    const auto lower=std::chrono::duration_cast<std::chrono::microseconds>(Timestamp::duration::min()).count();
    const auto upper=std::chrono::duration_cast<std::chrono::microseconds>(Timestamp::duration::max()).count();
    if(micros<lower||micros>upper)reject();
    return Timestamp(std::chrono::duration_cast<Timestamp::duration>(std::chrono::microseconds(micros)));
}
Quantity exact(const Json& value){
    auto parsed=parse_qt_quantity_exact(text(value));if(parsed.is_error())reject();return parsed.value();
}
ComponentPositionKey key(const Json& value){
    fields(value,{"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"});
    return {text(value.at("portfolio_id")),text(value.at("strategy_id")),text(value.at("strategy_name")),
        text(value.at("date")),text(value.at("symbol")),text(value.at("portfolio_type"))};
}
std::vector<ComponentPositionCandidate> accounting(const Json& rows,const Json& selection){
    if(!rows.is_array()||rows.size()>4096)reject();
    std::map<ComponentPositionKey,std::pair<AssetType,bool>> types;
    for(const auto& row:selection){
        auto k=key(row.at("key"));k.portfolio_type="qt";
        const auto type=text(row.at("asset_type"));
        if(type!="EQUITY"&&type!="FUTURE")reject();
        if(!types.emplace(k,std::make_pair(type=="EQUITY"?AssetType::EQUITY:AssetType::FUTURE,
            row.at("editable").get<bool>())).second)reject();
    }
    std::vector<ComponentPositionCandidate> result;std::set<ComponentPositionKey> seen;
    for(const auto& row:rows){
        fields(row,{"key","quantity_exact","average_price_exact","daily_unrealized_pnl_exact","daily_realized_pnl_exact","last_update"});
        const auto k=key(row.at("key"));const auto found=types.find(k);
        if(k.portfolio_type!="qt"||found==types.end()||!seen.insert(k).second)reject();
        Position position(k.symbol,exact(row.at("quantity_exact")),exact(row.at("average_price_exact")),
            exact(row.at("daily_unrealized_pnl_exact")),exact(row.at("daily_realized_pnl_exact")),timestamp(row.at("last_update")));
        result.push_back({k,{found->second.first,k.symbol},found->second.second,std::nullopt,std::move(position),false});
    }
    return result;
}
} // namespace

Result<QtInvestorReportSnapshot> project_processed_qt_report_evidence(
    const Json& evidence,const std::string& strategy_id,const std::vector<std::string>& strategy_names,
    const std::string& portfolio_id,const Timestamp& report_date,const StrategyPositionRows& current_system_rows){
    try{
        const bool empty_owner=evidence.contains("empty_owner");
        if(empty_owner)fields(evidence,{"workflow_required","decision","preview","receipt","empty_owner"});
        else fields(evidence,{"workflow_required","decision","preview","receipt"});
        if(!evidence.at("workflow_required").is_boolean()||!evidence.at("workflow_required").get<bool>())reject();
        const auto& decision=evidence.at("decision");const auto& preview=evidence.at("preview");const auto& receipt=evidence.at("receipt");
        auto selected=validate_qt_desk_decision_snapshot(decision,preview);if(selected.is_error())reject();
        if(receipt.at("status")!="processed"||receipt.at("report_eligibility_status")!="eligible"||
            !receipt.at("report_reason_codes").is_array()||!receipt.at("report_reason_codes").empty()||
            receipt.at("decision_id")!=decision.at("decision_id")||
            receipt.at("published_book_digest")!=decision.at("selected_book_digest"))reject();
        const auto& publication=receipt.at("publication_payload");
        fields(publication,{"schema_version","decision_id","attempt_id","observation_id","book_id","source_day",
            "model_publication_id","preview_payload_digest","read_set_digest","selected_book_digest","published_book_digest",
            "observation_digest","results_digest","before_accounting","after_accounting","report_scope"});
        if(publication.at("schema_version")!="qt-desk-publication/v1"||publication.at("decision_id")!=decision.at("decision_id")||
            publication.at("attempt_id")!=receipt.at("attempt_id")||publication.at("book_id")!=portfolio_id||
            publication.at("source_day")!=utc_day(report_date)||publication.at("model_publication_id")!=decision.at("model_publication_id")||
            publication.at("preview_payload_digest")!=preview.at("payload_digest")||
            publication.at("read_set_digest")!=preview.at("read_set_digest")||
            publication.at("selected_book_digest")!=decision.at("selected_book_digest")||
            publication.at("published_book_digest")!=receipt.at("published_book_digest")||
            publication.at("before_accounting")!=preview.at("read_set_payload").at("saved_accounting"))reject();
        auto canonical_names=strategy_names;std::sort(canonical_names.begin(),canonical_names.end());
        const Json expected_scope={{"portfolio_id",portfolio_id},{"strategy_id",strategy_id},{"strategy_names",canonical_names},
            {"portfolio_type","qt"},{"date",utc_day(report_date)}};
        if(publication.at("report_scope")!=expected_scope)reject();
        const auto before=accounting(publication.at("before_accounting"),preview.at("payload").at("selection_rows"));
        const auto after=accounting(publication.at("after_accounting"),preview.at("payload").at("selection_rows"));
        std::map<ComponentPositionKey,Quantity> quantities;
        for(const auto& row:after)quantities.emplace(row.key,row.position.quantity);
        if(quantities.size()!=selected.value().selected_rows.size())reject();
        for(const auto& row:selected.value().selected_rows)
            if(!quantities.contains(row.key)||quantities.at(row.key)!=row.quantity)reject();
        std::set<std::string> names;std::set<CurrentReportRowKey> raw_rows;
        for(const auto& name:strategy_names){if(name.empty()||!names.insert(name).second)reject();}
        for(const auto& row:before){
            if(!names.contains(row.key.strategy_name)||row.key.portfolio_id!=portfolio_id||row.key.strategy_id!=strategy_id||
                row.key.date!=utc_day(report_date)||!raw_rows.insert({row.key.strategy_name,row.key.symbol}).second)reject();
        }
        for(const auto& [name,rows]:current_system_rows){
            if(!names.contains(name))reject();
            for(const auto& [symbol,position]:rows)
                if(position.symbol!=symbol||(!position.quantity.is_zero()&&!raw_rows.contains({name,symbol})))reject();
        }
        ReportPositionSnapshot snapshot=build_qt_saved_report_snapshot(before,after,strategy_names,portfolio_id,strategy_id,report_date);
        if(empty_owner){
            if(!before.empty()||!after.empty()||!selected.value().selected_rows.empty())reject();
            for(const auto& [name,rows]:current_system_rows)if(!rows.empty())reject();
        }
        const auto projection=empty_owner?
            build_qt_empty_owner_report_quantity_projection(evidence.at("empty_owner"),snapshot,text(receipt.at("decision_id")),text(receipt.at("published_book_digest"))):
            build_qt_report_quantity_projection(before,after,snapshot,text(receipt.at("decision_id")),text(receipt.at("published_book_digest")));
        if(projection.status!="eligible"||!projection.projection||!projection.row_manifest_digest||
            *projection.row_manifest_digest!=text(receipt.at("row_manifest_digest")))reject();
        return QtInvestorReportSnapshot{std::move(snapshot),projection.projection};
    }catch(const std::exception&){
        return make_error<QtInvestorReportSnapshot>(ErrorCode::INVALID_DATA,"report_snapshot_stale","qt_processed_report");
    }
}
} // namespace trade_ngin
