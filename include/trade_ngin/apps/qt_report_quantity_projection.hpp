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
QtReportEligibility build_qt_report_quantity_projection(
    const std::vector<ComponentPositionCandidate>& before_qt,
    const std::vector<ComponentPositionCandidate>& after_qt,
    const ReportPositionSnapshot& legacy_calculation_snapshot,
    std::string_view decision_id, std::string_view published_book_digest);
// Final local renderer guard: validate complete display coverage before output.
bool valid_qt_report_display_rows(
    const std::unordered_map<std::string,std::unordered_map<std::string,Position>>& rows,
    const CurrentReportQuantityProjection& display);
} // namespace trade_ngin
