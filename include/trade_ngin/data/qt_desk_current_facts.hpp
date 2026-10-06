#pragma once

#include "trade_ngin/data/qt_desk_read_set.hpp"
#include <pqxx/pqxx>

namespace trade_ngin {

// A3 legacy risk/audit JSON uses Python json.dumps float spelling rather than
// the public QT wire. This bounded helper must match its literal vectors.
Result<std::string> canonical_qt_desk_source_json(const nlohmann::json& payload);
Result<std::string> canonical_qt_desk_input_json(const nlohmann::json& payload);

Result<QtDeskReadSetCapture> capture_qt_desk_processed_facts(
    pqxx::work& transaction, const nlohmann::json& decision_row,
    const nlohmann::json& preview_row, const nlohmann::json& receipt_row);

// Caller holds auth -> registry -> canonical book -> mutable locks. The proof
// is loaded from the immutable SQL preview, never from a supplied provider.
Result<QtDeskReadSetCapture> capture_qt_desk_current_facts(
    pqxx::work& transaction, const std::string& book_id,
    const std::string& source_day, std::int64_t actor_id,
    const std::string& model_publication_id,
    const nlohmann::json& stored_read_set);

}  // namespace trade_ngin
