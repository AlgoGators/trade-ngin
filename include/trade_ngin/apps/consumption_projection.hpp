#pragma once

#include <nlohmann/json.hpp>
#include <utility>

namespace trade_ngin {

struct RunConsumption;

enum class ConsumptionUnavailableReason {
    InstrumentationMissing,
    UnsupportedConsumer,
    CapacityExceeded,
    InvalidObservedValue,
    InvalidObservedIdentity,
};

class ConsumptionProjection final {
public:
    const nlohmann::json& document() const noexcept { return document_; }
    static ConsumptionProjection unavailable(ConsumptionUnavailableReason reason);

private:
    explicit ConsumptionProjection(nlohmann::json document)
        : document_(std::move(document)) {}
    friend ConsumptionProjection project_run_consumption(const RunConsumption& run);
    nlohmann::json document_;
};

ConsumptionProjection project_run_consumption(const RunConsumption& run);

// The compiled closed inventory used by the projector; tests reconcile this
// value with the local byte-identical catalog fixture.
const nlohmann::json& consumption_projection_catalog();

}  // namespace trade_ngin
