# Data Module

## Overview

The data module (`src/data/`) handles all database operations, credential management, connection pooling, and data conversion for the trading system.

---

## Architecture

```
data/
├── postgres_database.cpp           # Main database layer
├── postgres_database_extensions.cpp # Additional DB operations
├── database_pooling.cpp            # Connection pool management
├── credential_store.cpp            # Secure credential handling
├── market_data_bus.cpp             # Pub-sub for market data
└── conversion_utils.cpp            # Arrow table conversions
```

---

## Components

### 1. PostgresDatabase

**File**: `postgres_database.cpp`

The central database access layer with 50+ methods.

#### Key Methods

```cpp
// Connection management
Result<void> connect();
void disconnect();
bool is_connected();

// Market data
Result<std::shared_ptr<arrow::Table>> get_market_data(
    const std::vector<std::string>& symbols,
    const Timestamp& start_date,
    const Timestamp& end_date,
    AssetClass asset_class,
    DataFrequency freq,
    const std::string& data_type = "continuous");

// Storage operations
Result<void> store_executions(
    const std::vector<ExecutionReport>& executions,
    const std::string& strategy_id,
    const std::string& strategy_name,
    const std::string& portfolio_id,
    const std::string& table_name);

Result<void> store_positions(
    const std::vector<Position>& positions,
    const std::string& strategy_id,
    const std::string& strategy_name,
    const std::string& portfolio_id,
    const std::string& table_name);

Result<void> store_signals(
    const std::unordered_map<std::string, double>& signals,
    const std::string& strategy_id,
    const std::string& strategy_name,
    const std::string& portfolio_id,
    const Timestamp& timestamp,
    const std::string& table_name);

// Query operations
Result<std::vector<std::string>> get_symbols(
    AssetClass asset_class,
    DataFrequency freq,
    const std::string& data_type);

Result<std::unordered_map<std::string, double>> get_latest_prices(
    const std::vector<std::string>& symbols,
    AssetClass asset_class,
    DataFrequency freq,
    const std::string& data_type);

Result<std::unordered_map<std::string, Position>> load_positions_by_date(
    const std::string& strategy_id,
    const std::string& strategy_name,
    const std::string& portfolio_id,
    const Timestamp& date,
    const std::string& table_name);

// Metadata
Result<std::shared_ptr<arrow::Table>> get_contract_metadata();
```

---

### 2. DatabasePool

**File**: `database_pooling.cpp`

Connection pool for efficient database access.

```cpp
// Initialize pool
Result<void> DatabasePool::instance().initialize(
    const std::string& connection_string,
    size_t pool_size = 3);

// Acquire connection (RAII guard)
auto guard = DatabasePool::instance().acquire_connection();
auto db = guard.get();

// Use connection
auto result = db->get_market_data(...);

// Connection automatically returned when guard goes out of scope
```

---

### 3. CredentialStore

**File**: `credential_store.cpp`

Secure credential management.

```cpp
// Create from config file
auto credentials = std::make_shared<CredentialStore>("config.json");

// Get credentials
auto username = credentials->get<std::string>("database", "username");
auto password = credentials->get<std::string>("database", "password");

// Build connection string
std::string conn = "postgresql://" +
    username.value() + ":" + password.value() + "@" +
    host.value() + ":" + port.value() + "/" + name.value();
```

---

### 4. MarketDataBus

**File**: `market_data_bus.cpp`

Pub-sub pattern for market data distribution.

```cpp
// Subscribe to market data
MarketDataBus::instance().subscribe(
    "ES",
    [](const Bar& bar) {
        // Handle new bar
    }
);

// Publish new data
MarketDataBus::instance().publish(bars);
```

---

### 5. ConversionUtils

**File**: `conversion_utils.cpp`

Apache Arrow table conversions.

```cpp
// Convert Arrow table to Bar vector
std::vector<Bar> bars = convert_table_to_bars(arrow_table);

// Convert Bar vector to Arrow table
auto table = convert_bars_to_table(bars);
```

---

## Connection String Format

```
postgresql://username:password@host:port/database
```

Example:
```
postgresql://postgres:your-password@localhost:5432/your_database
```

---

## Configuration

From `config.json`:

```json
{
  "database": {
    "host": "localhost",
    "port": "5432",
    "username": "your-username",
    "password": "your-password",
    "name": "your-database"
  }
}
```

---

## Usage Example

```cpp
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/credential_store.hpp"

// Initialize from config
auto credentials = std::make_shared<CredentialStore>("config.json");

std::string conn_string = "postgresql://" +
    credentials->get<std::string>("database", "username").value() + ":" +
    credentials->get<std::string>("database", "password").value() + "@" +
    credentials->get<std::string>("database", "host").value() + ":" +
    credentials->get<std::string>("database", "port").value() + "/" +
    credentials->get<std::string>("database", "name").value();

// Initialize pool
auto pool_result = DatabasePool::instance().initialize(conn_string, 3);

// Acquire connection
auto guard = DatabasePool::instance().acquire_connection();
auto db = guard.get();

// Query data
std::vector<std::string> symbols = {"ES", "NQ"};
Timestamp start = parse_date("2024-01-01");
Timestamp end = parse_date("2024-12-31");

auto result = db->get_market_data(
    symbols, start, end,
    AssetClass::FUTURES,
    DataFrequency::DAILY,
    "continuous"
);

if (result.is_ok()) {
    auto table = result.value();
    // Process data...
}
```

---

## Error Handling

Database operations return `Result<T>`:

```cpp
auto result = db->connect();
if (result.is_error()) {
    std::cerr << "Connection failed: " << result.error()->what() << std::endl;
}
```

Common errors:
- `DATABASE_ERROR` - Connection or query failure
- `DATA_NOT_FOUND` - No matching data
- `INVALID_ARGUMENT` - Bad query parameters

---

## Dependencies

- **libpqxx** - PostgreSQL C++ interface
- **Apache Arrow** - Columnar data processing
- **nlohmann_json** - Configuration parsing

---

## Testing

```bash
cd build
ctest -R data --output-on-failure
```

---

## References

- [Backtest Module](../backtest/README.md) - Uses data for historical testing
- [Live Trading Module](../live/README.md) - Uses data for production
- [Strategy Module](../strategy/README.md) - Consumes market data
