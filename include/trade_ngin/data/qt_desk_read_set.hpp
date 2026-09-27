#pragma once

#include "trade_ngin/core/error.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace trade_ngin {

// Private A3 qt-read-set/v1 bytes, distinct from the public QT workflow wire.
// This does not select current SQL facts or establish desk freshness by itself.
Result<std::string> canonical_qt_desk_read_set_bytes(const nlohmann::json& payload);
Result<std::string> qt_desk_read_set_digest(const nlohmann::json& payload);

struct QtDeskReadSetCapture {
    nlohmann::json payload;
    std::string digest;
    std::string source_day;
    std::string checked_at;
};

// Readiness admission of already selected facts. The caller must select them
// under the actual database transaction locks; this function does not do SQL.
Result<QtDeskReadSetCapture> admit_qt_desk_read_set(
    const nlohmann::json& payload, std::string_view captured_at);

}  // namespace trade_ngin
