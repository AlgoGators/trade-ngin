#pragma once
#include "trade_ngin/apps/equity_run_consumption.hpp"
#include "trade_ngin/live/execution_consumption.hpp"
namespace trade_ngin {
Result<EquityExecutionConsumption> equity_execution_consumption(
    const ExecutionReport&,const DailyExecutionAttempt&,const std::string& portfolio_id,
    const std::string& strategy_id,const std::string& strategy_name,std::size_t index);
}
