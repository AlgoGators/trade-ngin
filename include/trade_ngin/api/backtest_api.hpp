#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "trade_ngin/backtest/backtest_types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

namespace trade_ngin {
namespace api {

class BacktestRunner {
public:
    Result<backtest::BacktestResults> run_backtest();

    // TODO register config?
    // Result<void> register_strategy(const std::string& strategy_id,
    //                                std::shared_ptr<BaseStrategy> strategy);
    Result<void> register_strategy(const std::string& strategy_id, const StrategyConfig& config,
                                   std::shared_ptr<PostgresDatabase> db);

private:
    std::vector<std::shared_ptr<BaseStrategy>> get_registered_strategies() const;
    std::unordered_map<std::string, std::shared_ptr<BaseStrategy>> registered_strategies_;
};

}  // namespace api
}  // namespace trade_ngin
