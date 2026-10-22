# Data Module

## Overview

The data module (`src/data/`) holds the database layer, the connection pool, the credential store,
the market data bus and the Arrow conversions, and the three pieces that decide what a futures bar
is: the session classification of a symbol's bar on a date, the futures roll series and the listing
dates. Headers are in `include/trade_ngin/data/`.

---

## Architecture

```
data/
├── postgres_database.cpp            # Main database layer
├── postgres_database_extensions.cpp # Additional DB operations
├── database_pooling.cpp             # Connection pool management
├── credential_store.cpp             # Reads credentials from a JSON file
├── market_data_bus.cpp              # Pub-sub for market data events
├── conversion_utils.cpp             # Arrow table conversions
├── market_data_utils.cpp            # The bar queries (one futures row per symbol and day)
├── session_classifier.cpp           # What a symbol's bar, or its absence, is on a date
├── roll_series.cpp                  # Contract switches, the back-adjusted series, roll legs
└── listing_dates.cpp                # Listing dates and vendor id relabels
```

The session classifier, the roll series and the listing dates are described in
[docs/FUTURES_ROLLS.md](../../docs/FUTURES_ROLLS.md) and
[docs/DATA_SOURCES_OF_TRUTH.md](../../docs/DATA_SOURCES_OF_TRUTH.md); the design comments at the
top of their headers are the reference.

---

## Components

### 1. PostgresDatabase

**File**: `postgres_database.cpp` (header `include/trade_ngin/data/postgres_database.hpp`)

The central database access layer with 50+ methods.

#### Key Methods

```cpp
// Connection management
Result<void> connect();
void disconnect();
bool is_connected() const;

// Market data
Result<std::shared_ptr<arrow::Table>> get_market_data(
    const std::vector<std::string>& symbols,
    const Timestamp& start_date,
    const Timestamp& end_date,
    AssetClass asset_class,
    DataFrequency freq = DataFrequency::DAILY,
    const std::string& data_type = "ohlcv");

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
    DataFrequency freq = DataFrequency::DAILY,
    const std::string& data_type = "ohlcv");

Result<std::unordered_map<std::string, double>> get_latest_prices(
    const std::vector<std::string>& symbols,
    AssetClass asset_class,
    DataFrequency freq = DataFrequency::DAILY,
    const std::string& data_type = "ohlcv");

Result<std::unordered_map<std::string, Position>> load_positions_by_date(
    const std::string& strategy_id,
    const std::string& strategy_name,
    const std::string& portfolio_id,
    const Timestamp& date,
    const std::string& table_name = "trading.positions");

// Metadata
Result<std::shared_ptr<arrow::Table>> get_contract_metadata() const;
```

`data_type` is the base name of the table. The table read is `<schema>.<data_type>_<frequency>`
(`build_table_name`, `include/trade_ngin/core/types.hpp:586`), so `AssetClass::FUTURES`,
`DataFrequency::DAILY` and `"ohlcv"` read `futures_data.ohlcv_1d`. Futures symbols are stored with
the continuous suffix, for example `MES.v.0`.

`get_market_data` also publishes one `BAR` event per returned row on the `MarketDataBus` unless
publishing is switched off.

---

### 2. DatabasePool

**File**: `database_pooling.cpp`

Connection pool for database access.

```cpp
// Initialize pool
Result<void> initialize(const std::string& connection_string, size_t pool_size = 5);

// Acquire connection (RAII guard)
auto guard = DatabasePool::instance().acquire_connection();
auto db = guard.get();   // std::shared_ptr<PostgresDatabase>

// Connection automatically returned when guard goes out of scope
```

The runners pass `database.num_connections` from `defaults.json` as the pool size (5 when the key
is absent).

---

### 3. CredentialStore

**File**: `credential_store.cpp`

Reads values from a JSON file by section and key. The file is the path given to the constructor
(default `config.json`), or the path in the environment variable `TRADING_CONFIG_PATH` when that is
set to a `.json` path. `EmailSender` has a constructor that takes a `CredentialStore`
(`include/trade_ngin/core/email_sender.hpp:46`); the runners do not use it and build the sender from
the book's `email.json`.

```cpp
auto credentials = std::make_shared<CredentialStore>("path/to/file.json");

// get<T>(section, key) returns Result<T>
auto username = credentials->get<std::string>("database", "username");
if (username.is_ok()) {
    const std::string& value = username.value();
}
```

The runners do not build their database connection through `CredentialStore`. They load the
`database` block of `config/defaults.json` with `ConfigLoader` and take the connection string from
`DatabaseConfig::get_connection_string()` (`include/trade_ngin/core/config_loader.hpp`).

---

### 4. MarketDataBus

**File**: `market_data_bus.cpp`

Pub-sub for market data events. A subscriber registers one `SubscriberInfo` (an id, the event types
and symbols it wants, and a callback that receives a `MarketDataEvent`); `publish` takes one
`MarketDataEvent`.

```cpp
SubscriberInfo info{
    "my_subscriber",
    {MarketDataEventType::BAR},
    {"MES.v.0"},
    [](const MarketDataEvent& event) {
        // event.symbol, event.timestamp, event.numeric_fields.at("close"), ...
    }};
auto subscribed = MarketDataBus::instance().subscribe(info);   // Result<void>

MarketDataEvent event;
event.type = MarketDataEventType::BAR;
event.symbol = "MES.v.0";
MarketDataBus::instance().publish(event);

auto unsubscribed = MarketDataBus::instance().unsubscribe("my_subscriber");   // Result<void>
```

`set_publish_enabled(false)` switches publishing off; the backtest does this while it loads data.

---

### 5. DataConversionUtils

**File**: `conversion_utils.cpp`

Apache Arrow table conversions.

```cpp
// Convert an Arrow table of OHLCV rows to bars
auto bars = DataConversionUtils::arrow_table_to_bars(arrow_table);   // Result<std::vector<Bar>>
```

There is no conversion from bars back to a table. The class also carries the type-checked cell
readers `safe_get_double`, `safe_get_int64` and `safe_get_string`, each returning a `Result`.

---

## Connection String Format

```
postgresql://username:password@host:port/database
```

`DatabaseConfig::get_connection_string()` builds it from the `database` block.

---

## Configuration

The `database` block of `config/defaults.json` (copy `config_template/defaults.json` and replace
the placeholders):

```json
{
  "database": {
    "host": "YOUR_DB_HOST",
    "port": "YOUR_DB_PORT",
    "username": "YOUR_DB_USERNAME",
    "password": "YOUR_DB_PASSWORD",
    "name": "YOUR_DB_NAME",
    "num_connections": 5
  }
}
```

`host`, `username`, `password` and `name` are required. See
[docs/CONFIG_GUIDE.md](../../docs/CONFIG_GUIDE.md).

---

## Usage Example

This is how the runners open the database and read bars (run from the repository root, with
`./config` in place):

```cpp
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

// Load the configuration of one book
auto app_config_result = ConfigLoader::load("./config", "conservative");
if (app_config_result.is_error()) {
    std::cerr << app_config_result.error()->what() << std::endl;
    return 1;
}
auto app_config = app_config_result.value();

// Initialize pool
auto pool_result = DatabasePool::instance().initialize(
    app_config.database.get_connection_string(), app_config.database.num_connections);
if (pool_result.is_error()) {
    std::cerr << pool_result.error()->what() << std::endl;
    return 1;
}

// Acquire connection
auto guard = DatabasePool::instance().acquire_connection();
auto db = guard.get();
if (!db || !db->is_connected()) {
    return 1;
}

// Query data: daily futures bars from futures_data.ohlcv_1d
std::vector<std::string> symbols = {"MES.v.0", "MNQ.v.0"};
Timestamp start, end;
if (!core::parse_utc_date("2024-01-01", start) || !core::parse_utc_date("2024-12-31", end)) {
    return 1;
}

auto result = db->get_market_data(symbols, start, end, AssetClass::FUTURES);

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
- `CONNECTION_ERROR`: the connection could not be opened
- `DATABASE_ERROR`: a query failed
- `DATA_NOT_FOUND`: no matching data
- `INVALID_ARGUMENT`: bad query parameters

---

## Dependencies

- **libpqxx**: PostgreSQL C++ interface
- **Apache Arrow**: columnar data processing
- **nlohmann_json**: configuration parsing

---

## Testing

The tests are registered under their GoogleTest names (`Suite.Test`), and `ctest -R` matches a
regular expression against that name. To run the main suites of `tests/data` by name (the
expression matches any test whose suite or test name contains one of these words, so it also
selects the roll-leg cases of `tests/backtest`, `tests/live`, `tests/core` and
`tests/transaction_cost`, and it leaves out the suites of `tests/data`
it does not name):

```bash
cd build
ctest -R "PostgresDatabase|DatabasePool|CredentialStore|ConversionUtils|MarketDataBus|MarketDataUtils|SessionClassifier|RollSeries|RollLegs" --output-on-failure
```

---

## References

- [Backtest Module](../backtest/README.md): reads stored bars for a backtest
- [Live Trading Module](../live/README.md): reads and writes the live tables
- [Strategy Module](../strategy/README.md): consumes market data
