# trade-ngin

## 📖 Project Overview

trade-ngin is a high-performance, modular quantitative trading system built in C++ designed for professional algorithmic traders and financial institutions. The system supports systematic trading strategies across multiple asset classes with a focus on futures, equities, and options markets.

### Core Capabilities

- **Multi-strategy portfolio management** with dynamic capital allocation
- **Comprehensive risk management** with position limits, drawdown control, and VaR constraints
- **High-performance backtesting engine** with realistic execution simulation
- **Transaction cost analysis** for strategy optimization
- **Modular architecture** designed for extensibility and customization
- **Professional-grade performance** with C++ efficiency

trade-ngin is designed to handle both backtesting and live trading seamlessly, with a unified codebase that maintains consistency between simulation and production environments.

## 📂 Repository Structure & Organization

```
trade_ngin/
├── apps/                       # Application executables
│   ├── backtest/               # Backtesting applications
│   │   ├── bt_trend.cpp        # Trend following strategy backtest
│   │   └── CMakeLists.txt      # Build configuration for backtests
│   └── CMakeLists.txt          # Build configuration for applications
├── include/                    # Public header files
│   └── trade_ngin/             # Main include directory
│       ├── backtest/           # Backtesting components
│       │   ├── backtest_engine.hpp
│       │   ├── slippage_models.hpp
│       │   └── transaction_cost_analysis.hpp
│       ├── core/               # Core system components
│       │   ├── config_base.hpp
│       │   ├── config_manager.hpp
│       │   ├── config_version.hpp
│       │   ├── error.hpp
│       │   ├── logger.hpp
│       │   ├── state_manager.hpp
│       │   └── types.hpp
│       ├── data/               # Data management
│       │   ├── conversion_utils.hpp
│       │   ├── credential_store.hpp
│       │   ├── database_interface.hpp
│       │   ├── market_data_bus.hpp
│       │   └── postgres_database.hpp
│       ├── execution/          # Order execution
│       │   └── execution_engine.hpp
│       ├── instruments/        # Financial instruments
│       │   ├── equity.hpp
│       │   ├── futures.hpp
│       │   ├── instrument.hpp
│       │   └── option.hpp
│       ├── optimization/       # Portfolio optimization
│       │   └── dynamic_optimizer.hpp
│       ├── order/              # Order management
│       │   └── order_manager.hpp
│       ├── portfolio/          # Portfolio management
│       │   └── portfolio_manager.hpp
│       ├── risk/               # Risk management
│       │   └── risk_manager.hpp
│       └── strategy/           # Strategy components
│           ├── base_strategy.hpp
│           ├── database_handler.hpp
│           ├── regime_detector.hpp
│           ├── strategy_interface.hpp
│           ├── trend_following.hpp
│           └── types.hpp
├── src/                        # Implementation files
│   ├── backtest/               # Backtesting implementations
│   │   ├── backtest_engine.cpp
│   │   ├── slippage_model.cpp
│   │   └── transaction_cost_analysis.cpp
│   ├── core/                   # Core system implementations
│   │   ├── config_base.cpp
│   │   ├── config_manager.cpp
│   │   ├── config_version.cpp
│   │   ├── logger.cpp
│   │   └── state_manager.cpp
│   ├── data/                   # Data management implementations
│   │   ├── conversion_utils.cpp
│   │   ├── credential_store.cpp
│   │   ├── market_data_bus.cpp
│   │   └── postgres_database.cpp
│   ├── execution/              # Order execution implementations
│   │   └── execution_engine.cpp
│   ├── instruments/            # Financial instruments implementations
│   │   ├── equity.cpp
│   │   ├── futures.cpp
│   │   └── option.cpp
│   ├── optimization/           # Portfolio optimization implementations
│   │   └── dynamic_optimizer.cpp
│   ├── order/                  # Order management implementations
│   │   └── order_manager.cpp
│   ├── portfolio/              # Portfolio management implementations
│   │   └── portfolio_manager.cpp
│   ├── risk/                   # Risk management implementations
│   │   └── risk_manager.cpp
│   └── strategy/               # Strategy implementations
│       ├── base_strategy.cpp
│       ├── regime_detector.cpp
│       └── trend_following.cpp
├── tests/                      # Unit and integration tests
│   ├── core/                   # Core component tests
│   │   ├── test_config_base.cpp
│   │   ├── test_config_manager.cpp
│   │   ├── test_config_version.cpp
│   │   ├── test_logger.cpp
│   │   ├── test_result.cpp
│   │   └── test_state_manager.cpp
│   └── CMakeLists.txt          # Build configuration for tests
├── CMakeLists.txt              # Main build configuration
└── config_template.json        # Template for configuration files
```

## ⚙️ System Architecture & Component Breakdown

trade-ngin follows a modular, component-based architecture with well-defined interfaces between system components. Here's a detailed breakdown of the major components:

### Core System Components

#### Error Handling System (`error.hpp`)
- Defines custom error codes via `ErrorCode` enum
- Implements `TradeError` class extending `std::runtime_error`
- Provides `Result<T>` template for error propagation throughout the system
- Supports `make_error<T>()` for creating standardized error responses

#### Logging Framework (`logger.hpp`, `logger.cpp`)
- Singleton implementation for system-wide logging
- Configurable log levels (TRACE, DEBUG, INFO, WARNING, ERROR, FATAL)
- Multiple output destinations (console, file, or both)
- Automatic log rotation based on file size
- Thread-safe implementation with mutex protection

#### State Management (`state_manager.hpp`, `state_manager.cpp`)
- Tracks component states throughout the system
- Supports state transitions with validation
- Maintains component metrics and diagnostics
- Enables system-wide health monitoring

#### Configuration Management
- `ConfigBase` - Base class for serializable configurations
- `ConfigManager` - Centralized configuration management
- `ConfigVersion` - Version tracking and migration support
- Environment-specific configuration overrides (dev, staging, prod, backtest)

### Data Management Components

#### Database Interface
- Abstract `DatabaseInterface` defining common database operations
- PostgreSQL implementation via `PostgresDatabase`
- Arrow-based data type support for efficient memory management
- Query building and parameter sanitization

#### Market Data Bus
- Publish-subscribe pattern for market data distribution
- Event-based architecture with callback registration
- Symbol and event type filtering
- Thread-safe implementation

#### Data Conversion Utilities
- Arrow table to/from domain objects conversion
- Type-safe timestamp handling
- Error handling with Result pattern

### Financial Instruments

#### Instrument Interface
- Abstract base class defining common instrument properties
- Asset-specific implementations:
  - `EquityInstrument` - Stocks, ETFs
  - `FuturesInstrument` - Futures contracts
  - `OptionInstrument` - Options with Greeks calculation

### Order Management

#### Order Manager
- Order lifecycle management (creation, submission, cancellation)
- Order book maintenance for status tracking
- Validation and error handling
- Commission calculation

### Execution Engine

- Algorithm-based order execution (TWAP, VWAP, PoV, etc.)
- Execution metrics tracking and analysis
- Position buffering to reduce unnecessary trading
- Custom algorithm extensibility

### Strategy Framework

#### Strategy Interface
- Abstract interface for all trading strategies
- Lifecycle management (init, start, stop, pause, resume)
- Event handling (market data, executions, signals)
- Position and risk management

#### Base Strategy
- Common implementation of strategy interface
- Position tracking and management
- Signal generation and execution
- Risk limit enforcement

#### Strategy Implementations
- `TrendFollowingStrategy` - Multi-timeframe trend following using EMA crossovers
- Extensible framework for adding new strategies

### Risk Management

- Position-level and portfolio-level risk constraints
- Value at Risk (VaR) calculation
- Jump risk monitoring
- Maximum drawdown limits
- Leverage constraints
- Correlation risk monitoring

### Portfolio Management

- Multi-strategy portfolio construction
- Dynamic capital allocation
- Position aggregation and netting
- Portfolio-level optimizations and constraints

### Backtesting Framework

#### Backtest Engine
- Historical market data simulation
- Event-driven architecture
- Realistic execution modeling
- Comprehensive performance metrics
- Transaction cost analysis

#### Slippage Models
- Volume-based slippage
- Spread-based slippage
- Custom slippage model extensibility

#### Transaction Cost Analysis
- Execution quality evaluation
- Implementation shortfall calculation
- Benchmark comparison (VWAP, TWAP, arrival price)
- Cost breakdown (spread, market impact, timing, delay)

### Portfolio Optimization

#### Dynamic Optimizer
- Position optimization considering transaction costs
- Risk constraints enforcement
- Tracking error minimization
- Convex optimization techniques

## 🔄 Workflow & System Flow

### Backtesting Workflow

1. **Initialization**
   - Load configuration from files
   - Initialize database connection
   - Create strategy instances
   - Initialize portfolio manager
   - Configure risk management and optimization components

2. **Market Data Loading**
   - Query historical data from database
   - Convert to internal Bar representation
   - Group data by timestamp for realistic simulation

3. **Strategy Processing**
   - Feed market data to strategies chronologically
   - Generate trading signals based on strategy logic
   - Convert signals to position targets

4. **Portfolio-Level Processing**
   - Aggregate positions across strategies
   - Apply risk management constraints
   - Apply position optimization if enabled
   - Calculate trade sizes based on current vs. target positions

5. **Order Execution Simulation**
   - Apply slippage models to execution prices
   - Calculate transaction costs
   - Generate execution reports
   - Update positions based on fills

6. **Performance Tracking**
   - Update equity curve
   - Calculate drawdowns
   - Track risk metrics over time
   - Record trade statistics

7. **Results Analysis**
   - Calculate performance metrics (Sharpe, Sortino, etc.)
   - Analyze transaction costs
   - Generate visualizations and reports
   - Save results to database

### Live Trading Workflow

1. **Initialization**
   - Load configuration from files
   - Initialize broker connections
   - Create strategy instances
   - Initialize portfolio manager
   - Configure risk management and optimization components

2. **Market Data Subscription**
   - Subscribe to real-time market data feeds
   - Process incoming ticks/bars
   - Update internal market data state

3. **Strategy Processing**
   - Feed real-time data to strategies
   - Generate trading signals based on strategy logic
   - Convert signals to position targets

4. **Portfolio-Level Processing**
   - Aggregate positions across strategies
   - Apply risk management constraints
   - Apply position optimization if enabled
   - Calculate trade sizes based on current vs. target positions

5. **Order Execution**
   - Submit orders to execution engine
   - Select appropriate execution algorithm
   - Route orders to broker/exchange
   - Monitor order status and fills

6. **Position Management**
   - Track current positions and exposures
   - Calculate realized and unrealized P&L
   - Enforce risk limits in real-time

7. **Monitoring and Reporting**
   - Track system health and component states
   - Generate performance reports
   - Log diagnostics and metrics

## 🛠️ Setup, Installation & Building Instructions

### Prerequisites

- C++17 compatible compiler (GCC 8+, Clang 7+, MSVC 2019+)
- CMake 3.17 or higher
- Required libraries:
  - nlohmann_json (for configuration handling)
  - Arrow C++ (for data processing)
  - libpqxx (for PostgreSQL connectivity)
  - GoogleTest (for testing)

### Clone the Repository

```bash
git clone https://github.com/your-organization/trade_ngin.git
cd trade_ngin
```

### Setup Database

1. Create a PostgreSQL database for market data and strategy results
2. Create the necessary schemas and tables (scripts provided in `scripts/db`)
3. Configure database credentials:
   ```bash
   cp config_template.json config.json
   # Edit config.json with your database credentials
   ```

### Building with CMake

```bash
# Create build directory
mkdir build && cd build

# Configure CMake
cmake ..

# Build the library and applications
cmake --build . --config Release

# Run tests
ctest -C Release
```

### Building with Visual Studio

1. Open the project folder in Visual Studio
2. Select "Open CMake Project"
3. Choose the desired configuration (Debug/Release)
4. Build the solution

### Building with CLion

1. Open the project folder in CLion
2. CLion should automatically detect the CMake configuration
3. Choose the desired build configuration
4. Click "Build" to compile the project

## 🚀 Running the System

### Running Backtests

```bash
# From the build directory
./bin/Release/bt_trend
```

### Configuration Parameters

All components in trade-ngin are configurable through JSON configuration files:

#### Backtest Configuration

```json
{
  "strategy_config": {
    "symbols": ["ES", "NQ", "CL", "GC"],
    "asset_class": 0,
    "data_freq": 0,
    "data_type": "ohlcv",
    "start_date": "1609459200",
    "end_date": "1640995200",
    "initial_capital": 1000000.0,
    "commission_rate": 0.0005,
    "slippage_model": 1.0,
    "store_trade_details": true
  },
  "portfolio_config": {
    "initial_capital": 1000000.0,
    "use_risk_management": true,
    "use_optimization": true,
    "risk_config": {
      "var_limit": 0.15,
      "jump_risk_limit": 0.10,
      "max_correlation": 0.7,
      "max_gross_leverage": 4.0,
      "max_net_leverage": 2.0,
      "confidence_level": 0.99,
      "lookback_period": 252,
      "capital": 1000000.0
    },
    "opt_config": {
      "tau": 1.0,
      "capital": 1000000.0,
      "asymmetric_risk_buffer": 0.1,
      "cost_penalty_scalar": 10,
      "max_iterations": 1000,
      "convergence_threshold": 1e-6
    }
  },
  "results_db_schema": "backtest_results"
}
```

#### Strategy Configuration

```json
{
  "capital_allocation": 1000000.0,
  "max_leverage": 3.0,
  "max_drawdown": 0.3,
  "var_limit": 0.1,
  "correlation_limit": 0.7,
  "trading_params": {
    "ES": 50.0,
    "NQ": 20.0,
    "CL": 1000.0,
    "GC": 100.0
  },
  "position_limits": {
    "ES": 100,
    "NQ": 100,
    "CL": 50,
    "GC": 50
  },
  "save_executions": true,
  "save_signals": true,
  "save_positions": true
}
```

## 🔍 Extending & Contributing to the System

### Adding New Strategies

To create a new strategy, follow these steps:

1. Create a new header file in `include/trade_ngin/strategy/`
2. Create a new implementation file in `src/strategy/`
3. Inherit from `BaseStrategy` or implement `StrategyInterface` directly
4. Implement the required methods for your strategy logic
5. Register your strategy with the portfolio manager

Example of a minimally viable strategy:

```cpp
class MyStrategy : public BaseStrategy {
public:
    MyStrategy(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db)
        : BaseStrategy(std::move(id), std::move(config), std::move(db)) {
        // Initialize strategy-specific parameters
    }

    Result<void> on_data(const std::vector<Bar>& data) override {
        // Call base class implementation first
        auto base_result = BaseStrategy::on_data(data);
        if (base_result.is_error()) return base_result;

        // Implement your strategy logic here
        // Generate signals and update positions

        return Result<void>();
    }

    Result<void> initialize() override {
        // Custom initialization logic
        auto base_result = BaseStrategy::initialize();
        if (base_result.is_error()) return base_result;

        // Additional initialization steps

        return Result<void>();
    }
};
```

### Adding New Data Sources

To integrate a new data source:

1. Implement a new class derived from `DatabaseInterface`
2. Override all abstract methods to interact with your data source
3. Implement conversion from your data source's format to trade-ngin's internal types

### Adding New Execution Algorithms

To add a new execution algorithm:

1. Update the `ExecutionAlgo` enum in `execution_engine.hpp`
2. Implement a new method in `ExecutionEngine` for your algorithm
3. Update the `submit_execution` method to handle your new algorithm

### Contribution Guidelines

1. **Code Style**
   - Follow the existing coding style and naming conventions
   - Use snake_case for member variables and method names
   - Use PascalCase for class names
   - Use snake_case for file names

2. **Error Handling**
   - Use the `Result<T>` pattern for functions that can fail
   - Provide detailed error messages with component context

3. **Testing**
   - Write tests for all new functionality
   - Ensure all tests pass before submitting a pull request

4. **Documentation**
   - Document all public interfaces with clear descriptions
   - Update relevant README sections for significant changes

## 🛡️ Error Handling & Troubleshooting

### Common Error Codes

- `ErrorCode::INVALID_ARGUMENT` - Invalid parameter provided
- `ErrorCode::NOT_INITIALIZED` - Component used before initialization
- `ErrorCode::DATABASE_ERROR` - Database connection or query error
- `ErrorCode::DATA_NOT_FOUND` - Requested data not found
- `ErrorCode::INVALID_DATA` - Data validation failed
- `ErrorCode::CONVERSION_ERROR` - Type conversion error
- `ErrorCode::ORDER_REJECTED` - Order rejected by the system
- `ErrorCode::INSUFFICIENT_FUNDS` - Insufficient capital for order
- `ErrorCode::POSITION_LIMIT_EXCEEDED` - Position limit exceeded
- `ErrorCode::INVALID_ORDER` - Invalid order parameters
- `ErrorCode::STRATEGY_ERROR` - Error in strategy logic
- `ErrorCode::RISK_LIMIT_EXCEEDED` - Risk limit exceeded

### Troubleshooting Techniques

#### Database Connection Issues

If you encounter database connection issues:

1. Verify your credentials in `config.json`
2. Ensure the PostgreSQL server is running
3. Check network connectivity and firewall settings
4. Examine the error message from `PostgresDatabase::connect()`

Example of handling database errors:

```cpp
auto db = std::make_shared<PostgresDatabase>(connection_string);
auto connect_result = db->connect();
if (connect_result.is_error()) {
    // Handle error case
    std::cerr << "Database connection error: " << connect_result.error()->what() << std::endl;
    std::cerr << "Error code: " << static_cast<int>(connect_result.error()->code()) << std::endl;
    return 1;
}
```

#### Strategy Initialization Failures

If a strategy fails to initialize:

1. Check that the database connection is valid
2. Verify that the strategy configuration is complete
3. Look for specific error messages in the initialization chain
4. Examine the strategy's `initialize()` method

#### Backtesting Performance Issues

If backtests are running slowly:

1. Reduce the number of symbols or date range
2. Check for inefficient loops or calculations in strategies
3. Optimize database queries to fetch data more efficiently
4. Consider using release builds instead of debug builds

#### Memory Management Issues

If you encounter memory issues:

1. Look for resource leaks in components with manual memory management
2. Ensure that destructors properly clean up allocated resources
3. Check for memory-intensive operations in tight loops
4. Consider using memory profiling tools like Valgrind

## 📊 Performance Considerations & Optimizations

### Data Processing Optimization

trade-ngin uses Apache Arrow for efficient in-memory data representation:

- Zero-copy data sharing between components
- Columnar memory layout for efficient processing
- Vectorized operations for performance
- Minimal memory allocations during data processing

### Concurrency Model

- Components use thread-safe designs with mutex protection
- The market data bus implements a publish-subscribe pattern for efficient event distribution
- Strategy processing can be parallelized for multiple instruments

### Memory Management

- RAII pattern throughout the codebase
- Smart pointers for automatic resource management
- Preallocated buffers for high-performance operations
- Limited heap allocations in performance-critical paths

### Performance Profiling

To profile trade-ngin performance:

1. Build with profiling flags enabled
   ```bash
   cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_PROFILING=ON ..
   ```

2. Run with profiling tools
   ```bash
   # Linux
   valgrind --tool=callgrind ./bin/bt_trend
   
   # Windows
   Visual Studio Profiler
   ```

## 📌 Best Practices & Coding Standards

### Naming Conventions

- **Classes**: PascalCase (e.g., `OrderManager`)
- **Methods**: snake_case (e.g., `add_strategy`)
- **Variables**: snake_case (e.g., `update_allocations`)
- **Constants**: UPPER_CASE (e.g., `MAX_ORDERS`)
- **Files**: snake_case (e.g., `order_manager.cpp`)

### Error Handling

- Use `Result<T>` for functions that can fail
- Avoid throwing exceptions across component boundaries
- Provide detailed error messages with context
- Check all return values for errors

Example of proper error handling:

```cpp
Result<void> MyComponent::doSomething() {
    auto result = dependentComponent_->operation();
    if (result.is_error()) {
        return make_error<void>(
            result.error()->code(),
            "Failed during operation: " + std::string(result.error()->what()),
            "MyComponent"
        );
    }
    
    // Continue with operation
    return Result<void>();
}
```

### Memory Management

- Use smart pointers (`std::shared_ptr`, `std::unique_ptr`)
- Follow RAII principles
- Avoid raw `new`/`delete` operations
- Be mindful of ownership semantics

### Documentation

- Document all public interfaces
- Include parameter descriptions
- Document preconditions and postconditions
- Document error conditions and handling

## 📖 License & Usage Terms

trade-ngin is licensed under the [GPL v3] License. See LICENSE file for details.

Third-party dependencies:
- nlohmann_json: MIT License
- Apache Arrow: Apache License 2.0
- libpqxx: BSD License
- GoogleTest: BSD 3-Clause License
