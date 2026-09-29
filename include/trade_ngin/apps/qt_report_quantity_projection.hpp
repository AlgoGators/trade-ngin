#pragma once
#include "trade_ngin/portfolio/component_book.hpp"
#include <map>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace trade_ngin {
struct ReportPositionSnapshot;
struct CurrentReportRowKey {
    std::string strategy_name, symbol;
    bool operator<(const CurrentReportRowKey& other) const {
        return std::tie(strategy_name,symbol)<std::tie(other.strategy_name,other.symbol);
    }
    bool operator==(const CurrentReportRowKey&) const = default;
};
struct CurrentReportQuantityProjection {
    std::map<CurrentReportRowKey,std::string> quantity_exact;
    std::string decision_id, published_book_digest, row_manifest_digest;
};
struct QtReportEligibility {
    std::string status;
    std::vector<std::string> reason_codes;
    std::optional<std::string> row_manifest_digest;
    std::optional<CurrentReportQuantityProjection> projection;
};
// Saved (after) keys must cover every before key; a saved key with no before row is treated as
// before = 0 and must satisfy the same scope, type and whole-FUTURE checks as every other row.
QtReportEligibility build_qt_report_quantity_projection(
    const std::vector<ComponentPositionCandidate>& before_qt,
    const std::vector<ComponentPositionCandidate>& after_qt,
    const ReportPositionSnapshot& saved_report_snapshot,
    std::string_view decision_id, std::string_view published_book_digest);
// Report snapshot from QT's saved positions: every nonzero saved row (including keys that had no
// row before), plus rows closed today (nonzero before, zero after) at 0; the combined map sums
// nonzero rows by symbol.
ReportPositionSnapshot build_qt_saved_report_snapshot(
    const std::vector<ComponentPositionCandidate>& before_qt,
    const std::vector<ComponentPositionCandidate>& after_qt,
    const std::vector<std::string>& strategy_names, const std::string& portfolio_id,
    const std::string& strategy_id, Timestamp report_date);
// Final local renderer guard: validate complete display coverage before output.
bool valid_qt_report_display_rows(
    const std::unordered_map<std::string,std::unordered_map<std::string,Position>>& rows,
    const CurrentReportQuantityProjection& display);
} // namespace trade_ngin
