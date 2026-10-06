#pragma once
#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
namespace trade_ngin {
// Explicit trusted producer capture. No database discovery, fallback data or
// authority activation; caller supplies already obtained immutable operands.
Result<nlohmann::json> publish_qt_desk_market_source(pqxx::connection&,const nlohmann::json& source);
Result<nlohmann::json> finalize_qt_desk_accounting(pqxx::connection&,const std::string& decision_id,
    const std::string& finalization_id,const std::string& market_source_id);
// Null means no transition exists. An existing but invalid transition is an
// error. The returned financial successor is calculated again from its links.
Result<nlohmann::json> verified_qt_desk_finalization(pqxx::work&,const nlohmann::json& decision);
Result<void> assemble_qt_desk_accounting_input(pqxx::work&,const nlohmann::json& decision,
    const nlohmann::json& selection,const std::string& input_id,
    const std::string& market_source_id,const std::string& finalization_source_id);
Result<void> validate_qt_desk_upstream_input(pqxx::work&,const nlohmann::json& decision,
    const nlohmann::json& input_row,const nlohmann::json& finalization_row);
// First live QT day: derive one immutable opening anchor from the current-day
// System publication and financial rows. No caller supplies financial values.
Result<nlohmann::json> create_qt_first_day_anchor(pqxx::connection&,
    const std::string& decision_id,const std::string& anchor_id);
Result<void> assemble_qt_first_day_accounting_input(pqxx::work&,
    const nlohmann::json& decision,const nlohmann::json& selection,
    const std::string& input_id,const std::string& market_source_id,
    const std::string& anchor_id);
Result<void> validate_qt_first_day_accounting_input(pqxx::work&,
    const nlohmann::json& decision,const nlohmann::json& input_row,bool fresh);
}
