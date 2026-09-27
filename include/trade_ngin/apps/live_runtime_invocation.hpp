#pragma once

#include "trade_ngin/apps/live_portfolio_helpers.hpp"

namespace trade_ngin {
// A scheduler-supplied date is still an operational invocation, never replay
// authorization. Benchmark replay uses its separate recorded-snapshot program.
inline Result<bool> resolve_live_runtime_control(const char* gate,
    const Timestamp& requested_date, const Timestamp& wall_clock) {
    const bool controlled = runtime_control_enabled(gate);
    if (controlled && std::chrono::floor<std::chrono::days>(requested_date) !=
                      std::chrono::floor<std::chrono::days>(wall_clock))
        return make_error<bool>(ErrorCode::INVALID_ARGUMENT,"controlled_historical_run_unsupported");
    return controlled;
}
}  // namespace trade_ngin
