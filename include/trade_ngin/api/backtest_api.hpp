#pragma once

#include "trade_ngin/backtest/backtest_types.hpp"
#include "trade_ngin/core/error.hpp"

namespace trade_ngin {
namespace api {

Result<backtest::BacktestResults> run_backtest();

}  // namespace api
}  // namespace trade_ngin
