#pragma once

#include "trade_ngin/backtest/backtest_types.hpp"

namespace trade_ngin::api {

trade_ngin::backtest::BacktestResults run_backtest();

// Example usage in bt_portfolio.cpp
// auto result = coordinator->run_portfolio(
//     portfolio, config.strategy_config.symbols, config.strategy_config.start_date,
//     config.strategy_config.end_date, config.strategy_config.asset_class,
//     config.strategy_config.data_freq);

}  // namespace trade_ngin::api
