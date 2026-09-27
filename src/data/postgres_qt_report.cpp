#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/qt_desk_processor.hpp"
namespace trade_ngin {
Result<nlohmann::json> PostgresDatabase::load_qt_processed_report_evidence(
    const std::string& portfolio_id,const std::string& source_day){
    std::lock_guard<std::mutex> lock(mutex_);
    const auto valid=validate_connection();
    if(valid.is_error()||publication_transaction_!=nullptr)
        return make_error<nlohmann::json>(ErrorCode::DATABASE_ERROR,"report_source_unavailable","PostgresDatabase");
    return load_qt_desk_report_evidence(*connection_,portfolio_id,source_day);
}
} // namespace trade_ngin
