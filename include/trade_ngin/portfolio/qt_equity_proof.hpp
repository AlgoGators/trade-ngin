#pragma once
#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>
#include <string_view>
namespace trade_ngin {
// Bounded JSON routing only. Existing qt-eval/v1 semantic admission remains
// independent; parsing a document cannot establish SQL or artifact authority.
Result<nlohmann::json> parse_qt_offline_envelope(std::string_view bytes);
// Offline exact equity recomputation. The caller separately proves original
// SQL authority and the full pinned executable bundle. No expected output,
// database, source paths or mutable financial state is accepted here.
Result<nlohmann::json> recompute_qt_equity_proof(const nlohmann::json& request);
Result<nlohmann::json> recompute_qt_equity_finalization_proof(const nlohmann::json& request);
}
