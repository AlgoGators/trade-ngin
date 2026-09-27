#include "trade_ngin/apps/qt_processed_report.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include <chrono>
#include <cstdio>
namespace trade_ngin {
Result<QtInvestorReportSnapshot> load_qt_investor_report_snapshot(
    PostgresDatabase& db,const std::string& strategy_id,const std::vector<std::string>& strategy_names,
    const std::string& book,const Timestamp& date,const StrategyPositionRows& system_rows){
    try{
        const auto day=std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(date)};
        char source_day[11];std::snprintf(source_day,sizeof(source_day),"%04d-%02u-%02u",
            int(day.year()),unsigned(day.month()),unsigned(day.day()));
        auto evidence=db.load_qt_processed_report_evidence(book,source_day);
        if(evidence.is_ok()){
            const auto& value=evidence.value();
            if(value.is_object()&&value.contains("workflow_required")&&value.at("workflow_required").is_boolean()){
                if(value.at("workflow_required").get<bool>())
                    return project_processed_qt_report_evidence(value,strategy_id,strategy_names,book,date,system_rows);
                if(value.size()==1){
                    auto legacy=load_qt_report_position_snapshot(db,strategy_id,strategy_names,book,date,system_rows);
                    if(legacy.is_ok())return QtInvestorReportSnapshot{std::move(legacy.value()),std::nullopt};
                }
            }
        }
    }catch(const std::exception&){}
    return make_error<QtInvestorReportSnapshot>(ErrorCode::INVALID_DATA,
        "report_source_unavailable","qt_report_loader");
}
} // namespace trade_ngin
