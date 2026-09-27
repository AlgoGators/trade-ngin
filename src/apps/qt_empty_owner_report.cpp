#include "trade_ngin/apps/qt_empty_owner_report.hpp"
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/data/qt_empty_model_owner_publication.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include <chrono>
#include <cstdio>
#include <stdexcept>
namespace trade_ngin {
QtReportEligibility build_qt_empty_owner_report_quantity_projection(
    const nlohmann::json& owner, const ReportPositionSnapshot& snapshot,
    std::string_view decision, std::string_view published) {
    const QtReportEligibility unavailable{"unavailable", {"qt_report_row_mapping_changed"}, std::nullopt, std::nullopt};
    try {
        auto bytes = canonical_qt_empty_model_owner_bytes(owner);
        if(bytes.is_error() || owner.at("schema_version") != "qt-empty-model-owner-publication/v2" ||
           owner.at("configured_owner_names").size() != 1 ||
           owner.at("strategy_id") != "LIVE_EQUITY_MEAN_REVERSION" ||
           !owner.at("system_components").empty() || !owner.at("proposal_components").empty() ||
           !owner.at("qt_components").empty()) return unavailable;
        const auto name = owner.at("configured_owner_names")[0].get<std::string>();
        const auto day = std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(snapshot.date)};
        char date[11]; std::snprintf(date, sizeof(date), "%04d-%02u-%02u", int(day.year()), unsigned(day.month()), unsigned(day.day()));
        if(snapshot.portfolio_type != "qt" || owner.at("book_id") != snapshot.portfolio_id ||
           owner.at("strategy_id") != snapshot.strategy_id || owner.at("source_day") != date ||
           snapshot.strategy_names != std::vector<std::string>{name} ||
           snapshot.by_strategy.size() != 1 || !snapshot.by_strategy.contains(name) ||
           !snapshot.by_strategy.at(name).empty() || !snapshot.combined.empty() ||
           snapshot.evidence_counts.size() != 1 || !snapshot.evidence_counts.contains(name) ||
           snapshot.evidence_counts.at(name) != 0) return unavailable;
        auto selected = qt_digest_v1(nlohmann::json{{"selection_rows", nlohmann::json::array()}});
        auto manifest = qt_digest_v1(nlohmann::json{{"component_keys", nlohmann::json::array()}});
        if(selected.is_error() || manifest.is_error() || selected.value() != published) return unavailable;
        CurrentReportQuantityProjection projection{{}, std::string(decision), std::string(published), manifest.value()};
        if(!valid_qt_report_display_rows(snapshot.by_strategy, projection)) return unavailable;
        return {"eligible", {}, manifest.value(), std::move(projection)};
    } catch(const std::exception&) { return unavailable; }
}
}
