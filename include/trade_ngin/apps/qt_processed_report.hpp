#pragma once
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/apps/qt_report_quantity_projection.hpp"

namespace trade_ngin {
struct QtInvestorReportSnapshot {
    ReportPositionSnapshot calculations;
    std::optional<CurrentReportQuantityProjection> display;
};

// Pure presentation admission AFTER the SQL reader verifies immutable receipt
// linkage and current source/accounting freshness. This does not establish SQL
// authority from caller-supplied JSON.
Result<QtInvestorReportSnapshot> project_processed_qt_report_evidence(
    const nlohmann::json& evidence, const std::string& strategy_id,
    const std::vector<std::string>& strategy_names, const std::string& portfolio_id,
    const Timestamp& report_date, const StrategyPositionRows& current_system_rows);

Result<QtInvestorReportSnapshot> load_qt_investor_report_snapshot(
    PostgresDatabase& database, const std::string& strategy_id,
    const std::vector<std::string>& strategy_names, const std::string& portfolio_id,
    const Timestamp& report_date, const StrategyPositionRows& current_system_rows);
} // namespace trade_ngin
