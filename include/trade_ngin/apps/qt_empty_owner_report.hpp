#pragma once
#include "trade_ngin/apps/qt_report_quantity_projection.hpp"
#include <nlohmann/json.hpp>
namespace trade_ngin {
// Presentation mapping only. SQL must independently prove original owner,
// processed accounting/receipt, and complete current physical scope first.
QtReportEligibility build_qt_empty_owner_report_quantity_projection(
    const nlohmann::json& immutable_owner_document,
    const ReportPositionSnapshot& proved_empty_snapshot,
    std::string_view decision_id, std::string_view published_book_digest);
}
