# Equity Strategy Guide

How to create, integrate, configure, and backtest an equities strategy in trade-ngin.

---

## Architecture Overview

Trade-ngin uses a layered strategy architecture. Every strategy — futures or equities — plugs into the same framework:

```
StrategyInterface          (pure virtual contract)
    └── BaseStrategy       (state machine, positions, PnL, risk)
            └── YourStrategy   (signal logic, custom config)
```

The backtest pipeline flows:

```
Config ─> DataLoader ─> Strategy.on_data() ─> Positions ─> ExecutionManager ─> PnLManager ─> MetricsCalculator ─> DB Storage
```

All orchestration is handled by `BacktestCoordinator`. Your strategy only needs to implement `on_data()` and optionally override `initialize()`.

---

## Step 1: Define Your Strategy Config Struct

Create a config struct that holds all tunable parameters. This keeps strategy logic decoupled from hardcoded values.

**File:** `include/trade_ngin/strategy/your_strategy.hpp`

```cpp
struct YourStrategyConfig {
    int lookback_period{20};
    double entry_threshold{2.0};
    double exit_threshold{0.5};
    double risk_target{0.15};
    double position_size{0.1};        // Fraction of capital per position
    int vol_lookback{20};
    bool use_stop_loss{true};
    double stop_loss_pct{0.05};
    bool allow_fractional_shares{true};
};
```

Reference: `include/trade_ngin/strategy/mean_reversion.hpp` — `MeanReversionConfig`

---

## Step 2: Create Your Strategy Class

Extend `BaseStrategy` and implement your signal logic.

**Header:** `include/trade_ngin/strategy/your_strategy.hpp`

```cpp
#pragma once
#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"

namespace trade_ngin {

struct YourInstrumentData {
    std::vector<double> price_history;
    double current_price = 0.0;
    double target_position = 0.0;
    double entry_price = 0.0;
    double current_volatility = 0.01;
    Timestamp last_update;
    // ... add your indicator fields
};

class YourStrategy : public BaseStrategy {
public:
    YourStrategy(std::string id, StrategyConfig config,
                 YourStrategyConfig custom_config,
                 std::shared_ptr<PostgresDatabase> db,
                 std::shared_ptr<InstrumentRegistry> registry = nullptr);

    Result<void> initialize() override;
    Result<void> on_data(const std::vector<Bar>& data) override;

    std::unordered_map<std::string, std::vector<double>>
        get_price_history() const override;

protected:
    Result<void> validate_config() const override;

private:
    YourStrategyConfig custom_config_;
    std::shared_ptr<InstrumentRegistry> registry_;
    std::unordered_map<std::string, YourInstrumentData> instrument_data_;

    // Your indicator/signal methods
    double generate_signal(const std::string& symbol,
                           const YourInstrumentData& data) const;
    double calculate_position_size(const std::string& symbol,
                                   double price, double volatility) const;
};

}  // namespace trade_ngin
```

**Implementation:** `src/strategy/your_strategy.cpp`

Key methods to implement:

### initialize()

```cpp
Result<void> YourStrategy::initialize() {
    // 1. Call base class init
    auto base_result = BaseStrategy::initialize();
    if (base_result.is_error()) return base_result;

    // 2. Set PnL accounting (equities use UNREALIZED_ONLY)
    set_pnl_accounting_method(PnLAccountingMethod::UNREALIZED_ONLY);

    // 3. Initialize per-symbol data from config_.trading_params
    for (const auto& [symbol, _] : config_.trading_params) {
        YourInstrumentData data;
        data.price_history.reserve(custom_config_.lookback_period * 2);
        instrument_data_[symbol] = data;

        Position pos;
        pos.symbol = symbol;
        pos.quantity = 0.0;
        pos.average_price = 0.0;
        positions_[symbol] = pos;
    }

    return Result<void>();
}
```

### on_data()

This is called once per bar batch (all symbols for one timestamp). The framework passes a `std::vector<Bar>` where each `Bar` has:

| Field       | Type       | Description              |
|-------------|------------|--------------------------|
| `symbol`    | `string`   | Ticker (e.g. "AAPL")    |
| `open`      | `Decimal`  | Open price               |
| `high`      | `Decimal`  | High price               |
| `low`       | `Decimal`  | Low price                |
| `close`     | `Decimal`  | Close price              |
| `volume`    | `Quantity` | Volume                   |
| `timestamp` | `Timestamp`| Bar timestamp            |

```cpp
Result<void> YourStrategy::on_data(const std::vector<Bar>& data) {
    if (state_ != StrategyState::RUNNING)
        return make_error<void>(ErrorCode::STRATEGY_ERROR, "Not running", "YourStrategy");

    for (const auto& bar : data) {
        auto& inst = instrument_data_[bar.symbol];

        // 1. Update price history
        inst.price_history.push_back(bar.close.as_double());
        inst.current_price = bar.close.as_double();

        // 2. Trim history to prevent memory growth
        //    Keep at most lookback * 2 entries
        while (inst.price_history.size() > lookback * 2)
            inst.price_history.erase(inst.price_history.begin());

        // 3. Skip until enough data
        if (inst.price_history.size() < lookback) continue;

        // 4. Calculate your indicators
        // ... your logic here ...

        // 5. Generate signal (-1.0 short, 0.0 flat, +1.0 long)
        double signal = generate_signal(bar.symbol, inst);

        // 6. Size the position
        double size = calculate_position_size(bar.symbol,
            bar.close.as_double(), inst.current_volatility);
        inst.target_position = signal * size;

        // 7. Track entry price (for stop loss / PnL)
        if (std::abs(positions_[bar.symbol].quantity.as_double()) < 1e-8)
            inst.entry_price = bar.close.as_double();

        // 8. Update position in base class
        Position pos = positions_[bar.symbol];
        pos.quantity = Quantity(inst.target_position);
        pos.average_price = bar.close;
        pos.last_update = bar.timestamp;
        positions_[bar.symbol] = pos;
    }
    return Result<void>();
}
```

### PnL Accounting Methods

| Method            | Use Case | Behavior |
|-------------------|----------|----------|
| `UNREALIZED_ONLY` | Equities | Positions held, mark-to-market daily |
| `REALIZED_ONLY`   | Futures  | Marked-to-market, daily settlement |
| `MIXED`           | Complex  | Both realized and unrealized tracked |

For equities, always use `UNREALIZED_ONLY` — set this in `initialize()`.

---

## Step 3: Register in CMake

### Library Sources

Add your `.cpp` to the main library in the root `CMakeLists.txt` or `src/CMakeLists.txt` (wherever strategy sources are listed):

```cmake
set(STRATEGY_SOURCES
    src/strategy/base_strategy.cpp
    src/strategy/mean_reversion.cpp
    src/strategy/your_strategy.cpp       # <-- add
)
```

### Backtest Executable

Add a backtest app in `apps/backtest/CMakeLists.txt`:

```cmake
set(BT_YOUR_STRATEGY_SOURCES
    bt_your_strategy.cpp
)

add_executable(bt_your_strategy ${BT_YOUR_STRATEGY_SOURCES})

target_link_libraries(bt_your_strategy PRIVATE trade_ngin)

set_target_properties(bt_your_strategy
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)
```

---

## Step 4: Create the Configuration

### Portfolio Config

Create a directory under `config/portfolios/<your_strategy_id>/` with a `portfolio.json`:

```
config/
  defaults.json                         # Shared: database, execution, backtest params
  portfolios/
    your_strategy/
      portfolio.json                    # Strategy-specific config
```

**`config/portfolios/your_strategy/portfolio.json`:**

```json
{
  "_description": "YOUR_PORTFOLIO - Description of your strategy",
  "portfolio_id": "YOUR_PORTFOLIO",
  "initial_capital": 100000,
  "reserve_capital_pct": 0.1,
  "strategies": {
    "YOUR_STRATEGY": {
      "enabled_backtest": true,
      "enabled_live": true,
      "default_allocation": 1.0,
      "type": "YourStrategy",
      "symbols": ["AAPL", "MSFT", "AMZN", "GOOGL", "META"],
      "config": {
        "lookback_period": 20,
        "entry_threshold": 2.0,
        "exit_threshold": 0.5,
        "risk_target": 0.15,
        "position_size": 0.1,
        "vol_lookback": 20,
        "use_stop_loss": true,
        "stop_loss_pct": 0.05,
        "allow_fractional_shares": true
      }
    }
  }
}
```

**Key fields:**

| Field                  | Description |
|------------------------|-------------|
| `portfolio_id`         | Unique ID stored in DB with results |
| `initial_capital`      | Starting capital in USD |
| `reserve_capital_pct`  | Fraction held as cash reserve (0.1 = 10%) |
| `symbols`              | Tickers to trade — loaded from config, avoids slow DB scan |
| `enabled_backtest`     | Whether this strategy runs in backtest mode |
| `enabled_live`         | Whether this strategy runs in live mode |
| `default_allocation`   | Weight in portfolio (1.0 = 100%) |
| `config`               | Passed directly to your strategy config struct |

### Shared Defaults (`config/defaults.json`)

These apply to all portfolios unless overridden:

| Section              | Key Fields |
|----------------------|------------|
| `database`           | `host`, `port`, `username`, `password`, `name` |
| `execution`          | `commission_rate`, `slippage_bps`, `position_limit_backtest` |
| `backtest`           | `lookback_years`, `store_trade_details` |
| `strategy_defaults`  | `use_optimization`, `use_risk_management`, `fdm` table |

---

## Step 5: Write the Backtest App

Create `apps/backtest/bt_your_strategy.cpp`. This is the `main()` entry point that wires everything together.

The standard flow (see `bt_equity_mean_reversion.cpp` as reference):

```cpp
int main() {
    // 1. Initialize logger
    Logger::reset_for_tests();
    auto& logger = Logger::instance();
    // ... configure logger ...

    // 2. Load configuration
    auto app_config = ConfigLoader::load("./config", "your_strategy").value();

    // 3. Setup database connection pool
    auto pool = DatabasePool::instance();
    pool.initialize(app_config.database.get_connection_string(),
                    app_config.database.num_connections);
    auto db = pool.acquire_connection().get();

    // 4. Initialize instrument registry + load equity symbols
    auto& registry = InstrumentRegistry::instance();
    registry.initialize(db);
    registry.load_instruments();  // loads futures metadata (optional)

    // 5. Load symbols from config (fast) or DB fallback (slow)
    std::vector<std::string> symbols;
    const auto& strategy_def = app_config.strategies_config["YOUR_STRATEGY"];
    for (const auto& sym : strategy_def["symbols"])
        symbols.push_back(sym.get<std::string>());

    // 6. Register equity instruments
    registry.load_equity_instruments(symbols);

    // 7. Configure backtest dates
    Timestamp start_date = /* now - lookback_years */;
    Timestamp end_date = /* now */;

    // 8. Create BacktestCoordinator
    BacktestCoordinatorConfig coord_config;
    coord_config.initial_capital = app_config.initial_capital;
    coord_config.portfolio_id = app_config.portfolio_id;
    coord_config.store_trade_details = true;
    auto coordinator = std::make_unique<BacktestCoordinator>(db, &registry, coord_config);
    coordinator->initialize();

    // 9. Create your strategy
    YourStrategyConfig custom_config;
    custom_config.lookback_period = strategy_def["config"]["lookback_period"];
    // ... parse remaining fields ...

    StrategyConfig strategy_config;
    strategy_config.asset_classes = {AssetClass::EQUITIES};
    strategy_config.frequencies = {DataFrequency::DAILY};
    strategy_config.capital_allocation = app_config.initial_capital;
    for (const auto& sym : symbols) {
        strategy_config.trading_params[sym] = {};
        strategy_config.position_limits[sym] = app_config.execution.position_limit_backtest;
    }

    auto strategy = std::make_shared<YourStrategy>(
        "YOUR_STRATEGY", strategy_config, custom_config, db, registry_ptr);
    strategy->initialize();
    strategy->start();

    // 10. Create portfolio and run
    PortfolioConfig portfolio_config;
    portfolio_config.total_capital = Decimal(app_config.initial_capital);
    portfolio_config.reserve_capital = Decimal(app_config.initial_capital * 0.1);
    auto portfolio = std::make_shared<PortfolioManager>(portfolio_config);
    portfolio->add_strategy(strategy, 1.0, false, false);

    auto result = coordinator->run_portfolio(
        portfolio, symbols, start_date, end_date,
        AssetClass::EQUITIES, DataFrequency::DAILY);

    // 11. Display + save results
    const auto& r = result.value();
    std::cout << "Total Return: " << r.total_return * 100 << "%" << std::endl;
    std::cout << "Sharpe Ratio: " << r.sharpe_ratio << std::endl;
    std::cout << "Max Drawdown: " << r.max_drawdown * 100 << "%" << std::endl;

    coordinator->save_portfolio_results_to_db(r, ...);
    return 0;
}
```

---

## Step 6: Transaction Costs for Equities

Equity transaction costs differ from futures. The `AssetCostConfigRegistry` handles this automatically when equity instruments are registered, but you should understand the model.

### Equity Cost Model

| Parameter                | Equity Default        | Futures Default       |
|--------------------------|-----------------------|-----------------------|
| `commission_per_unit`    | $0.005/share (IBKR)  | -1.0 (uses global)   |
| `min_commission_per_order` | $1.00               | N/A                   |
| `point_value`            | 1.0 (per share)       | Contract multiplier   |
| `tick_size`              | $0.01 (penny)         | Varies by contract    |
| `baseline_spread_ticks`  | 1.0                   | Varies by contract    |

### Cost Calculation Formula

```
Commissions = max(min_commission, abs(quantity) * commission_per_unit)
Spread Cost = spread_model(baseline_spread_ticks, tick_size, spread_cost_multiplier)
Market Impact = impact_model(order_size, max_impact_bps)
Total Cost = Commissions + (Spread Cost + Market Impact) * point_value * quantity
```

### Registering Custom Costs

In `src/transaction_cost/asset_cost_config.cpp`, the registry auto-populates equity defaults. To override for a specific symbol:

```cpp
AssetCostConfig config;
config.symbol = "AAPL";
config.commission_per_unit = 0.005;
config.min_commission_per_order = 1.0;
config.tick_size = 0.01;
config.point_value = 1.0;
config.baseline_spread_ticks = 1.0;
registry.register_config(config);
```

---

## Step 7: Build and Run

### Build

```bash
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make bt_your_strategy -j$(nproc)
```

### Run

```bash
cd /path/to/trade-ngin    # must be project root (config/ is relative)
./build/bin/bt_your_strategy
```

The executable expects `./config/defaults.json` and `./config/portfolios/<id>/portfolio.json` to exist relative to the working directory.

### Output

Results are printed to stdout and saved to the database:

```
======= Your Strategy Backtest Results =======
Total Return: X.XX%
Sharpe Ratio: X.XXX
Sortino Ratio: X.XXX
Max Drawdown: X.XX%
Calmar Ratio: X.XXX
Volatility: X.XX%
Win Rate: X.XX%
Total Trades: N
===============================================
```

---

## Step 8: Database Results

Backtest results are stored in the `backtest` schema:

| Table                       | Contents |
|-----------------------------|----------|
| `backtest.results`          | Summary metrics (Sharpe, return, drawdown, etc.) |
| `backtest.equity_curve`     | Daily portfolio value, cumulative PnL, drawdown |
| `backtest.executions`       | Every trade: symbol, side, quantity, price, costs |
| `backtest.daily_pnl`        | Per-strategy daily PnL breakdown |

### Querying Results

```sql
-- Latest backtest summary
SELECT * FROM backtest.results
WHERE portfolio_id = 'YOUR_PORTFOLIO'
ORDER BY created_at DESC LIMIT 1;

-- Equity curve
SELECT date, portfolio_value, cumulative_pnl, drawdown
FROM backtest.equity_curve
WHERE backtest_id = '<id>'
ORDER BY date;

-- Execution details
SELECT timestamp, symbol, side, quantity, fill_price,
       commissions_fees, slippage_market_impact
FROM backtest.executions
WHERE backtest_id = '<id>'
ORDER BY timestamp;
```

---

## Step 9: Live Trading App (Optional)

To run your strategy live, create `apps/strategies/live_your_strategy.cpp`. The pattern mirrors the backtest app but uses `LiveTradingCoordinator` instead of `BacktestCoordinator`. See `live_equity_mean_reversion.cpp` as reference.

Register it in `apps/strategies/CMakeLists.txt` the same way.

---

## Checklist for a New Equity Strategy

- [ ] Strategy config struct in header (`include/trade_ngin/strategy/`)
- [ ] Strategy class extending `BaseStrategy` (header + cpp)
- [ ] Set `PnLAccountingMethod::UNREALIZED_ONLY` in `initialize()`
- [ ] Implement `on_data()` with your signal logic
- [ ] Implement `validate_config()` for parameter bounds checking
- [ ] Implement `get_price_history()` override
- [ ] Add `.cpp` to library sources in CMakeLists.txt
- [ ] Create portfolio config directory under `config/portfolios/`
- [ ] Write `portfolio.json` with symbols and strategy params
- [ ] Create backtest app (`apps/backtest/bt_*.cpp`)
- [ ] Register backtest executable in `apps/backtest/CMakeLists.txt`
- [ ] Build and run: `make bt_your_strategy && ./build/bin/bt_your_strategy`
- [ ] Verify results in `backtest.results` and `backtest.equity_curve`
- [ ] (Optional) Create live app in `apps/strategies/`

---

## Reference: Existing Mean Reversion Implementation

| Component | File |
|-----------|------|
| Strategy header | `include/trade_ngin/strategy/mean_reversion.hpp` |
| Strategy impl | `src/strategy/mean_reversion.cpp` |
| Backtest app | `apps/backtest/bt_equity_mean_reversion.cpp` |
| Live app | `apps/strategies/live_equity_mean_reversion.cpp` |
| Portfolio config | `config/portfolios/equity_mr/portfolio.json` |
| Base strategy | `include/trade_ngin/strategy/base_strategy.hpp` |
| Strategy interface | `include/trade_ngin/strategy/strategy_interface.hpp` |
| Transaction costs | `include/trade_ngin/transaction_cost/asset_cost_config.hpp` |
| Cost manager | `src/transaction_cost/transaction_cost_manager.cpp` |
| Config loader | `include/trade_ngin/core/config_loader.hpp` |
| Backtest coordinator | `include/trade_ngin/backtest/backtest_coordinator.hpp` |
| Portfolio manager | `src/portfolio/portfolio_manager.cpp` |
