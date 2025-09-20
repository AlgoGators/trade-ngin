# Strategy Creation Tutorial: Building Your Own Strategy

## Table of Contents
1. [Overview](#overview)
2. [Understanding the Framework Structure](#understanding-the-framework-structure)
3. [Core System Components](#core-system-components)
4. [Creating Your Strategy Header](#creating-your-strategy-header)
5. [Implementing Your Strategy Logic](#implementing-your-strategy-logic)
6. [Building and Testing](#building-and-testing)
7. [Complete File Structure](#complete-file-structure)

## Overview

This tutorial shows you how to create your own trading strategy by understanding the Trade Ngin framework structure and implementing the required components. You'll learn what variables to declare, what to inherit from base classes, and how to connect to the existing infrastructure.

## Understanding the Framework Structure

### Key Base Classes You Must Inherit From

#### 1. **StrategyInterface** (`include/trade_ngin/strategy/strategy_interface.hpp`)
**What it provides**: Abstract contract that defines required methods
**What you must implement**:
```cpp
class StrategyInterface {
public:
    virtual Result<void> initialize() = 0;
    virtual Result<void> start() = 0;
    virtual Result<void> stop() = 0;
    virtual Result<void> on_data(const std::vector<Bar>& data) = 0;
    virtual Result<void> on_execution(const ExecutionReport& report) = 0;
    virtual Result<void> on_signal(const std::string& symbol, double signal) = 0;
};
```

#### 2. **BaseStrategy** (`include/trade_ngin/strategy/base_strategy.hpp`)
**What it provides**: Common functionality for all strategies
**What you inherit**:
- State management (INITIALIZED, RUNNING, PAUSED, STOPPED, ERROR)
- Position tracking (`positions_` map)
- Signal persistence (`last_signals_` map)
- Risk limits (`risk_limits_`)
- Database connection (`db_`)
- Logging component registration

**Key member variables you get access to**:
```cpp
protected:
    std::string id_;                    // Strategy identifier
    StrategyConfig config_;             // Strategy configuration
    StrategyMetadata metadata_;         // Strategy metadata
    StrategyMetrics metrics_;           // Performance metrics
    std::atomic<StrategyState> state_;  // Current state
    std::unordered_map<std::string, Position> positions_;  // Current positions
    std::unordered_map<std::string, double> last_signals_; // Last signals
    RiskLimits risk_limits_;            // Risk management
    std::shared_ptr<PostgresDatabase> db_;  // Database interface
    mutable std::mutex mutex_;          // Thread safety
```

#### 3. **Strategy Types** (`include/trade_ngin/strategy/types.hpp`)
**What it provides**: Core data structures you must use
**Key structures you'll work with**:
```cpp
struct StrategyConfig : public ConfigBase {
    double capital_allocation{0.0};                           // Your capital
    double max_leverage{0.0};                                 // Max leverage
    std::unordered_map<std::string, double> position_limits;  // Per-symbol limits
    std::unordered_map<std::string, double> trading_params;   // Strategy params
    std::vector<AssetClass> asset_classes;                    // What you trade
    std::vector<DataFrequency> frequencies;                   // Data frequency
    bool save_executions{false};                              // Persistence flags
    bool save_signals{false};
    bool save_positions{false};
};

struct StrategyMetrics {
    double unrealized_pnl;      // Current P&L
    double realized_pnl;        // Closed P&L
    double sharpe_ratio;        // Risk-adjusted return
    double max_drawdown;        // Worst drawdown
    double win_rate;            // Win percentage
    int total_trades;           // Number of trades
};
```

## Core System Components

### 1. **Instrument System** (`include/trade_ngin/instruments/`)

#### **Base Instrument Class** (`instrument.hpp`)
**Purpose**: Defines the contract for all tradeable instruments
**Key Methods You'll Use**:
- `get_multiplier()` - Contract size multiplier
- `get_tick_size()` - Minimum price increment
- `round_price()` - Round to valid tick size
- `get_notional_value()` - Calculate position value

#### **Futures Instruments** (`futures.hpp`)
**Purpose**: Specialized futures contract implementation
**What You Get**:
- Contract specifications (multiplier, tick size, margin)
- Expiration handling
- Trading hours validation
- Commission calculations

**How to Use in Your Strategy**:
```cpp
// Get contract details from registry
auto instrument = registry_->get_instrument(symbol);
if (instrument) {
    double contract_size = instrument->get_multiplier();
    double tick_size = instrument->get_tick_size();
    double margin = instrument->get_margin_requirement();
}
```

#### **Instrument Registry** (`instrument_registry.hpp`)
**Purpose**: Manages all available instruments
**Key Methods**:
- `initialize()` - Load instruments from database
- `get_instrument()` - Retrieve specific instrument
- `has_instrument()` - Check if instrument exists
- `get_all_instruments()` - Get all available instruments

### 2. **Configuration System** (`include/trade_ngin/core/`)

#### **ConfigBase** (`config_base.hpp`)
**Purpose**: Base class for all configuration types
**What You Get**:
- JSON serialization/deserialization
- File save/load functionality
- Configuration versioning

**How to Use**:
```cpp
struct YourStrategyConfig : public ConfigBase {
    // Your parameters here

    nlohmann::json to_json() const override {
        nlohmann::json j;
        // Serialize your parameters
        return j;
    }

    void from_json(const nlohmann::json& j) override {
        // Deserialize your parameters
    }
};
```

### 3. **Logging System** (`include/trade_ngin/core/logger.hpp`)

#### **Logger Configuration**
**Purpose**: Structured logging for debugging and monitoring
**Key Features**:
- Multiple log levels (TRACE, DEBUG, INFO, WARNING, ERR, FATAL)
- File and console output
- Log rotation and size management
- Component-specific logging

**How to Use in Your Strategy**:
```cpp
// Register your component
Logger::register_component("YourStrategyName");

// Use logging macros
INFO("Strategy initialized successfully");
DEBUG("Processing data for symbol " + symbol);
WARN("Position limit reached for " + symbol);
ERROR("Failed to process data: " + error_message);
```

### 4. **State Management** (`include/trade_ngin/core/state_manager.hpp`)

#### **Component State Tracking**
**Purpose**: Monitor system health and component states
**Key Features**:
- Component state tracking (INITIALIZED, RUNNING, PAUSED, ERR_STATE, STOPPED)
- Health monitoring
- Metrics collection
- Error reporting

**How to Use**:
```cpp
// Register your strategy component
auto& state_manager = StateManager::instance();
ComponentInfo info;
info.type = ComponentType::STRATEGY;
info.id = "YourStrategyName";
info.state = ComponentState::INITIALIZED;
state_manager.register_component(info);

// Update state as your strategy runs
state_manager.update_state("YourStrategyName", ComponentState::RUNNING);
```

### 5. **Data Handling System** (`include/trade_ngin/data/`)

#### **Database Interface** (`database_interface.hpp`)
**Purpose**: Abstract database operations
**Key Methods**:
- `get_market_data()` - Retrieve historical data
- `store_signals()` - Save strategy signals
- `store_positions()` - Save position data
- `get_symbols()` - Get available symbols

#### **Database Pooling** (`database_pooling.hpp`)
**Purpose**: Manage multiple database connections
**Key Features**:
- Connection pooling for performance
- Automatic retry with exponential backoff
- Connection lifecycle management

**How to Use**:
```cpp
// Get connection from pool
auto db_guard = DatabasePool::instance().acquire_connection();
auto db = db_guard.get();

// Use database connection
auto symbols_result = db->get_symbols(AssetClass::FUTURES);
```

#### **Credential Store** (`credential_store.hpp`)
**Purpose**: Secure credential management
**Key Features**:
- Encrypted credential storage
- Environment variable support
- Secure access to database credentials

### 6. **Execution System** (`include/trade_ngin/execution/`)

#### **Execution Engine** (`execution_engine.hpp`)
**Purpose**: Handle order execution and management
**Key Features**:
- Multiple execution algorithms (TWAP, VWAP, IS, POV)
- Market impact modeling
- Execution cost analysis
- Venue routing

**How to Use**:
```cpp
// Configure execution
ExecutionConfig exec_config;
exec_config.max_participation_rate = 0.3;
exec_config.urgency_level = 0.5;
exec_config.time_horizon = std::chrono::minutes(60);

// Create execution engine
auto exec_engine = std::make_unique<ExecutionEngine>(exec_config);
```

### 7. **Optimization System** (`include/trade_ngin/optimization/`)

#### **Dynamic Optimizer** (`dynamic_optimizer.hpp`)
**Purpose**: Optimize portfolio positions
**Key Features**:
- Risk-return optimization
- Transaction cost consideration
- Position buffering
- Convergence control

**How to Use**:
```cpp
// Configure optimization
DynamicOptConfig opt_config;
opt_config.tau = 1.0;                    // Risk aversion
opt_config.capital = 1000000.0;          // Available capital
opt_config.cost_penalty_scalar = 50.0;   // Cost penalty weight
opt_config.use_buffering = true;         // Enable position buffering

// Create optimizer
auto optimizer = std::make_unique<DynamicOptimizer>(opt_config);
```

### 8. **Risk Management** (`include/trade_ngin/risk/`)

#### **Risk Manager** (`risk_manager.hpp`)
**Purpose**: Monitor and control portfolio risk
**Key Features**:
- Value at Risk (VaR) calculation
- Jump risk detection
- Correlation monitoring
- Leverage limits

**How to Use**:
```cpp
// Configure risk management
RiskConfig risk_config;
risk_config.var_limit = 0.15;           // 15% VaR limit
risk_config.max_correlation = 0.7;      // Max correlation
risk_config.max_gross_leverage = 4.0;   // Max leverage
risk_config.confidence_level = 0.99;    // Risk confidence

// Create risk manager
auto risk_manager = std::make_unique<RiskManager>(risk_config);
```

### 9. **Portfolio Management** (`include/trade_ngin/portfolio/`)

#### **Portfolio Manager** (`portfolio_manager.hpp`)
**Purpose**: Manage multiple strategies and allocations
**Key Features**:
- Strategy allocation management
- Risk aggregation
- Performance monitoring
- Capital allocation

**How to Use**:
```cpp
// Configure portfolio
PortfolioConfig portfolio_config;
portfolio_config.total_capital = 1000000.0;
portfolio_config.use_optimization = true;
portfolio_config.use_risk_management = true;
portfolio_config.opt_config = opt_config;
portfolio_config.risk_config = risk_config;

// Create portfolio manager
auto portfolio = std::make_unique<PortfolioManager>(portfolio_config);

// Add your strategy
portfolio->add_strategy(your_strategy, 1.0, true, true);
```

### 10. **Backtest System** (`include/trade_ngin/backtest/`)

#### **Slippage Models** (`slippage_models.hpp`)
**Purpose**: Model realistic trading costs
**Key Models**:
- Volume-based slippage
- Spread-based slippage
- Market impact modeling

**How to Use**:
```cpp
// Configure slippage model
VolumeSlippageConfig slippage_config;
slippage_config.price_impact_coefficient = 1e-6;
slippage_config.volatility_multiplier = 1.5;

// Create slippage model
auto slippage_model = std::make_unique<VolumeSlippageModel>(slippage_config);
```

## Creating Your Strategy Header

### 1. **Create Your Header File** (`include/trade_ngin/strategy/your_strategy.hpp`)

#### **Required Includes** (copy these exactly):
```cpp
#pragma once

#include <memory>
#include <utility>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"
```

#### **Your Configuration Structure** (create this for your strategy):
```cpp
struct YourStrategyConfig {
    // Position sizing parameters
    double weight{1.0};                 // Position weight
    double risk_target{0.2};            // Annualized risk target (20%)
    double fx_rate{1.0};                // FX conversion rate

    // Strategy-specific parameters (add what you need)
    int lookback_period{20};            // Your lookback period
    double threshold{0.5};              // Your signal threshold
    bool use_volatility_scaling{true};  // Your volatility option

    // Add more parameters as needed for your strategy
};
```

#### **Your Instrument Data Structure** (what you store per instrument):
```cpp
struct YourInstrumentData {
    // Static properties (from instrument registry)
    double contract_size = 1.0;
    double weight = 1.0;

    // Your strategy's data storage
    std::vector<double> price_history;      // Price data
    std::vector<double> your_indicator;     // Your calculated indicator
    double current_signal = 0.0;            // Current signal value

    // Position data
    double raw_position = 0.0;              // Calculated position
    double final_position = 0.0;            // After risk adjustments

    // Market data
    double current_volatility = 0.01;       // Current volatility
    Timestamp last_update;                  // Last update time
};
```

#### **Your Strategy Class Declaration**:
```cpp
class YourStrategyName : public BaseStrategy {
public:
    // Constructor - you MUST have this signature
    YourStrategyName(std::string id, StrategyConfig config,
                    YourStrategyConfig your_config,
                    std::shared_ptr<PostgresDatabase> db,
                    std::shared_ptr<InstrumentRegistry> registry = nullptr);

    // You MUST override these methods
    Result<void> on_data(const std::vector<Bar>& data) override;
    Result<void> initialize() override;

    // Public accessors for your data
    double get_signal(const std::string& symbol) const;
    double get_position(const std::string& symbol) const;
    const YourInstrumentData* get_instrument_data(const std::string& symbol) const;

protected:
    // You MUST override this for validation
    Result<void> validate_config() const override;

private:
    // Your strategy's configuration
    YourStrategyConfig your_config_;

    // External components you'll use
    std::shared_ptr<InstrumentRegistry> registry_;

    // Your data storage
    std::unordered_map<std::string, YourInstrumentData> instrument_data_;

    // Your calculation methods (declare these)
    std::vector<double> calculate_your_indicator(const std::vector<double>& prices) const;
    double calculate_your_signal(const std::vector<double>& indicator) const;
    double calculate_position(const std::string& symbol, double signal,
                            double price, double volatility) const;
    double apply_risk_controls(const std::string& symbol, double position) const;
};
```

## Implementing Your Strategy Logic

### 1. **Create Your Implementation File** (`src/strategy/your_strategy.cpp`)

#### **Required Includes**:
```cpp
#include "trade_ngin/strategy/your_strategy.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
```

#### **Constructor Implementation** (copy this pattern exactly):
```cpp
YourStrategyName::YourStrategyName(std::string id, StrategyConfig config,
                                   YourStrategyConfig your_config,
                                   std::shared_ptr<PostgresDatabase> db,
                                   std::shared_ptr<InstrumentRegistry> registry)
    : BaseStrategy(std::move(id), std::move(config), std::move(db)),
      your_config_(std::move(your_config)),
      registry_(registry) {

    // Register your logging component
    Logger::register_component("YourStrategyName");

    // Set your metadata
    metadata_.name = "Your Strategy Name";
    metadata_.description = "Description of what your strategy does";
}
```

#### **Configuration Validation** (you MUST implement this):
```cpp
Result<void> YourStrategyName::validate_config() const {
    // Always call base validation first
    auto result = BaseStrategy::validate_config();
    if (result.is_error())
        return result;

    // Validate your specific parameters
    if (your_config_.risk_target <= 0.0 || your_config_.risk_target > 1.0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                               "Risk target must be between 0 and 1",
                               "YourStrategyName");
    }

    if (your_config_.lookback_period <= 0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                               "Lookback period must be positive",
                               "YourStrategyName");
    }

    return Result<void>();
}
```

#### **Initialization Method** (you MUST implement this):
```cpp
Result<void> YourStrategyName::initialize() {
    // Always call base initialization first
    auto base_result = BaseStrategy::initialize();
    if (base_result.is_error()) {
        std::cerr << "Base strategy initialization failed: "
                  << base_result.error()->what() << std::endl;
        return base_result;
    }

    try {
        // Initialize your data containers for each symbol
        for (const auto& [symbol, _] : config_.trading_params) {
            // Reserve memory for your data
            instrument_data_[symbol].price_history.reserve(1000);

            // Initialize positions (you get this from base class)
            Position pos;
            pos.symbol = symbol;
            pos.quantity = 0.0;
            pos.average_price = 1.0;
            pos.last_update = std::chrono::system_clock::now();
            positions_[symbol] = pos;  // This updates the base class positions
        }

        return Result<void>();

    } catch (const std::exception& e) {
        std::cerr << "Error in YourStrategyName::initialize: " << e.what() << std::endl;
        return make_error<void>(ErrorCode::STRATEGY_ERROR,
                               std::string("Failed to initialize your strategy: ") + e.what(),
                               "YourStrategyName");
    }
}
```

#### **Data Processing Method** (you MUST implement this):
```cpp
Result<void> YourStrategyName::on_data(const std::vector<Bar>& data) {
    // Validate data
    if (data.empty()) {
        return Result<void>();
    }

    // Always call base class data processing first
    auto base_result = BaseStrategy::on_data(data);
    if (base_result.is_error())
        return base_result;

    try {
        // Group data by symbol
        std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
        for (const auto& bar : data) {
            // Validate bar data
            if (bar.symbol.empty() || bar.timestamp == Timestamp{} ||
                bar.close <= 0.0) {
                return make_error<void>(ErrorCode::INVALID_DATA,
                                      "Invalid bar data",
                                      "YourStrategyName");
            }
            bars_by_symbol[bar.symbol].push_back(bar);
        }

        // Process each symbol
        for (const auto& [symbol, symbol_bars] : bars_by_symbol) {
            auto& instrument_data = instrument_data_[symbol];

            // Update price history
            for (const auto& bar : symbol_bars) {
                instrument_data.price_history.push_back(static_cast<double>(bar.close));
            }

            // Wait for enough data
            if (instrument_data.price_history.size() < your_config_.lookback_period) {
                continue;
            }

            // Calculate your indicator
            auto indicator = calculate_your_indicator(instrument_data.price_history);
            instrument_data.your_indicator = indicator;

            // Calculate your signal
            double signal = calculate_your_signal(indicator);
            instrument_data.current_signal = signal;

            // Calculate position
            double price = instrument_data.price_history.back();
            double volatility = 0.01; // You'll implement volatility calculation
            double raw_position = calculate_position(symbol, signal, price, volatility);
            instrument_data.raw_position = raw_position;

            // Apply risk controls
            double final_position = apply_risk_controls(symbol, raw_position);
            instrument_data.final_position = final_position;

            // Update position in base class
            Position pos;
            pos.symbol = symbol;
            pos.quantity = final_position;
            pos.average_price = price;
            pos.last_update = symbol_bars.back().timestamp;

            auto pos_result = update_position(symbol, pos);
            if (pos_result.is_error()) {
                WARN("Failed to update position for " + symbol);
            }

            // Save signal
            auto signal_result = on_signal(symbol, signal);
            if (signal_result.is_error()) {
                WARN("Failed to save signal for " + symbol);
            }

            instrument_data.last_update = symbol_bars.back().timestamp;
        }

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Error processing data in YourStrategyName: " + std::string(e.what()));
        return make_error<void>(ErrorCode::STRATEGY_ERROR,
                               std::string("Error processing data: ") + e.what(),
                               "YourStrategyName");
    }
}
```

#### **Your Calculation Methods** (implement these for your strategy):
```cpp
std::vector<double> YourStrategyName::calculate_your_indicator(
    const std::vector<double>& prices) const {

    std::vector<double> indicator(prices.size(), 0.0);

    // Implement your indicator calculation here
    // Example: Simple moving average
    for (size_t i = your_config_.lookback_period - 1; i < prices.size(); ++i) {
        double sum = 0.0;
        for (size_t j = 0; j < your_config_.lookback_period; ++j) {
            sum += prices[i - j];
        }
        indicator[i] = sum / your_config_.lookback_period;
    }

    return indicator;
}

double YourStrategyName::calculate_your_signal(
    const std::vector<double>& indicator) const {

    if (indicator.empty()) return 0.0;

    // Implement your signal generation here
    // Example: Compare current to previous value
    double current = indicator.back();
    double previous = indicator[indicator.size() - 2];

    if (current > previous + your_config_.threshold) return 1.0;   // Buy signal
    if (current < previous - your_config_.threshold) return -1.0;  // Sell signal
    return 0.0;  // No signal
}

double YourStrategyName::calculate_position(const std::string& symbol,
                                          double signal, double price,
                                          double volatility) const {

    // Get instrument data
    auto it = instrument_data_.find(symbol);
    if (it == instrument_data_.end()) return 0.0;

    const auto& data = it->second;

    // Use parameters from base class config
    double capital = config_.capital_allocation;
    double weight = data.weight;
    double contract_size = data.contract_size;

    // Your position sizing formula
    double position = (signal * capital * weight) / (price * contract_size * volatility);

    // Apply position limits from base class config
    double position_limit = 1000.0;
    if (config_.position_limits.count(symbol) > 0) {
        position_limit = config_.position_limits.at(symbol);
    }

    return std::clamp(position, -position_limit, position_limit);
}

double YourStrategyName::apply_risk_controls(const std::string& symbol,
                                            double position) const {

    // Get current position from base class
    double current_position = 0.0;
    auto pos_it = positions_.find(symbol);
    if (pos_it != positions_.end()) {
        current_position = static_cast<double>(pos_it->second.quantity);
    }

    // Implement your risk controls here
    // Example: Position buffering
    double buffer = 0.1 * std::abs(position);
    double lower_bound = position - buffer;
    double upper_bound = position + buffer;

    if (current_position < lower_bound) return std::round(lower_bound);
    if (current_position > upper_bound) return std::round(upper_bound);
    return std::round(current_position);
}
```

#### **Public Accessor Methods**:
```cpp
double YourStrategyName::get_signal(const std::string& symbol) const {
    auto it = instrument_data_.find(symbol);
    if (it != instrument_data_.end()) {
        return it->second.current_signal;
    }
    return 0.0;
}

double YourStrategyName::get_position(const std::string& symbol) const {
    auto it = instrument_data_.find(symbol);
    if (it != instrument_data_.end()) {
        return it->second.final_position;
    }
    return 0.0;
}

const YourInstrumentData* YourStrategyName::get_instrument_data(
    const std::string& symbol) const {
    auto it = instrument_data_.find(symbol);
    if (it != instrument_data_.end()) {
        return &it->second;
    }
    return nullptr;
}
```

## Building and Testing

### 1. **Update Build Configuration**

#### **Add to `src/CMakeLists.txt`**:
```cmake
# Add your strategy source file
target_sources(trade_ngin_core
    PRIVATE
        strategy/your_strategy.cpp
)
```

#### **Add to `apps/CMakeLists.txt`**:
```cmake
# Add your backtest application
add_executable(bt_your_strategy
    backtest/bt_your_strategy.cpp
)

target_link_libraries(bt_your_strategy
    trade_ngin_core
    trade_ngin_backtest
)
```

### 2. **Build Commands** (use these exact commands):
```bash
# From project root
mkdir -p build
cd build
cmake ..
cmake --build . --config Release
cd ..
```

### 3. **Run Your Strategy**:
```bash
# Run backtest
./build/bin/Release/bt_your_strategy

# Run live (when you create it)
./build/bin/Release/live_your_strategy
```

## Complete File Structure

### Files You Must Create:
```
include/trade_ngin/strategy/
├── your_strategy.hpp              # Your strategy header

src/strategy/
├── your_strategy.cpp              # Your strategy implementation

apps/
├── backtest/
│   └── bt_your_strategy.cpp      # Your backtest application
└── strategies/
    └── live_your_strategy.cpp    # Your live trading application
```

### Files You Must Update:
```
CMakeLists.txt                     # Add your source files
src/CMakeLists.txt                # Include your strategy
apps/CMakeLists.txt               # Add your applications
```

### Key Files to Reference (Don't Modify):
```
include/trade_ngin/strategy/
├── strategy_interface.hpp         # Required interface
├── base_strategy.hpp             # Base functionality
└── types.hpp                     # Data structures

src/strategy/
└── base_strategy.cpp             # Base implementation

include/trade_ngin/
├── core/                         # Core utilities
├── data/                         # Database interface
├── instruments/                  # Instrument management
└── portfolio/                    # Portfolio management
```

## Key Points to Remember

### 1. **You MUST Inherit from BaseStrategy**
- This gives you all the common functionality
- You get position tracking, state management, database access
- You get logging, error handling, and risk management

### 2. **You MUST Override These Methods**
- `initialize()` - Set up your data structures
- `on_data()` - Process incoming market data
- `validate_config()` - Validate your parameters

### 3. **Use Base Class Member Variables**
- `config_` - Your strategy configuration
- `positions_` - Current positions (from base class)
- `db_` - Database interface
- `metadata_` - Strategy metadata

### 4. **Follow the Error Handling Pattern**
- Always use `Result<T>` return types
- Call base class methods first
- Handle errors gracefully with proper logging

### 5. **Reference Existing Components**
- Use `InstrumentRegistry` for contract details
- Use `PostgresDatabase` for data access
- Use `Logger` for debugging and monitoring

### 6. **Understand the System Architecture**
- **Instruments**: Define what you trade (futures, equities, options)
- **Configuration**: Manage parameters and settings
- **Logging**: Track execution and debug issues
- **State Management**: Monitor system health
- **Data Handling**: Access market data and store results
- **Execution**: Manage order execution and costs
- **Optimization**: Optimize portfolio positions
- **Risk Management**: Control portfolio risk
- **Portfolio Management**: Coordinate multiple strategies

By following this structure and implementing the required methods, you'll have a fully functional strategy that integrates seamlessly with the Trade Ngin framework and leverages all the sophisticated components for robust, production-ready trading.
