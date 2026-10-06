#pragma once
#include "trade_ngin/data/qt_empty_model_owner_publication.hpp"
namespace trade_ngin {
// Structural immutable reference only; SQL publication authority is separate.
Result<nlohmann::json> qt_empty_model_owner_reference(const nlohmann::json& owner_document,
    int64_t publication_version,const std::string& producer_version,
    const std::string& registry_id,int64_t registry_revision);
} // namespace trade_ngin
