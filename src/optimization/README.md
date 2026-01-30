# Optimization & Risk Module

## Overview

The optimization module (`src/optimization/`) and risk module (`src/risk/`) provide portfolio-level position optimization with transaction cost consideration and comprehensive risk management.

---

## Architecture

```
optimization/
└── dynamic_optimizer.cpp    # Position optimizer

risk/
└── risk_manager.cpp         # Risk management
```

---

## Dynamic Optimizer

**File**: `src/optimization/dynamic_optimizer.cpp`

Optimizes portfolio positions to minimize tracking error while considering transaction costs.

### Configuration

```cpp
struct DynamicOptConfig {
    double tau = 1.0;                  // Risk aversion parameter
    double capital = 500000.0;         // Available capital
    double cost_penalty_scalar = 50.0; // Weight on transaction costs
    bool use_buffering = true;         // Enable position buffering
    int convergence_iterations = 10;   // Max optimization iterations
};
```

### Key Methods

```cpp
// Main optimization function
Result<OptimizationResult> optimize(
    const std::vector<double>& current_positions,
    const std::vector<double>& target_positions,
    const std::vector<double>& costs,                    // Per-symbol costs
    const std::vector<double>& weights_per_contract,     // Contract weights
    const std::vector<std::vector<double>>& covariance   // Covariance matrix
);

// Single period optimization
Result<OptimizationResult> optimize_single_period(
    const std::vector<double>& current_positions,
    const std::vector<double>& target_positions,
    const std::vector<double>& costs,
    const std::vector<double>& weights_per_contract,
    const std::vector<std::vector<double>>& covariance
);

// Apply buffering to reduce trading
Result<OptimizationResult> apply_buffering(
    const std::vector<double>& current_positions,
    const std::vector<double>& optimized_positions,
    const std::vector<double>& target_positions,
    const std::vector<double>& costs,
    const std::vector<double>& weights_per_contract,
    const std::vector<std::vector<double>>& covariance
);
```

### OptimizationResult Structure

```cpp
struct OptimizationResult {
    std::vector<double> optimized_positions;
    double tracking_error;
    double cost_penalty;
    double total_cost;
    bool converged;
    int iterations;
};
```

### Optimization Logic

The optimizer minimizes:
```
Objective = τ × TrackingError + CostPenalty
```

Where:
- `TrackingError = √((target - proposed)ᵀ × Σ × (target - proposed))`
- `CostPenalty = cost_penalty_scalar × Σ(|position_change| × cost)`

---

## Risk Manager

**File**: `src/risk/risk_manager.cpp`

Provides portfolio-level risk constraints and monitoring.

### Configuration

```cpp
struct RiskConfig {
    double var_limit = 0.15;            // 15% VaR limit
    double max_correlation = 0.7;       // Max correlation threshold
    double max_gross_leverage = 4.0;    // Max total exposure
    double max_net_leverage = 2.0;      // Max directional exposure
    double confidence_level = 0.99;     // VaR confidence
    int lookback_period = 252;          // Days for calculations
};
```

### Key Methods

```cpp
// Main risk processing
Result<RiskResult> process_positions(
    const std::unordered_map<std::string, Position>& positions,
    const MarketData& market_data,
    const std::unordered_map<std::string, double>& current_prices);

// Calculate portfolio weights
std::vector<double> calculate_weights(
    const std::unordered_map<std::string, Position>& positions);

// Risk multiplier calculations
double calculate_portfolio_multiplier(
    const MarketData& market_data,
    const std::vector<double>& weights,
    RiskResult& result);

double calculate_jump_multiplier(
    const MarketData& market_data,
    const std::vector<double>& weights,
    RiskResult& result);

double calculate_correlation_multiplier(
    const MarketData& market_data,
    const std::vector<double>& weights,
    RiskResult& result);

double calculate_leverage_multiplier(
    const MarketData& market_data,
    const std::vector<double>& weights,
    const std::vector<double>& position_values,
    double total_value,
    RiskResult& result);

// VaR calculation
double calculate_var(
    const std::unordered_map<std::string, Position>& positions,
    const std::vector<std::vector<double>>& returns);
```

### RiskResult Structure

```cpp
struct RiskResult {
    double portfolio_multiplier = 1.0;    // Overall scaling
    double jump_multiplier = 1.0;         // Jump risk adjustment
    double correlation_multiplier = 1.0;  // Correlation adjustment
    double leverage_multiplier = 1.0;     // Leverage constraint
    double combined_multiplier = 1.0;     // Product of all multipliers
    
    double gross_leverage = 0.0;
    double net_leverage = 0.0;
    double var_estimate = 0.0;
    
    bool var_limit_breached = false;
    bool leverage_limit_breached = false;
};
```

### Risk Multiplier Calculation

```
final_position = target_position × combined_multiplier

combined_multiplier = min(
    portfolio_multiplier,
    jump_multiplier,
    correlation_multiplier,
    leverage_multiplier
)
```

Each multiplier is in range [0, 1]:
- `1.0` = No constraint active
- `< 1.0` = Risk limit approaching, scale down
- `0.0` = Risk limit breached, exit entirely

---

## Integration with Portfolio Manager

```cpp
// In PortfolioManager::optimize_positions()

// Get trading costs
costs = calculate_trading_costs(symbols, capital);

// Run optimization
auto opt_result = optimizer_->optimize(
    current_positions,
    target_positions,
    costs,
    weights_per_contract,
    covariance
);

// Apply risk constraints
auto risk_result = risk_manager_->process_positions(
    positions,
    market_data,
    current_prices
);

// Scale positions by risk multiplier
for (auto& pos : optimized_positions) {
    pos *= risk_result.combined_multiplier;
}
```

---

## Usage Example

```cpp
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include "trade_ngin/risk/risk_manager.hpp"

// Configure optimizer
DynamicOptConfig opt_config;
opt_config.tau = 1.0;
opt_config.capital = 500000.0;
opt_config.cost_penalty_scalar = 50.0;
opt_config.use_buffering = true;

auto optimizer = std::make_unique<DynamicOptimizer>(opt_config);

// Configure risk manager
RiskConfig risk_config;
risk_config.var_limit = 0.15;
risk_config.max_gross_leverage = 4.0;

auto risk_manager = std::make_unique<RiskManager>(risk_config);

// Optimize
std::vector<double> current = {10, 5, -3};
std::vector<double> target = {15, 8, -5};
std::vector<double> costs = {0.001, 0.002, 0.0015};
std::vector<double> weights = {0.4, 0.35, 0.25};
std::vector<std::vector<double>> cov = {...};

auto result = optimizer->optimize(current, target, costs, weights, cov);

if (result.is_ok()) {
    auto optimized = result.value().optimized_positions;
    // Apply to portfolio...
}
```

---

## Key Formulas

### Tracking Error

```
TE = √((Δw)ᵀ × Σ × (Δw))

Where:
Δw = target_weights - proposed_weights
Σ = covariance matrix
```

### Cost Penalty

```
CP = scalar × Σ(|Δq_i| × cost_i)

Where:
Δq_i = change in position for symbol i
cost_i = transaction cost rate for symbol i
```

### VaR (Historical Simulation)

```
VaR_99 = -percentile(portfolio_returns, 1)
```

### Leverage

```
gross_leverage = Σ|notional_i| / capital
net_leverage = |Σ(notional_i)| / capital
```

---

## Testing

```bash
cd build
ctest -R optimization --output-on-failure
ctest -R risk --output-on-failure
```

---

## References

- [Strategy Development Guide](../strategy/README.md) - Creating strategies
- [Portfolio Module](../portfolio/README.md) - Multi-strategy coordination
- [Transaction Cost Module](../transaction_cost/README.md) - Cost calculations
- [Backtest Module](../backtest/README.md) - Historical testing
