#pragma once
#include <utility>
#include <nlohmann/json.hpp>

namespace trade_ngin {
struct EquityRunConsumption;
// Distinct private projection. A caller JSON object is never trusted as native trace.
class EquityRunProjection final {
public:
    const nlohmann::json& document() const noexcept { return document_; }
private:
    explicit EquityRunProjection(nlohmann::json document):document_(std::move(document)) {}
    friend EquityRunProjection project_equity_run_consumption(const EquityRunConsumption&);
    nlohmann::json document_;
};
EquityRunProjection project_equity_run_consumption(const EquityRunConsumption&);
} // namespace trade_ngin
