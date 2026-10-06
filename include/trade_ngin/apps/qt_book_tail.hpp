#pragma once
#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>

namespace trade_ngin {
// Pure financial phase inputs, borrowed after the processor's admission.
// They contain exact full-owner selections, never strategy forecasts. The
// processor retains its transaction, storage, revalidation and receipt order.
struct QtFuturesBookTailInputs {
    const nlohmann::json& decision;
    const nlohmann::json& selection;
    const nlohmann::json& accounting_input;
};
struct QtEquityBookTailInputs {
    const nlohmann::json& decision;
    const nlohmann::json& selection;
    const nlohmann::json& accounting_input;
    const nlohmann::json& producer_authority;
};
Result<nlohmann::json> run_book_tail(const QtFuturesBookTailInputs&);
Result<nlohmann::json> run_book_tail(const QtEquityBookTailInputs&);
}
