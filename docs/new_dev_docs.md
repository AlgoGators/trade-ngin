# Trade-Ngin System Overview

## Why C++?

While Trade-Ngin isn't a high-frequency trading system, we chose C++ for several strategic reasons:

1. **Performance Without Compromise**
   - Direct memory management for predictable performance
   - Zero-copy operations with modern C++ features
   - Minimal garbage collection pauses
   - Efficient CPU cache utilization

2. **Industry Standard**
   - Most quantitative finance firms use C++
   - Essential skill for quant developers
   - Large ecosystem of financial libraries
   - Strong community support

3. **Learning Opportunity**
   - Understanding memory management
   - Working with complex type systems
   - Learning concurrent programming
   - Developing systems programming skills

4. **Future Extensibility**
   - Easy to add low latency components later
   - Direct hardware access if needed
   - Integration with low-level systems
   - Performance optimization potential

## Getting Started

### Prerequisites
1. **Development Environment Setup**
   ```bash
   # Required tools
   - C++ compiler (gcc/g++ 9.0+ or clang)
   - CMake (3.10+)
   - Git
   - PostgreSQL client
   - Visual Studio Code or CLion (recommended)
   ```

2. **Required Libraries**
   ```bash
   # Core libraries
   - Boost (1.75+)
   - Apache Arrow
   - libpqxx
   - nlohmann-json
   - spdlog
   - GTest
   ```

3. **Building Your First Component**
   ```bash
   # Clone and build
   git clone https://github.com/your-org/trade-ngin.git
   cd trade-ngin
   mkdir build && cd build
   cmake ..
   make
   
   # Run tests
   ./bin/test_ibkr_paper_trader
   ```

### First Steps
1. **Start with the Test Suite**
   - Begin by reading `tests/test_ibkr_paper_trader.cpp`
   - This shows how components interact
   - Try running and modifying tests(will not work without CMAKE/docker setup see the cmke.md file [NOT NEEDED NOW])

2. **Explore Core Components**
   ```cpp
   // Example: Creating your first strategy
   class MyFirstStrategy : public Strategy {
       void onMarketData(const OHLCV& data) override {
           // Your logic here
           Logger::getInstance().info("Received data for {}", data.symbol);
       }
   };
   ```

3. **Common Development Tasks**
   - Building components: `make <component_name>`
   - Running tests: `./bin/test_*`
   - Debugging: Use VS Code's built-in debugger
   - Logging: Use `Logger::getInstance()`

### Quick Reference
1. **Key Files for Beginners**
   ```
   trade_ngin/
   ├── docs/               # Start here
   ├── tests/             # Example usage
   ├── trade_ngin/
   │   ├── strategy/      # Trading strategies
   │   ├── system/        # Core system
   │   └── data/          # Data handling
   ```

2. **Common Code Patterns**
   ```cpp
   // 1. Resource Management
   auto ptr = std::make_unique<Resource>();  // Always use smart pointers
   
   // 2. Error Handling
   try {
       // Your code
   } catch (const std::exception& e) {
       Logger::getInstance().error("Error: {}", e.what());
   }
   
   // 3. Thread Safety
   std::lock_guard<std::mutex> lock(mutex_);  // RAII locking
   ```

3. **Development Workflow**
   - Write tests first
   - Implement feature
   - Run tests
   - Review memory/thread safety
   - Submit PR

## Writing Safe and Correct C++

### Type Safety Examples

#### Good C++ Code:
```cpp
// GOOD: Strong typing with meaningful types
struct Price {
    explicit Price(double value) : value_(value) {
        if (value < 0) throw std::invalid_argument("Price cannot be negative");
    }
    double get() const { return value_; }
private:
    double value_;
};

struct Quantity {
    explicit Quantity(int value) : value_(value) {
        if (value <= 0) throw std::invalid_argument("Quantity must be positive");
    }
    int get() const { return value_; }
private:
    int value_;
};

class Order {
public:
    Order(Price price, Quantity qty) : price_(price), qty_(qty) {}
    // Type safety prevents accidental swapping of price and quantity
private:
    Price price_;
    Quantity qty_;
};

// Usage:
void submitOrder() {
    // Compiler enforces correct types
    Order order(Price(100.50), Quantity(100));
    // Order order(Quantity(100), Price(100.50)); // Won't compile - wrong order
}
```

#### Bad C++ Code:
```cpp
// BAD: Weak typing, easy to make mistakes
class Order {
public:
    // Bad: Raw types can be easily confused
    Order(double price, int qty) : price_(price), qty_(qty) {}
    // Bad: Public members allow invalid modifications
    double price_;
    int qty_;
};

// Usage:
void submitOrder() {
    // Easy to make mistakes:
    Order order(100, 100.50); // Compiles but price/qty are swapped!
    order.price_ = -50.0; // No validation
    order.qty_ = -100; // Invalid quantity
}
```

### Memory Safety Examples

#### Good C++ Code:
```cpp
// GOOD: RAII and smart pointers
class DataHandler {
public:
    DataHandler(const std::string& config) {
        // Resource acquisition is initialization
        connection_ = std::make_unique<DBConnection>(config);
    }
    
    void processData() {
        // No need to check for nullptr - guaranteed to exist
        auto data = connection_->fetch();
        // No memory leaks - automatic cleanup
        std::vector<std::shared_ptr<MarketData>> market_data;
        
        for (const auto& row : data) {
            market_data.push_back(std::make_shared<MarketData>(row));
        }
        // Vector and contained pointers cleaned up automatically
    }
    
private:
    std::unique_ptr<DBConnection> connection_;  // Automatically deleted
};
```

#### Bad C++ Code:
```cpp
// BAD: Manual memory management
class DataHandler {
public:
    DataHandler(const char* config) {
        // Bad: Manual memory management
        connection = new DBConnection(config);
    }
    
    void processData() {
        // Bad: Possible null pointer dereference
        if (connection != nullptr) {
            auto* data = connection->fetch();
            // Bad: Manual array management
            MarketData** market_data = new MarketData*[100];
            
            // Bad: Memory leaks if exception occurs
            for (int i = 0; i < 100; i++) {
                market_data[i] = new MarketData();
            }
            
            // Bad: Easy to forget cleanup
            // delete[] market_data; // Forgotten cleanup
        }
    }
    
    ~DataHandler() {
        // Bad: Manual deletion
        delete connection;
    }
    
private:
    DBConnection* connection;  // Raw pointer
};
```

### Thread Safety Examples

#### Good C++ Code:
```cpp
// GOOD: Thread-safe singleton with proper synchronization
class OrderManager {
public:
    static OrderManager& getInstance() {
        static OrderManager instance;  // Thread-safe in C++11
        return instance;
    }
    
    void submitOrder(const Order& order) {
        std::lock_guard<std::mutex> lock(mutex_);  // RAII lock
        orders_.push_back(order);
        notifyListeners();
    }
    
private:
    OrderManager() = default;
    std::mutex mutex_;
    std::vector<Order> orders_;
    
    void notifyListeners() {
        // Thread-safe notification using condition variable
        std::lock_guard<std::mutex> lock(notify_mutex_);
        updated_ = true;
        cv_.notify_all();
    }
    
    std::mutex notify_mutex_;
    std::condition_variable cv_;
    bool updated_{false};
};
```

#### Bad C++ Code:
```cpp
// BAD: Race conditions and thread safety issues
class OrderManager {
public:
    static OrderManager* getInstance() {
        // Bad: Race condition in initialization
        if (!instance_) {
            instance_ = new OrderManager();
        }
        return instance_;
    }
    
    void submitOrder(const Order& order) {
        // Bad: No synchronization
        orders_.push_back(order);  // Race condition
        notifyListeners();  // Race condition
    }
    
private:
    static OrderManager* instance_;
    std::vector<Order> orders_;  // No protection
    
    void notifyListeners() {
        // Bad: No synchronization
        updated_ = true;  // Race condition
    }
    
    bool updated_{false};  // No protection
};
```

## Introduction
Trade-Ngin is a high-performance algorithmic trading system designed for executing automated trading strategies with a focus on futures markets. The system is built with C++ for optimal performance and uses modern libraries like Boost.Beast for networking, Apache Arrow for efficient data handling, and libpqxx for PostgreSQL integration.

## Core Components

### 1. Data Layer (`trade_ngin/data/`)
#### OHLCVDataHandler
- **Purpose**: Core market data management system
- **Key Files**: 
  - `ohlcv_data_handler.hpp/cpp`: Main interface
  - `database_client.hpp/cpp`: Database connectivity
  - `arrow_data_handler.hpp`: Arrow data structures

**Technical Details**:
- Uses Apache Arrow for zero-copy data handling
- Implements custom memory pools for data caching
- Supports both real-time and historical data modes
- Query optimization using prepared statements
- Automatic data type conversion and validation

**Data Flow**:
```
PostgreSQL -> DatabaseClient -> Arrow Tables -> OHLCVDataHandler -> Strategy
```

**Key Methods**:
```cpp
std::shared_ptr<arrow::Table> getOHLCVArrowTable(
    const std::string& start_date,
    const std::string& end_date,
    const std::vector<std::string>& symbols = {}
);

void setCallback(std::function<void(const OHLCV&)> callback);
```

### 2. Trading System (`trade_ngin/system/`)
#### IBKRInterface
- **Purpose**: Interactive Brokers API integration
- **Components**:
  - Base Interface (`ibkr_interface.hpp`)
  - Real Implementation (`real_ibkr_interface.hpp/cpp`)
  - Paper Trading (`paper_trading.hpp/cpp`)
  - Mock Interface (`mock_ib_interface.hpp`)

**Technical Details**:
- Uses Boost.Beast for WebSocket connections
- Implements rate limiting with token bucket algorithm
- Automatic session management and reconnection
- Robust error handling with exponential backoff
- Thread-safe order management

**Order Flow**:
```
Strategy -> IBKRInterface -> Rate Limiter -> API Request -> Order Status -> Portfolio Update
```

**Key Methods**:
```cpp
json submitOrder(const Order& order);
json modifyOrder(const std::string& orderId, const Order& updatedOrder);
json cancelOrder(const std::string& orderId);
json getOrderStatus(const std::string& orderId);
```

#### Portfolio Management
- **Purpose**: Position and risk tracking
- **Components**:
  - Position Manager (`portfolio.hpp/cpp`)
  - Risk Engine (`risk_engine.hpp/cpp + risk-ngn`)
  - P&L Calculator (`pnl_calculator.hpp/cpp`)

**Technical Details**:
- Real-time position tracking with thread-safe updates
- Multi-level risk checks (pre-trade, post-trade)
- VaR calculations using historical simulation
- Dynamic position sizing based on volatility
- Automatic stop-loss and take-profit management

**Risk Calculations**:
```cpp
// Position sizing with Kelly Criterion
double getPositionSize(const SymbolPosition& pos, double vol_scalar, double price) {
    double kelly_fraction = (win_rate * avg_win - (1 - win_rate) * avg_loss) / avg_win;
    double position_size = account_value * kelly_fraction * vol_scalar;
    return std::min(position_size, max_position_size);
}
```

### 3. Strategy Layer (`trade_ngin/strategy/`)
#### TrendStrategy
- **Purpose**: Example momentum-based trading strategy
- **Components**:
  - Signal Generation (`signals.cpp + trend_strategy.hpp/cpp`)
  - Volatility Regime (`volatility_regime.hpp/cpp`)


**Technical Details**:
- Implements adaptive trend following
- Uses multiple timeframe analysis
- Volatility regime classification
- Dynamic threshold adjustment
- Position sizing based on ATR

**Signal Generation**:
```cpp
double calculateSignal(const std::vector<double>& prices, 
                      const std::vector<double>& volumes) {
    double short_ma = calculateEMA(prices, short_period);
    double long_ma = calculateEMA(prices, long_period);
    double vol = calculateVolatility(prices, vol_period);
    
    double trend_strength = (short_ma - long_ma) / long_ma;
    double vol_adjusted_signal = trend_strength * (target_vol / vol);
    
    return std::clamp(vol_adjusted_signal, -1.0, 1.0);
}
```

## System Architecture

### Data Flow Architecture
```
Market Data Source
       ↓
DatabaseClient (PostgreSQL)
       ↓
OHLCVDataHandler (Arrow)
       ↓
Strategy Layer
       ↓
Risk Engine
       ↓
Order Manager
       ↓
IBKRInterface
       ↓
Market
```

### Threading Model
- Main thread: Strategy execution
- Data thread: Market data processing
- Order thread: Order management
- Risk thread: Risk calculations
- Logging thread: Asynchronous logging

### Memory Management
- Custom allocators for high-performance data structures
- Zero-copy data passing using Arrow
- Thread-safe smart pointers
- RAII principles throughout

## Performance Considerations

### Latency Management
- Critical path optimization
- Lock-free data structures
- Memory pre-allocation
- Custom memory pools

### Database Optimization
- Prepared statements
- Connection pooling
- Batch processing
- Index optimization

### Network Optimization
- Keep-alive connections
- Request batching
- Compression
- Connection pooling

## Error Handling

### Retry Mechanism
```cpp
template<typename F>
auto executeWithRetry(F&& operation,
                     const std::string& operationName,
                     const RetryConfig& config = RetryConfig()) {
    int retryCount = 0;
    std::chrono::milliseconds delay = config.initialDelay;
    
    while (true) {
        try {
            return operation();
        } catch (const std::exception& e) {
            if (retryCount >= config.maxRetries) throw;
            std::this_thread::sleep_for(delay);
            delay *= 2; // Exponential backoff
            retryCount++;
        }
    }
}
```

### Error Categories
1. Network Errors
   - Connection failures
   - Timeouts
   - Rate limiting
2. Market Data Errors
   - Invalid data
   - Missing data
   - Stale data
3. Order Errors
   - Rejection
   - Partial fills
   - Cancel failures

## Configuration System

### Environment Variables
```bash
IBKR_API_URL=https://localhost:5000/v1/api
IBKR_ACCOUNT_ID=DU123456
IBKR_AUTH_TOKEN=your_token_here
IBKR_PAPER_TRADING=true
(make sure same is true for the psql connection)
```

### Risk Parameters
```cpp
struct RiskParams {
    double max_position_size;
    double max_drawdown;
    double daily_loss_limit;
    double portfolio_risk;
    double vol_target;
    int max_positions;
};
```

## Progressive Learning Path

### Week 1-2: Basics
1. **Environment Setup**
   - Install all prerequisites
   - Build the system
   - Run basic tests
   - Set up debugging

2. **Code Navigation**
   - Read through test files
   - Understand basic system flow
   - Learn to use logging
   - Practice with GDB/LLDB

3. **First Changes**
   ```cpp
   // Start with simple modifications
   void MyFirstStrategy::onMarketData(const OHLCV& data) {
       // Add basic logging
       Logger::getInstance().info("Processing {}: price={}", 
           data.symbol, data.close);
   }
   ```

### Week 3-4: Core Concepts
1. **Memory Management**
   - Smart pointers usage
   - RAII patterns
   - Memory leak detection
   - Valgrind basics

2. **Thread Safety**
   - Mutex usage
   - Race condition detection
   - Lock-free programming
   - Thread sanitizer

3. **Error Handling**
   - Exception hierarchy
   - Error propagation
   - Retry mechanisms
   - Logging best practices

### Week 5-6: System Components
1. **Data Layer**
   - PostgreSQL interaction
   - Arrow data structures
   - Zero-copy operations
   - Data validation

2. **Trading System**
   - Order lifecycle
   - Position management
   - Risk checks
   - Paper trading

3. **Strategy Layer**
   - Signal generation
   - Position sizing
   - Risk management
   - Backtesting

### Week 7-8: Advanced Topics
1. **Performance**
   - Profiling tools
   - Optimization techniques
   - Cache efficiency
   - Lock contention

2. **Testing**
   - Unit test writing
   - Integration testing
   - Performance testing
   - Mock objects

3. **Production Readiness**
   - Monitoring
   - Alerting
   - Deployment
   - Documentation

### Milestones
1. **Week 1-2 Goal**: Modify and run test suite successfully
2. **Week 3-4 Goal**: Implement a basic strategy
3. **Week 5-6 Goal**: Create full trading component
4. **Week 7-8 Goal**: Deploy to paper trading

### Common Pitfalls and Solutions
1. **Build Issues**
   ```bash
   # Common fix for linking errors
   cmake clean .
   rm -rf build/
   cmake -DCMAKE_BUILD_TYPE=Debug ..
   ```

2. **Runtime Issues**
   ```cpp
   // Always check pointer validity
   if (!data_ptr) {
       Logger::getInstance().error("Invalid data pointer");
       return;
   }
   ```

3. **Performance Issues**
   ```cpp
   // Use appropriate containers
   std::unordered_map<std::string, double> prices;  // O(1) lookup
   std::vector<double> returns;  // Contiguous memory
   ```

# Learning Exercises

### Exercise 1: System Navigation
1. Find and explain the main order execution flow in the codebase.
2. Locate where API rate limits are implemented and explain the mechanism.
3. Identify where trading signals are generated and explain the process.

### Exercise 2: Data Flow Implementation
1. Trace the market data flow from database to strategy:
   - How is data fetched from PostgreSQL?
   - How is it converted to Arrow format?
   - How does it reach the strategy layer?

2. Implement a signal flow:
   - Generate a simple moving average signal
   - Process it through the portfolio
   - Submit it to the IBKR interface

3. Add position update handling:
   - Implement order status monitoring
   - Update portfolio positions
   - Trigger risk limit checks

### Exercise 3: Configuration Management
1. Add new API credentials to the configuration:
   ```cpp
   // Add to ibkr_config.hpp:
   struct IBKRConfig {
       // ... existing fields ...
       std::string api_key;
       std::string api_secret;
   };
   ```

2. Implement risk limit configuration:
   ```cpp
   // Add to risk_engine.hpp:
   struct RiskLimits {
       double max_position_size;
       double max_drawdown;
       double daily_loss_limit;
   };
   ```

3. Add strategy parameters:
   ```cpp
   // Add to trend_strategy.hpp:
   struct StrategyParams {
       int lookback_period;
       double entry_threshold;
       double exit_threshold;
   };
   ```

### Exercise 4: Error Handling Implementation
1. Implement API retry logic:
   ```cpp
   // Template to implement in error_handler.hpp:
   template<typename F>
   auto executeWithRetry(F&& operation, int max_retries) {
       // TODO: Implement retry logic with exponential backoff
   }
   ```

2. Add database error handling:
   ```cpp
   // Template to implement in database_client.cpp:
   void DatabaseClient::connect() {
       // TODO: Implement connection error handling
   }
   ```

3. Implement risk limit violations:
   ```cpp
   // Template to implement in risk_engine.cpp:
   bool RiskEngine::checkViolations() {
       // TODO: Implement risk limit checks
   }
   ```

### Exercise 5: Code Location Challenge
1. Find where maximum position size is enforced
2. Locate rate limiting implementation
3. Find order status logging code

### Exercise 6: Configuration Challenge
1. Add new API endpoints
2. Configure logging levels
3. Set up strategy parameters

### Exercise 7: Data Flow Challenge
1. Trace a market tick through the system
2. Implement position updates
3. Calculate P&L

### Exercise 8: Performance Optimization
1. Identify bottlenecks in data processing
2. Optimize memory usage in the Arrow data handler
3. Improve thread synchronization in order management

### Exercise 9: Testing Challenge
1. Write unit tests for signal generation
2. Implement integration tests for order flow
3. Add performance tests for data handling

### Exercise 10: Documentation Challenge
1. Document a complex algorithm
2. Create API documentation
3. Write a troubleshooting guide

## Learning Exercises Answer Key

### Exercise 1: System Navigation
1. Main order execution: `real_ibkr_interface.cpp` (submitOrder method)
2. API rate limits: `ibkr_interface.cpp` (checkRateLimit method)
3. Trading signals: `trend_strategy.cpp` (generateSignals method)

### Exercise 2: Data Flow
1. Market data flow: 
   ```
   DatabaseClient::fetchData() -> 
   OHLCVDataHandler::processData() -> 
   Strategy::onMarketData()
   ```
2. Signal flow:
   ```
   Strategy::generateSignals() -> 
   Portfolio::processSignal() -> 
   IBKRInterface::submitOrder()
   ```
3. Position updates:
   ```
   IBKRInterface::getOrderStatus() -> 
   Portfolio::updatePosition() -> 
   RiskEngine::checkLimits()
   ```

### Exercise 3: Configuration
1. API credentials: `ibkr_config.hpp`
2. Risk limits: `risk_engine.hpp`
3. Strategy parameters: `trend_strategy.hpp`

### Exercise 4: Error Handling
1. API retry: `error_handler.hpp` (executeWithRetry)
2. Database: `database_client.cpp` (connect method)
3. Risk limits: `risk_engine.cpp` (checkViolations)

### Code Location Challenge
1. Max position size: `portfolio.hpp` (PortfolioLimits struct)
2. Rate limiting: `ibkr_interface.cpp` (RateLimiter class)
3. Order status logging: `real_ibkr_interface.cpp` (getOrderStatus)

### Configuration Challenge
1. API endpoints: `ibkr_config.hpp`
2. Logging levels: `Logger.hpp`
3. Strategy parameters: `trend_strategy.hpp` (StrategyParams struct)

### Data Flow Challenge
1. Market tick -> trade: 
   ```
   OHLCVDataHandler::onTick() -> 
   Strategy::onMarketData() -> 
   Portfolio::executeOrder()
   ```
2. Position updates: `portfolio.cpp` (updatePosition method)
3. P&L calculation: `portfolio.cpp` (calculatePnL method)

## Common Gotchas
1. Always check rate limits before API calls
2. Use proper error handling with retries
3. Validate market data before processing
4. Check risk limits before order submission
5. Monitor memory usage with large datasets
6. Handle partial fills correctly
7. Always use thread-safe operations
8. Implement proper logging
9. Test with paper trading first
10. Monitor system resource usage

Remember: The system is designed for safety first, then performance. Always prioritize risk management over execution speed.

Let me know how this doc helps you get started! Rate it 1-10 and leave comments to me about how to improve.

