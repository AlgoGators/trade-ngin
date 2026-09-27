#pragma once
#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <string>
namespace trade_ngin {
// Completes local immutable nodes through a bounded iterative prior proof;
// no recursive calls to the public current-output verifier.
Result<void> validate_qt_equity_original_lineage(pqxx::work&,
    const nlohmann::json& original_records);
// Borrow the central book-fenced transaction; no commits or current-policy
// substitution inside historical verification. Null means absent successor.
Result<nlohmann::json> finalize_qt_equity_desk_accounting(pqxx::work&,
    const nlohmann::json& original_decision,const std::string& finalization_id,
    const std::string& market_source_id);
Result<nlohmann::json> verified_qt_equity_desk_finalization(pqxx::work&,
    const nlohmann::json& original_decision);
Result<void> validate_qt_equity_finalized_anchor(pqxx::work&,
    const nlohmann::json& current_decision,const nlohmann::json& market_row,
    const nlohmann::json& final_source_row);
}
