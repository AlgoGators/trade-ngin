#pragma once

#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>

namespace trade_ngin {
// Pure strict futures accounting. The caller must admit the confirmed decision,
// governed input identity, prior QT book/equity and current authority in one SQL
// transaction. This function neither grants authority nor writes/delivers.
// D-day convention matches the live runner: prior-close fills, costs-only D PnL,
// and an explicitly supplied, finalized D-1 equity anchor. No price/capital fallback.
Result<nlohmann::json> produce_qt_futures_accounting(
    const nlohmann::json& decision, const nlohmann::json& selection_rows,
    const nlohmann::json& accounting_inputs);
Result<nlohmann::json> build_qt_desk_diagnostics(
    const nlohmann::json& decision,const nlohmann::json& preview_payload,
    const nlohmann::json& current_facts,const nlohmann::json& accounting_inputs,
    const nlohmann::json& accounting_output);
}
