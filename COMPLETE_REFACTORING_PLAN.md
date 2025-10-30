# Complete Refactoring Plan: trade-ngin Modularization

## PHASE 0 COMPLETED ✅ (2025-10-19)

### Achievements
Phase 0 has been successfully completed with all database operations refactored to eliminate raw SQL:

#### Files Created/Modified:
1. **Created**: `src/data/postgres_database_extensions.cpp` (520 lines)
   - Implements 11 new database methods to replace raw SQL
   - All methods use parameterized queries for safety
   - Proper error handling with Result<T> pattern

2. **Modified**: `include/trade_ngin/data/postgres_database.hpp`
   - Added 11 new method declarations for database operations

3. **Modified**: `apps/strategies/live_trend.cpp`
   - Line 876: Replaced DELETE query with `db->delete_stale_executions()`
   - Line 1428: Replaced UPDATE query with `db->update_live_equity_curve()`
   - Line 1737: Replaced DELETE query with `db->delete_live_results()`
   - Line 1777: Replaced INSERT query with `db->store_live_results_complete()`
   - Line 2068: Replaced DELETE query with `db->delete_live_equity_curve()`

4. **Modified**: `src/backtest/backtest_engine.cpp`
   - Line 1967: Replaced raw SQL INSERT with `db->store_backtest_summary()`
   - Line 2007: Replaced raw SQL INSERT with `db->store_backtest_equity_curve_batch()`
   - Maintained existing positions storage logic

#### Methods Implemented:
```cpp
// Execution management
delete_stale_executions() - Clean up same-day duplicates for re-runs

// Backtest storage
store_backtest_summary() - Store performance metrics
store_backtest_equity_curve_batch() - Batch insert equity curve points
store_backtest_positions() - Store final positions

// Live trading storage
update_live_results() - Update existing live results
update_live_equity_curve() - Update equity curve entry
delete_live_results() - Remove entries for re-runs
delete_live_equity_curve() - Remove equity curve entries
store_live_results_complete() - Complete live results storage
```

#### Verification Completed:
- ✅ Backtest run for one year period - Results identical
- ✅ Live trend run for October 18, 2025 - Results identical
- ✅ Both programs compile without errors
- ✅ Database operations function correctly
- ✅ delete_stale_executions() verified to only remove same-day duplicates

#### Important Notes:
- `delete_stale_executions()` is specifically for LIVE trading re-runs on the same day
- It preserves the complete audit trail - only removes duplicates when re-running for the same day
- All new methods follow existing patterns: validate_connection(), validate_table_name(), etc.
- Unit tests were attempted but removed due to data isolation issues - functionality verified through actual runs

---

## Executive Summary
This document outlines the complete refactoring plan to modularize the monolithic `live_trend.cpp` (2300+ lines) and improve `bt_trend.cpp` by creating reusable components that follow existing codebase patterns. The refactoring will be done incrementally without breaking functionality.

## Current State Analysis

### Problems Identified
1. **`live_trend.cpp`**: 2300+ lines of monolithic code in main()
2. **Raw SQL scattered throughout**: Direct SQL queries instead of using database abstraction
3. **Duplicated logic**: Same functionality implemented multiple times
4. **Storage fragmentation**: Both backtest and live use fragmented storage approaches
5. **Decoy flags**: `save_positions`, `save_signals`, `save_executions` are ignored
6. **Poor testability**: Cannot unit test individual components
7. **No reusability**: Other strategies cannot reuse the logic

### Database Schema Discovery
```
backtest.*: results, equity_curve, final_positions, executions, signals, run_metadata
trading.*: live_results, positions, executions, signals, equity_curve
```

## Complete Modular Architecture

### File Structure After Refactoring
```
trade-ngin/
├── include/trade_ngin/
│   ├── storage/                          # NEW: Unified storage layer
│   │   ├── results_manager_base.hpp      # Base class for all storage
│   │   ├── backtest_results_manager.hpp  # Backtest-specific storage
│   │   └── live_results_manager.hpp      # Live-specific storage
│   │
│   ├── live/                             # NEW: Live trading modules
│   │   ├── live_trading_manager.hpp      # Main orchestrator
│   │   ├── live_pnl_manager.hpp          # PnL calculations
│   │   ├── price_manager.hpp             # Price extraction/retrieval
│   │   ├── execution_manager.hpp         # Execution generation
│   │   ├── margin_manager.hpp            # Margin calculations
│   │   └── csv_exporter.hpp              # CSV file generation
│   │
│   └── data/
│       └── postgres_database.hpp         # EXTENDED with new methods
│
├── src/
│   ├── storage/                          # NEW implementations
│   │   ├── results_manager_base.cpp
│   │   ├── backtest_results_manager.cpp
│   │   └── live_results_manager.cpp
│   │
│   ├── live/                             # NEW implementations
│   │   ├── live_trading_manager.cpp
│   │   ├── live_pnl_manager.cpp
│   │   ├── price_manager.cpp
│   │   ├── execution_manager.cpp
│   │   ├── margin_manager.cpp
│   │   └── csv_exporter.cpp
│   │
│   ├── data/
│   │   └── postgres_database.cpp         # EXTENDED implementation
│   │
│   └── backtest/
│       └── backtest_engine.cpp           # REFACTORED to use BacktestResultsManager
│
└── apps/
    ├── strategies/
    │   └── live_trend.cpp                # REFACTORED: ~200 lines
    └── backtest/
        └── bt_trend.cpp                   # MINIMAL changes
```

## Implementation Phases (Fast-Track)

### Phase 0: Database Extensions ✅ COMPLETED (2025-10-19)
**Goal**: Extend PostgresDatabase to eliminate ALL raw SQL

**Status**: ✅ **SUCCESSFULLY COMPLETED**

**Files modified**:
- ✅ `include/trade_ngin/data/postgres_database.hpp` - Added 11 method declarations
- ✅ `src/data/postgres_database_extensions.cpp` - Created with 520 lines of implementation

**Methods implemented**:
```cpp
// Execution management
✅ Result<void> delete_stale_executions(const std::vector<std::string>& order_ids,
                                        const Timestamp& date,
                                        const std::string& table_name = "trading.executions");

// Backtest storage
✅ Result<void> store_backtest_summary(const std::string& run_id,
                                      const Timestamp& start_date,
                                      const Timestamp& end_date,
                                      const std::unordered_map<std::string, double>& metrics,
                                      const std::string& table_name = "backtest.results");

✅ Result<void> store_backtest_equity_curve_batch(const std::string& run_id,
                                                 const std::vector<std::pair<Timestamp, double>>& equity_points,
                                                 const std::string& table_name = "backtest.equity_curve");

✅ Result<void> store_backtest_positions(const std::vector<Position>& positions,
                                        const std::string& run_id,
                                        const std::string& table_name = "backtest.final_positions");

// Live trading storage
✅ Result<void> update_live_results(const std::string& strategy_id,
                                   const Timestamp& date,
                                   const std::unordered_map<std::string, double>& updates,
                                   const std::string& table_name = "trading.live_results");

✅ Result<void> update_live_equity_curve(const std::string& strategy_id,
                                        const Timestamp& date,
                                        double equity,
                                        const std::string& table_name = "trading.equity_curve");

✅ Result<void> delete_live_results(const std::string& strategy_id,
                                   const Timestamp& date,
                                   const std::string& table_name = "trading.live_results");

✅ Result<void> delete_live_equity_curve(const std::string& strategy_id,
                                        const Timestamp& date,
                                        const std::string& table_name = "trading.equity_curve");

✅ Result<void> store_live_results_complete(const std::string& strategy_id,
                                           const Timestamp& date,
                                           const std::unordered_map<std::string, double>& metrics,
                                           const std::unordered_map<std::string, int>& int_metrics,
                                           const nlohmann::json& config,
                                           const std::string& table_name = "trading.live_results");
```

**Testing**: ✅ Ran backtest for 1 year and live_trend for Oct 18 - identical results

---

### Phase 1: Storage Managers (Day 1 Afternoon)
**Goal**: Create unified storage architecture for both backtest and live

**New files to create**:
```
include/trade_ngin/storage/results_manager_base.hpp
include/trade_ngin/storage/backtest_results_manager.hpp
include/trade_ngin/storage/live_results_manager.hpp
src/storage/results_manager_base.cpp
src/storage/backtest_results_manager.cpp
src/storage/live_results_manager.cpp
```

**Key classes**:
```cpp
class ResultsManagerBase {
protected:
    std::shared_ptr<PostgresDatabase> db_;
    bool store_enabled_;  // Single control flag
    std::string schema_;  // "backtest" or "trading"
public:
    virtual Result<void> save_all_results(const std::string& run_id) = 0;
};

class BacktestResultsManager : public ResultsManagerBase {
    // Replaces BacktestEngine::save_results_to_db() fragmentation
};

class LiveResultsManager : public ResultsManagerBase {
    // New centralized storage for live trading
};
```

**Integration**:
1. Modify `BacktestEngine::save_results_to_db()` to use `BacktestResultsManager`
2. Keep old code behind feature flag for safety

---

### Phase 2: Price and PnL Management (Day 2 Morning)
**Goal**: Extract price retrieval and PnL calculation logic

**New files to create**:
```
include/trade_ngin/live/price_manager.hpp
include/trade_ngin/live/live_pnl_manager.hpp
src/live/price_manager.cpp
src/live/live_pnl_manager.cpp
```

**Extract from live_trend.cpp**:
- Lines 487-561: Price extraction logic → `PriceManager`
- Lines 565-683: PnL finalization → `LivePnLManager`
- Lines 1137-1594: Daily metrics calculation → `LivePnLManager`

**Key interfaces**:
```cpp
class PriceManager {
public:
    struct PriceSet {
        std::unordered_map<std::string, double> day_t1_close;  // Yesterday's close
        std::unordered_map<std::string, double> day_t2_close;  // Two days ago close
    };
    Result<PriceSet> extract_historical_prices(const std::vector<Bar>& bars, Timestamp target_date);
    Result<PriceSet> get_latest_prices(std::shared_ptr<PostgresDatabase> db,
                                       const std::vector<std::string>& symbols);
};

class LivePnLManager {
public:
    Result<PnLFinalization> finalize_previous_day(
        const std::unordered_map<std::string, Position>& positions,
        const std::unordered_map<std::string, double>& t2_prices,
        const std::unordered_map<std::string, double>& t1_prices,
        std::shared_ptr<TrendFollowingStrategy> strategy);

    Result<DailyMetrics> calculate_daily_metrics(
        double daily_pnl, double previous_portfolio_value,
        double initial_capital, int trading_days);
};
```

---

### Phase 3: Execution and Margin Management (Day 2 Afternoon)
**Goal**: Extract execution generation and margin calculations

**New files to create**:
```
include/trade_ngin/live/execution_manager.hpp
include/trade_ngin/live/margin_manager.hpp
src/live/execution_manager.cpp
src/live/margin_manager.cpp
```

**Extract from live_trend.cpp**:
- Lines 717-833: Execution generation → `ExecutionManager`
- Lines 915-1030: Margin calculations → `MarginManager`

**Key interfaces**:
```cpp
class ExecutionManager {
private:
    double commission_rate_;
    double slippage_model_;
public:
    Result<std::vector<ExecutionReport>> generate_daily_executions(
        const std::unordered_map<std::string, Position>& current_positions,
        const std::unordered_map<std::string, Position>& previous_positions,
        const std::unordered_map<std::string, double>& prices,
        Timestamp timestamp);
};

class MarginManager {
private:
    InstrumentRegistry& registry_;
public:
    struct MarginMetrics {
        double total_posted_margin;
        double maintenance_requirement;
        double equity_to_margin_ratio;
        double margin_cushion;
        double gross_notional;
        double net_notional;
    };
    Result<MarginMetrics> calculate_requirements(
        const std::unordered_map<std::string, Position>& positions,
        const std::unordered_map<std::string, double>& prices);
};
```

---

### Phase 4: CSV Export and Orchestration (Day 3 Morning)
**Goal**: Extract CSV generation and create main orchestrator

**New files to create**:
```
include/trade_ngin/live/csv_exporter.hpp
include/trade_ngin/live/live_trading_manager.hpp
src/live/csv_exporter.cpp
src/live/live_trading_manager.cpp
```

**Extract from live_trend.cpp**:
- Lines 1772-2043: CSV generation → `CSVExporter`
- Main orchestration logic → `LiveTradingManager`

**Key interfaces**:
```cpp
class CSVExporter {
public:
    Result<void> export_daily_positions(
        const std::string& filename,
        const std::unordered_map<std::string, Position>& positions,
        const std::unordered_map<std::string, double>& prices,
        const PortfolioMetrics& metrics);

    Result<void> export_finalized_positions(
        const std::string& filename,
        const std::unordered_map<std::string, Position>& positions,
        const std::unordered_map<std::string, double>& entry_prices,
        const std::unordered_map<std::string, double>& exit_prices);
};

class LiveTradingManager {
private:
    // All managers as members
    std::shared_ptr<PostgresDatabase> db_;
    std::shared_ptr<TrendFollowingStrategy> strategy_;
    std::shared_ptr<PortfolioManager> portfolio_;
    std::shared_ptr<RiskManager> risk_manager_;
    std::unique_ptr<PriceManager> price_manager_;
    std::unique_ptr<LivePnLManager> pnl_manager_;
    std::unique_ptr<ExecutionManager> execution_manager_;
    std::unique_ptr<MarginManager> margin_manager_;
    std::unique_ptr<LiveResultsManager> results_manager_;
    std::unique_ptr<CSVExporter> csv_exporter_;
    std::shared_ptr<EmailSender> email_sender_;

public:
    Result<LiveTradingResults> run_daily_processing(Timestamp target_date);
};
```

---

### Phase 5: Integration and Testing (Day 3 Afternoon)
**Goal**: Integrate all components and refactor main files

**Files to modify**:
- `apps/strategies/live_trend.cpp` - Reduce from 2300 to ~200 lines
- `src/backtest/backtest_engine.cpp` - Use BacktestResultsManager

**New live_trend.cpp structure**:
```cpp
int main(int argc, char* argv[]) {
    // 1. Parse arguments (20 lines)
    auto [target_date, send_email] = parse_arguments(argc, argv);

    // 2. Initialize infrastructure (30 lines)
    Logger::instance().initialize(config);
    auto credentials = std::make_shared<CredentialStore>("./config.json");
    DatabasePool::instance().initialize(conn_string, 5);
    InstrumentRegistry::instance().initialize(db);

    // 3. Create configurations (40 lines)
    LiveTradingConfig config;
    RiskConfig risk_config{...};
    PortfolioConfig portfolio_config{...};
    TrendFollowingConfig trend_config{...};

    // 4. Create strategy and portfolio (20 lines)
    auto strategy = std::make_shared<TrendFollowingStrategy>(...);
    auto portfolio = std::make_shared<PortfolioManager>(portfolio_config);

    // 5. Create live trading manager (10 lines)
    auto live_manager = std::make_unique<LiveTradingManager>(
        db, strategy, portfolio, credentials, config);

    // 6. Run processing (20 lines)
    auto results = live_manager->run_daily_processing(target_date);
    if (results.is_error()) {
        ERROR("Processing failed: " + results.error()->what());
        return 1;
    }

    // 7. Store results (10 lines)
    auto results_manager = std::make_unique<LiveResultsManager>(db, config.store_enabled);
    results_manager->set_results(results.value());
    auto save_result = results_manager->save_all_results(generate_run_id());

    // 8. Send email if needed (20 lines)
    if (send_email) {
        email_sender->send_daily_report(results.value());
    }

    return 0;
}
```

---

## Testing Strategy

### Unit Testing (Concurrent with Development)
Each new component gets unit tests:
```cpp
tests/live/
├── test_price_manager.cpp
├── test_live_pnl_manager.cpp
├── test_execution_manager.cpp
├── test_margin_manager.cpp
├── test_csv_exporter.cpp
└── test_live_results_manager.cpp
```

### Integration Testing (End of Each Phase)
1. **Database operations**: Compare database writes before/after
2. **PnL calculations**: Verify identical results
3. **Position files**: Diff CSV outputs
4. **Email content**: Compare generated emails

### Regression Testing (Phase 5)
```bash
# Run both versions in parallel
./live_trend_original 2025-01-01 > original.log
./live_trend_refactored 2025-01-01 > refactored.log

# Compare outputs
diff original.log refactored.log

# Compare database
psql -c "SELECT * FROM trading.positions WHERE date='2025-01-01'" > original_db.txt
psql -c "SELECT * FROM trading.positions WHERE date='2025-01-01'" > refactored_db.txt
diff original_db.txt refactored_db.txt
```

---

## Migration Strategy

### Feature Flag Approach
```cpp
// config.json
{
    "refactoring": {
        "use_new_storage": false,      // Phase 1
        "use_price_manager": false,    // Phase 2
        "use_pnl_manager": false,      // Phase 2
        "use_execution_manager": false,// Phase 3
        "use_margin_manager": false,   // Phase 3
        "use_live_manager": false      // Phase 4
    }
}
```

### Gradual Rollout
1. **Day 1**: Enable new storage managers in test environment
2. **Day 2**: Enable price/PnL managers
3. **Day 3**: Enable execution/margin managers
4. **Day 4**: Full switch to new architecture
5. **Day 5**: Remove old code

---

## Benefits Achieved

1. **Modularity**: Each component has single responsibility
2. **Reusability**: Other strategies can use these managers
3. **Testability**: Each component can be unit tested
4. **Maintainability**: Changes localized to specific modules
5. **No Raw SQL**: All database operations through proper methods
6. **Unified Architecture**: Backtest and live use same patterns
7. **Clear Control**: Single `store_enabled` flag instead of decoy flags
8. **Reduced Complexity**: live_trend.cpp from 2300 to ~200 lines

---

## Risk Mitigation

1. **Feature flags**: Can rollback instantly
2. **Parallel running**: Old and new code side-by-side
3. **Incremental changes**: Small, testable steps
4. **Database validation**: Automated comparison of outputs
5. **Existing tests**: All existing tests must pass
6. **Monitoring**: Log all differences during parallel run

---

## Success Metrics

- [ ] Zero functional changes (identical output)
- [ ] All unit tests passing
- [ ] Database writes identical
- [ ] CSV outputs identical
- [ ] Email content identical
- [ ] Performance within 5% of original
- [ ] Code coverage > 80% for new modules
- [ ] live_trend.cpp < 250 lines
- [ ] No raw SQL in codebase

---

## Next Steps

1. **Review and approve this plan**
2. **Create feature branch**: `feature/modularization`
3. **Start Phase 0**: Database extensions
4. **Daily progress reviews**
5. **Merge when all success metrics met**

---

## Appendix: Detailed Line Mapping

### live_trend.cpp Line-to-Module Mapping
```
Lines 1-86:      → Stays in main() (initialization)
Lines 87-126:    → Stays in main() (credentials)
Lines 127-147:   → Stays in main() (database pool)
Lines 150-285:   → Stays in main() (instrument registry)
Lines 288-336:   → Stays in main() (configuration)
Lines 350-441:   → Stays in main() (strategy/portfolio creation)
Lines 395-430:   → LiveTradingManager (market data loading)
Lines 447-465:   → PostgresDatabase::load_positions_by_date()
Lines 487-561:   → PriceManager
Lines 565-683:   → LivePnLManager::finalize_previous_day()
Lines 684-711:   → LiveTradingManager (position updates)
Lines 717-833:   → ExecutionManager
Lines 836-899:   → PostgresDatabase::delete_stale_executions() + store_executions()
Lines 900-1030:  → MarginManager
Lines 1031-1104: → LiveResultsManager::store_positions()
Lines 1106-1135: → Stays (uses existing RiskManager)
Lines 1137-1594: → LivePnLManager::calculate_daily_metrics()
Lines 1616-1648: → LiveResultsManager::store_signals()
Lines 1650-1770: → LiveResultsManager::store_live_results()
Lines 1772-2043: → CSVExporter
Lines 2044-2075: → LiveResultsManager::store_equity_curve()
Lines 2097-2307: → Stays (uses existing EmailSender)
```

### BacktestEngine Line-to-Module Mapping
```
Lines 1860-1958: → BacktestResultsManager::store_summary_results()
Lines 1986-2010: → BacktestResultsManager::store_equity_curve()
Lines 2018-2076: → BacktestResultsManager::store_final_positions()
Lines 2083-2092: → BacktestResultsManager::store_executions()
Lines 2107-2127: → BacktestResultsManager::store_signals()
Lines 2094-2101: → BacktestResultsManager::store_metadata()
```

---

## Implementation Checklist

### Day 1
- [x] ~~Create feature branch~~ (Working directly on main-hd)
- [x] Extend PostgresDatabase with new methods ✅ COMPLETED
- [x] Test database operations ✅ COMPLETED
- [ ] Create ResultsManagerBase
- [ ] Create BacktestResultsManager
- [ ] Create LiveResultsManager

### Day 2
- [ ] Create PriceManager
- [ ] Create LivePnLManager
- [ ] Create ExecutionManager
- [ ] Create MarginManager
- [ ] Write unit tests
- [ ] Integration test Phase 2

### Day 3
- [ ] Create CSVExporter
- [ ] Create LiveTradingManager
- [ ] Refactor live_trend.cpp
- [ ] Refactor BacktestEngine
- [ ] Full regression testing
- [ ] Performance testing

### Day 4
- [ ] Fix any issues found
- [ ] Documentation update
- [ ] Code review
- [ ] Merge preparation

---

*Document Version: 1.0*
*Created: 2025-01-20*
*Target Completion: 3-4 days*