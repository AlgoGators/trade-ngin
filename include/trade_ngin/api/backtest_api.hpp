#pragma once

#include <memory>
#include <unordered_map>

#include "trade_ngin/backtest/backtest_types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

namespace trade_ngin {
namespace api {

struct StrategyContext {
    StrategyConfig base_strategy_config;
    std::shared_ptr<PostgresDatabase> db;
    std::shared_ptr<InstrumentRegistry> registry;
};

using StrategyFactory =
    std::function<std::shared_ptr<BaseStrategy>(const StrategyContext&, const nlohmann::json&)>;

class BacktestRunner {
public:
    void initialize(std::string portfolio_name);

    Result<backtest::BacktestResults> run_backtest();

    Result<void> register_strategy(const std::string& strategy_id, StrategyFactory factory);

private:
    std::unordered_map<std::string, StrategyFactory> registered_strategies_;
    std::string portfolio_name_;
};

}  // namespace api
}  // namespace trade_ngin
