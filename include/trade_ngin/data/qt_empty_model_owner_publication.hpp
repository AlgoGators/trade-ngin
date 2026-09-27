#pragma once
#include "trade_ngin/data/qt_seed_publication.hpp"
namespace trade_ngin {
// Each entry is captured by a real explicitly dated system batch callback.
// The SQL publisher must bind it to the actual pending governed MODEL scope.
struct QtModelEmptyBatchScope {
    std::string portfolio_id,strategy_id,strategy_name,source_day;
};
struct QtEmptyModelOwnerPublication {
    QtModelSeedPublication publication;
    nlohmann::json configuration_snapshot;
    std::vector<std::string> configured_owner_names;
    std::vector<QtModelEmptyBatchScope> fresh_empty_batches;
    std::vector<QtSeedRow> qt_components;
};
// Explicit additive v2 only. Existing v1 seed function remains unchanged.
Result<nlohmann::json> qt_empty_model_owner_document(const QtEmptyModelOwnerPublication&);
Result<std::string> canonical_qt_empty_model_owner_bytes(const nlohmann::json& closed_document);
} // namespace trade_ngin
