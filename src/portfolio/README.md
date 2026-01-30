# Portfolio Module

## Overview

The portfolio module (`src/portfolio/`) coordinates multiple trading strategies, handles capital allocation, and integrates optimization and risk management for the entire portfolio.

---

## Architecture

```
portfolio/
└── portfolio_manager.cpp    # Main coordinator

include/trade_ngin/portfolio/
└── portfolio_manager.hpp    # Interface
```

---

## PortfolioManager

The central orchestrator for multi-strategy portfolio management.

### Configuration

```cpp
struct PortfolioConfig {
    double total_capital = 500000.0;
    bool use_optimization = true;
    bool use_risk_management = true;
    
    DynamicOptConfig opt_config;
    RiskConfig risk_config;
    
    std::unordered_map<std::string, double> strategy_allocations;
};
```

### Key Methods

```cpp
// Strategy management
void add_strategy(
    std::shared_ptr<StrategyInterface> strategy,
    double allocation,
    bool enabled_backtest,
    bool enabled_live);

void remove_strategy(const std::string& strategy_id);

// Market data processing
Result<void> on_data(const std::vector<Bar>& data);

// Position optimization
Result<void> optimize_positions(
    const std::vector<std::string>& symbols,
    const std::unordered_map<std::string, double>& current_prices);

// Current state
std::unordered_map<std::string, Position> get_aggregated_positions() const;
std::vector<ExecutionReport> get_all_executions() const;
```

---

## Multi-Strategy Workflow

```mermaid
sequenceDiagram
    participant PM as PortfolioManager
    participant S1 as Strategy1
    participant S2 as Strategy2
    participant Opt as DynamicOptimizer
    participant Risk as RiskManager
    participant TCM as TransactionCostManager

    PM->>S1: on_data(bars)
    PM->>S2: on_data(bars)
    
    S1-->>PM: positions_1
    S2-->>PM: positions_2
    
    PM->>PM: aggregate_positions()
    PM->>TCM: calculate_trading_costs()
    PM->>Opt: optimize(aggregated, costs)
    Opt-->>PM: optimized_positions
    
    PM->>Risk: process_positions()
    Risk-->>PM: risk_result
    
    PM->>PM: apply_risk_multiplier()
    PM->>PM: generate_executions()
```

---

## Strategy Allocation

```json
{
  "portfolio": {
    "strategies": {
      "TREND_FOLLOWING": {
        "default_allocation": 0.7
      },
      "TREND_FOLLOWING_FAST": {
        "default_allocation": 0.3
      }
    }
  }
}
```

Allocations determine capital distribution:
- `TREND_FOLLOWING`: 70% × $500,000 = $350,000
- `TREND_FOLLOWING_FAST`: 30% × $500,000 = $150,000

---

## Position Aggregation

When multiple strategies trade the same symbol:

```cpp
// Aggregate by weighted sum
for (const auto& [strat_id, positions] : strategy_positions_) {
    double allocation = strategy_allocations_[strat_id];
    for (const auto& [symbol, pos] : positions) {
        aggregated[symbol].quantity += pos.quantity * allocation;
    }
}
```

---

## Execution Recording

```cpp
struct ExecutionReport {
    Timestamp timestamp;
    std::string symbol;
    Side side;
    Decimal quantity;
    Decimal price;
    
    // Transaction costs
    Decimal commissions_fees;
    Decimal implicit_price_impact;
    Decimal slippage_market_impact;
    Decimal total_transaction_costs;
    
    // Strategy attribution
    std::string strategy_id;
    std::string portfolio_id;
    std::string run_id;
};
```

---

## Integration Points

1. **BacktestCoordinator**: Uses PortfolioManager for portfolio backtests
2. **LiveTradingCoordinator**: Uses PortfolioManager for live trading
3. **TransactionCostManager**: Calculates costs for optimization
4. **RiskManager**: Applies risk constraints to final positions

---

## Usage Example

```cpp
#include "trade_ngin/portfolio/portfolio_manager.hpp"

// Configure
PortfolioConfig config;
config.total_capital = 500000.0;
config.use_optimization = true;
config.strategy_allocations = {
    {"TREND_FOLLOWING", 0.7},
    {"TREND_FOLLOWING_FAST", 0.3}
};

auto portfolio = std::make_shared<PortfolioManager>(config);

// Add strategies
portfolio->add_strategy(trend_strategy, 0.7, true, true);
portfolio->add_strategy(fast_strategy, 0.3, true, true);

// Initialize
portfolio->initialize();

// Process data
portfolio->on_data(market_bars);

// Get results
auto positions = portfolio->get_aggregated_positions();
auto executions = portfolio->get_all_executions();
```

---

## Testing

```bash
cd build
ctest -R portfolio --output-on-failure
```

---

## References

- [Strategy Development Guide](../strategy/README.md) - Creating strategies
- [Backtest Module](../backtest/README.md) - Testing portfolios
- [Live Trading Module](../live/README.md) - Production usage
- [Optimization Module](../optimization/README.md) - Position optimization
- [Transaction Cost Module](../transaction_cost/README.md) - Cost calculations
