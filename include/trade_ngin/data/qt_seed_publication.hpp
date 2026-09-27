#pragma once

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/portfolio/component_book.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace trade_ngin {

struct QtSeedRow {
    ComponentPositionKey key;
    Quantity quantity;
    Price average_price;
};

struct QtProposalManifestRow {
    ComponentPositionKey key;
    Quantity quantity;
    Price average_price;
    std::string action;
    std::optional<std::string> position_revision;
    std::optional<std::string> origin_publication_id;
};

struct QtModelSeedPublication {
    std::string publication_id;
    std::string portfolio_id;
    std::string strategy_id;
    std::string source_day;
    std::vector<QtSeedRow> system_components;
    std::string seed_digest;
    std::string producer_version;
    std::vector<QtProposalManifestRow> proposal_components;
};

// The same seed_rows document is digested by the API's qt-workflow/v1 wire.
Result<nlohmann::json> qt_model_seed_document(const QtModelSeedPublication& publication);
Result<std::string> qt_model_seed_digest(const QtModelSeedPublication& publication);
// Closed producer schema, separate from the public qt-workflow wire validator.
Result<nlohmann::json> qt_proposal_manifest_document(const QtModelSeedPublication& publication);
Result<std::string> qt_proposal_manifest_digest(const QtModelSeedPublication& publication);
Result<nlohmann::json> qt_validate_proposal_manifest_json(
    const nlohmann::json& rows, const std::string& portfolio_id,
    const std::string& strategy_id, const std::string& source_day);

}  // namespace trade_ngin
