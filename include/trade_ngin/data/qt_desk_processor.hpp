#pragma once

#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <string>

namespace trade_ngin {
struct QtDeskProcessedReceipt {
    std::string decision_id;
    std::string attempt_id;
    std::string published_book_digest;
    nlohmann::json publication_payload;
    bool replayed = false;
};

// One transaction loads authority and observed fills from SQL, revalidates,
// and publishes QT positions + per-decision result + receipt atomically.
Result<QtDeskProcessedReceipt> process_qt_desk_decision(
    pqxx::connection& connection, const std::string& decision_id,
    const std::string& attempt_id, const std::string& observation_id);

// Explicit confirmed-decision entry: create governed futures observations,
// executions, live results and equity inside the same publication transaction.
// Does not confirm, schedule, send mail, or invoke a legacy runner.
Result<QtDeskProcessedReceipt> process_qt_desk_accounting_decision(
    pqxx::connection& connection, const std::string& decision_id,
    const std::string& attempt_id, const std::string& accounting_input_id);

// Assemble admitted market + finalized prior QT evidence and execute the
// confirmed choice in one transaction. Source capture/finalization are explicit.
Result<QtDeskProcessedReceipt> process_qt_desk_sourced_decision(
    pqxx::connection&,const std::string& decision_id,const std::string& attempt_id,
    const std::string& accounting_input_id,const std::string& market_source_id,
    const std::string& finalization_source_id);
Result<QtDeskProcessedReceipt> process_qt_desk_first_day_decision(
    pqxx::connection&,const std::string& decision_id,const std::string& attempt_id,
    const std::string& accounting_input_id,const std::string& market_source_id,
    const std::string& first_day_anchor_id);

// Explicitly disabled capability is the sole legacy report admission.
Result<nlohmann::json> load_qt_desk_report_evidence(
    pqxx::connection& connection, const std::string& book_id,
    const std::string& source_day);
}  // namespace trade_ngin
