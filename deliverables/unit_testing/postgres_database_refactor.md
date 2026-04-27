# postgres_database.cpp — Refactor Plan for Unit Test Compatibility

**Status:** deferred from Phase 3 — planning doc only
**Selected option:** **Option B — pqxx connection injection** (per user preference)
**Current coverage:** 1.8% line / 0.4% branch (1398 lines in postgres_database.cpp + 430 in postgres_database_extensions.cpp = 1828 total)
**Target after refactor:** 70% line / 35% branch on postgres_database.cpp, 60% line on extensions
**Effort estimate:** 1–2 days (~500 lines of source refactor + ~1500 LOC of tests + extending MockPostgresDatabase)

## Why it can't be unit-tested today

`PostgresDatabase` directly owns `std::unique_ptr<pqxx::connection> connection_` and every method does:

```cpp
pqxx::work txn(*connection_);
auto result = txn.exec(query, pqxx::params{...});
// arrow conversion
txn.commit();
```

There is no seam to inject a fake. The class is the actual driver. To unit-test the implementation logic (validators, query string assembly, arrow conversion, error paths), pqxx must be replaced with an interface that tests can fake.

## The plan — `IPgConnection` interface

### 1. New header `include/trade_ngin/data/pg_connection_interface.hpp`

Define a minimal interface that exposes only the pqxx operations `postgres_database.cpp` actually uses. After greping the file, the operations are:
- Construct/connect with a connection string
- Begin a transaction (`pqxx::work`)
- `exec(query, params)` returning a result-set abstraction
- `commit()` / `abort()`
- `prepare(name, sql)` for prepared statements (used in batch insert helpers)
- Query the underlying connection state (`is_open()`)

Proposed interface (rough sketch — final API tightened during refactor):

```cpp
namespace trade_ngin::data {

/// Result row abstraction: column access by name and by index.
class IPgResultRow {
public:
    virtual ~IPgResultRow() = default;
    virtual std::string get_string(std::size_t col_index) const = 0;
    virtual std::string get_string(const std::string& col_name) const = 0;
    virtual double get_double(std::size_t col_index) const = 0;
    virtual int64_t get_int64(std::size_t col_index) const = 0;
    virtual bool is_null(std::size_t col_index) const = 0;
    virtual std::size_t size() const = 0;  // num columns
};

/// Result set abstraction.
class IPgResult {
public:
    virtual ~IPgResult() = default;
    virtual std::size_t row_count() const = 0;
    virtual std::size_t column_count() const = 0;
    virtual std::string column_name(std::size_t col_index) const = 0;
    virtual const IPgResultRow& row(std::size_t row_index) const = 0;
    /// Affected-rows count for INSERT/UPDATE/DELETE.
    virtual std::size_t affected_rows() const = 0;
};

/// Abstract transaction.
class IPgTransaction {
public:
    virtual ~IPgTransaction() = default;
    virtual std::unique_ptr<IPgResult> exec(
        const std::string& query,
        const std::vector<std::string>& string_params = {},
        const std::vector<double>& double_params = {}) = 0;
    /// Variadic-style helper for arbitrary param mixes; signature TBD during impl.
    virtual void commit() = 0;
    virtual void abort() = 0;
};

/// Abstract connection.
class IPgConnection {
public:
    virtual ~IPgConnection() = default;
    virtual bool is_open() const = 0;
    virtual std::unique_ptr<IPgTransaction> begin_transaction() = 0;
    virtual void prepare(const std::string& name, const std::string& sql) = 0;
};

/// Real implementation wrapping pqxx.
class PqxxConnection : public IPgConnection {
public:
    explicit PqxxConnection(const std::string& conn_str);
    bool is_open() const override;
    std::unique_ptr<IPgTransaction> begin_transaction() override;
    void prepare(const std::string& name, const std::string& sql) override;
private:
    pqxx::connection conn_;
};

}  // namespace trade_ngin::data
```

Real wrapper classes (`PqxxResult`, `PqxxResultRow`, `PqxxTransaction`) live in `src/data/pqxx_connection.cpp` and are thin pass-throughs over pqxx types.

### 2. Refactor `PostgresDatabase` to use the interface

```cpp
// Before:
class PostgresDatabase : public DatabaseInterface {
    std::unique_ptr<pqxx::connection> connection_;
    ...
};

// After:
class PostgresDatabase : public DatabaseInterface {
    std::unique_ptr<IPgConnection> connection_;
    ...
public:
    explicit PostgresDatabase(std::string connection_string);  // existing — internally constructs PqxxConnection
    /// New constructor for tests / dependency injection.
    PostgresDatabase(std::string connection_string,
                     std::unique_ptr<IPgConnection> conn);
};
```

Each method body changes from:
```cpp
pqxx::work txn(*connection_);
auto result = txn.exec(query, pqxx::params{a, b, c});
txn.commit();
```
to:
```cpp
auto txn = connection_->begin_transaction();
auto result = txn->exec(query, {a, b, c});
txn->commit();
```

This is mechanical — every `pqxx::work` becomes `auto txn = connection_->begin_transaction()` and result access uses `IPgResult` instead of `pqxx::result`.

### 3. Extend `MockPostgresDatabase` (under `tests/`)

Add a `FakePgConnection` in `tests/data/test_db_utils.hpp` that constructs scripted result sets. Tests can:

```cpp
auto conn = std::make_unique<FakePgConnection>();
conn->expect_query("SELECT * FROM trading.executions WHERE ...")
    .return_rows({{"order-1", "AAPL", 100.0}, ...});
auto db = std::make_shared<PostgresDatabase>("ignored", std::move(conn));
```

This unlocks unit-testing every method on PostgresDatabase that reads/writes the DB.

## Source change line counts

| File | Lines changed |
|---|---|
| `include/trade_ngin/data/pg_connection_interface.hpp` | +120 (new) |
| `include/trade_ngin/data/pqxx_connection.hpp` | +40 (new) |
| `src/data/pqxx_connection.cpp` | +250 (new — wrappers around pqxx) |
| `include/trade_ngin/data/postgres_database.hpp` | +5 (new ctor, type swap) |
| `src/data/postgres_database.cpp` | ~200 lines mechanically modified (every txn site) |
| `src/data/postgres_database_extensions.cpp` | ~80 lines mechanically modified |
| `src/data/CMakeLists.txt` | +1 (new source) |
| **Total** | **~700 lines added/changed** |

This is a real refactor, not a tweak. Plan for ~1–2 days, including:
- Pqxx wrappers correctly translate every pqxx::params shape used (string, double, int, timestamp, vectors, prepared statements)
- All existing unit + integration tests still pass
- Phase 0 backtest sentinel still produces identical output (no behavior drift)

## What gets unit-tested

After the refactor, the following PostgresDatabase methods become unit-testable via `FakePgConnection`:

| Method group | Approx LOC | Notes |
|---|---|---|
| `connect`, `disconnect`, `is_connected` | 30 | Lifecycle; trivial |
| `get_market_data`, `get_latest_prices`, `get_symbols`, `load_positions_by_date` | 220 | Query assembly + arrow conversion |
| `store_executions`, `store_positions`, `store_signals` | 150 | Insert + param binding |
| `store_backtest_*` family (8 methods) | 380 | Backtest result writers |
| `store_live_*` and `update_live_*` family | 240 | Live-trading writers |
| `delete_live_results`, `delete_live_equity_curve`, `delete_stale_executions` | 70 | Cleanup ops |
| `execute_query`, `execute_direct_query` | 80 | Generic query runner |
| 9 `validate_*` helpers (private, exposed via existing `#define private public`) | 150 | Pure validators (could be done today even without refactor — see "Quick win" below) |
| Error-path branches (connection lost, txn rollback) | 100 | FakePgConnection scripted to throw |

**Total unit-testable after refactor:** ~1400 of 1828 lines (postgres_database + extensions). **Realistic target: 70% line / 35% branch.**

## Quick win we can do BEFORE the full refactor

The 9 private `validate_*` methods (validate_table_name, validate_table_name_components, validate_symbol, validate_symbols, validate_strategy_id, validate_execution_report, validate_date_range, validate_connection) are pure logic with no pqxx dependency. Reachable today via `#define private public`.

- Effort: ~1 hour, ~200 LOC of tests
- Coverage on postgres_database.cpp: 1.8% → ~12% (+150 covered lines)
- Done as part of regular Phase 3 work, not the refactor

This delivers the validator surface immediately and reduces what's gated on the big refactor.

## Test file plan

- `tests/data/fake_pg_connection.hpp` — `FakePgConnection`, `FakeTransaction`, `FakeResult`, `FakeResultRow` (~300 LOC)
- `tests/data/test_postgres_database_lifecycle.cpp` — connect/disconnect/is_connected (~80 LOC)
- `tests/data/test_postgres_database_reads.cpp` — get_market_data, get_latest_prices, etc. (~400 LOC)
- `tests/data/test_postgres_database_writes.cpp` — store_*, update_*, delete_* (~600 LOC)
- `tests/data/test_postgres_database_validators.cpp` — pure validators via `#define private public` (~200 LOC; QUICK WIN, do first)
- `tests/data/test_postgres_database_extensions.cpp` — extensions file methods (~300 LOC)

## Risk + mitigation

**Risk:** the refactor touches every single DB query in the system. Even a tiny shape change in `IPgTransaction::exec()` parameter handling could subtly break production.

**Mitigation:**
- Land the refactor on a feature branch
- Run the Phase 0 backtest sentinel before AND after — if normalized diff is non-zero, the refactor introduced behavior drift
- Run all existing integration tests against a real test DB
- Code review on every changed query site

**Risk:** `pqxx::params{...}` supports a wide variety of types (string, int, double, timestamp, vector, optional). The interface needs to handle all the same shapes.

**Mitigation:**
- During implementation, scan `postgres_database.cpp` for every distinct `pqxx::params` shape and ensure `IPgTransaction::exec` covers it
- Type-tag variant or templates can model the param list

## Order of operations recommended

1. Quick win first — write `test_postgres_database_validators.cpp` (no source changes; ~12% coverage gain on the file)
2. Do the 13 `virtual` keyword changes already authorized → unlocks storage layer tests
3. Tests for storage layer using the existing MockPostgresDatabase
4. Schedule the IPgConnection refactor as a dedicated session — it deserves its own commit/PR sequence:
   - PR 1: introduce IPgConnection + PqxxConnection wrappers, leave PostgresDatabase unchanged
   - PR 2: switch PostgresDatabase internals to IPgConnection (mechanical, large diff)
   - PR 3: write tests via FakePgConnection
   - Sentinel must pass at end of PR 2 with zero diff against baseline

## Out of scope (explicitly not unit-tested even after refactor)

- The pqxx wrappers themselves (`PqxxConnection`, `PqxxTransaction`, `PqxxResult`) — those need integration tests against a real test DB, not unit tests. Their job is to faithfully translate to/from libpqxx; a fake substitute can't validate that.
- Connection-pool race conditions and reconnection retry behavior — better validated by stress/integration tests.
- Schema migrations — separate concern.
