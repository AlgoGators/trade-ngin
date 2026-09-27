#pragma once
#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <string>

namespace trade_ngin {
// Local immutable node loader only; callers must separately close its prior
// lineage. Never substitutes current physical rows or current policy.
Result<nlohmann::json> load_qt_equity_desk_original_records(pqxx::work&,
    const nlohmann::json& decision,const std::string& input_id);
Result<void> verify_qt_equity_desk_original_outputs(pqxx::work&,
    const nlohmann::json& decision,const std::string& input_id);
// Internal sibling of futures accounting; caller owns confirmed authority,
// book/decision locks and the single receipt/publication transaction.
Result<nlohmann::json> prepare_qt_equity_desk_accounting(pqxx::work&,
    const nlohmann::json& decision,const nlohmann::json& preview,
    const std::string& input_id,const nlohmann::json& current_facts);
Result<void> revalidate_qt_equity_desk_accounting(pqxx::work&,
    const nlohmann::json& decision,const std::string& input_id);
Result<void> store_qt_equity_desk_accounting(pqxx::work&,
    const nlohmann::json& decision,const std::string& input_id,
    const nlohmann::json& output);
Result<void> verify_qt_equity_desk_accounting_outputs(pqxx::work&,
    const nlohmann::json& decision,const std::string& input_id);
Result<void> assemble_qt_equity_desk_accounting_input(pqxx::work&,
    const nlohmann::json& decision,const nlohmann::json& selection,
    const std::string& input_id,const std::string& market_id,
    const std::string& final_id);
}
