#pragma once

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace trade_ngin {

struct QtEvaluationRequest;
struct QtEvaluation;

Result<QtEvaluationRequest> parse_qt_evaluation_request(std::string_view utf8);
Result<Quantity> parse_qt_quantity_exact(std::string_view canonical_decimal8);
Result<nlohmann::json> parse_qt_json(std::string_view utf8);
Result<std::string> canonical_qt_bytes(const nlohmann::json& value);
Result<std::string> canonical_qt_eval_bytes(const nlohmann::json& value);
Result<std::string> qt_digest_v1(const nlohmann::json& value);
Result<std::string> serialize_qt_evaluation(const QtEvaluation& value);

}  // namespace trade_ngin
