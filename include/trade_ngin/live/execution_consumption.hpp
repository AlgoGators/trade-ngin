#pragma once

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/transaction_cost/consumption.hpp"
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace trade_ngin {

enum class ExecutionCallState {
    entered,
    rejected_stream,
    rejected_id,
    cost_call_reached,
    returned
};

struct ExecutionCallObservation {
    ExecutionCallState state = ExecutionCallState::entered;
    transaction_cost::CostChargeObservation cost;
};

enum class DailyPositionBranch { current_position, removed_position };
enum class ExecutionPriceSource {
    market_prices,
    current_average_price,
    previous_average_price
};

struct DailyExecutionAttempt {
    std::string symbol;
    size_t sequence = 0;
    DailyPositionBranch branch = DailyPositionBranch::current_position;
    ExecutionPriceSource price_source = ExecutionPriceSource::market_prices;
    double selected_price = 0.0;
    ExecutionCallObservation execution;
    bool returned = false;
};

enum class DailyExecutionState {
    entered,
    rejected_stream,
    returned,
    invalid_argument
};

struct DailyExecutionObservation {
    DailyExecutionState state = DailyExecutionState::entered;
    std::optional<ErrorCode> error_code;
    std::vector<DailyExecutionAttempt> attempts;
};

enum class PreviousCloseSource { initial_current_close, stored_previous_close };

struct ExecutionMarketDataObservation {
    bool cost_model_reached = false;
    std::optional<PreviousCloseSource> previous_close_source;
    std::optional<double> previous_close_forwarded;
    transaction_cost::MarketDataObservation market_data;
};

}  // namespace trade_ngin
