#pragma once
#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>
namespace trade_ngin {
// Pure mark dependency; supplied provenance never grants processing authority.
Result<nlohmann::json> produce_qt_equity_prior_finalization(
    const nlohmann::json& decision, const nlohmann::json& original_input,
    const nlohmann::json& original_output, const nlohmann::json& market_payload,
    const nlohmann::json& actions_payload, const nlohmann::json& before_financial,
    const nlohmann::json& provenance);
// Market schemas by role. The finalization-only market
// (qt-equity-finalization-market/v1) binds no model and is admitted only as
// the market of an S->D finalization, never as a D accounting input market.
enum class QtEquityMarketRole { Finalization, AccountingInput };
bool qt_equity_market_schema_admitted(const nlohmann::json& market_payload,
    QtEquityMarketRole role) noexcept;
// A qt_desk_market_sources row (to_jsonb) holding a finalization-only market:
// the v1 key set, the same book and day on row and payload, and a null
// model_publication_id on both. There is no publication to load or seed to check.
bool qt_equity_finalization_only_market_row(const nlohmann::json& market_row) noexcept;
}
