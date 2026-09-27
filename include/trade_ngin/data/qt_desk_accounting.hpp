#pragma once
#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
namespace trade_ngin {
// Internal orchestration seam; always called inside the confirmed processor's
// transaction, after authorization and read-set validation.
Result<nlohmann::json> prepare_qt_desk_accounting(pqxx::work&,const nlohmann::json& decision,
    const nlohmann::json& preview,const std::string& input_id,const nlohmann::json& current_facts);
Result<void> store_qt_desk_accounting(pqxx::work&,const nlohmann::json& decision,
    const std::string& input_id,const nlohmann::json& output);
Result<void> revalidate_qt_desk_accounting(pqxx::work&,const nlohmann::json& decision,
    const std::string& input_id);
Result<void> verify_qt_desk_accounting_outputs(pqxx::work&,const nlohmann::json& decision,
    const std::string& input_id);
}
