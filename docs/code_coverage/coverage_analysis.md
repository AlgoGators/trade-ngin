# Code Coverage Analysis — Module-by-Module Breakdown

**Generated**: 2026-02-13  
**Tool**: lcov + genhtml (Apple Clang 15, Debug, `--coverage`)

---

## Overall Project Summary (Excluding External Libraries)

| Metric     | Covered  | Total   | Coverage |
|------------|----------|---------|----------|
| Lines      | 4,503    | 19,599  | **23.0%** |
| Functions  | 1,042    | 2,264   | **46.0%** |
| Branches   | 4,184    | 50,217  | **8.3%**  |

> [!NOTE]
> These numbers exclude Eigen, nlohmann/json, and pqxx headers. The overall `lcov` report (including those libs) shows 36.2% line / 53.6% function / 11.8% branch.

---

## Module-by-Module Breakdown

### 1. Core (`src/core/` + `include/trade_ngin/core/`)

**Test files**: `test_result.cpp`, `test_state_manager.cpp`, `test_logger.cpp`, `test_config_manager.cpp`, `test_config_version.cpp`, `test_config_base.cpp`

| File | Lines | Line % | Funcs | Func % | Branches | Branch % |
|------|-------|--------|-------|--------|----------|----------|
| `config_base.cpp` | 27 | **74.1%** | 2 | 100% | 66 | 31.8% |
| `config_manager.cpp` | 443 | **45.6%** | 25 | 64.0% | 1,140 | 23.1% |
| `config_version.cpp` | 159 | **69.8%** | 9 | 100% | 385 | 35.8% |
| `logger.cpp` | 120 | **83.3%** | 14 | 85.7% | 182 | 47.8% |
| `state_manager.cpp` | 101 | **53.5%** | 9 | 55.6% | 162 | 30.9% |
| `config_loader.cpp` | 209 | **0.0%** | 7 | 0.0% | 592 | 0.0% |
| `chart_generator.cpp` | 1,230 | **0.1%** | 29 | 17.2% | 4,086 | 0.0% |
| `email_sender.cpp` | 2,226 | **0.1%** | 78 | 6.4% | 5,591 | 0.0% |
| `run_id_generator.cpp` | 50 | **0.0%** | 8 | 0.0% | 82 | 0.0% |
| Headers (error.hpp, types.hpp, etc.) | ~180 | varies | varies | varies | varies | varies |

**Assessment**: Config management and logging are well-covered. `chart_generator.cpp` (1,230 lines) and `email_sender.cpp` (2,226 lines) are the two largest files in the module with **near-zero coverage**. `config_loader.cpp` and `run_id_generator.cpp` also have 0% coverage.

**Priority gaps**:
- `config_loader.cpp` — loads configuration from disk; error paths completely untested
- `run_id_generator.cpp` — generates unique IDs; no coverage
- `chart_generator.cpp` — very large file, likely requires mock/stub for gnuplot
- `email_sender.cpp` — depends on external SMTP; needs mocking

---

### 2. Data (`src/data/` + `include/trade_ngin/data/`)

**Test files**: `test_db_utils.cpp`, `test_postgres_database.cpp`, `test_database_pooling.cpp`, `test_market_data_bus.cpp`, `test_credential_store.cpp`

| File | Lines | Line % | Funcs | Func % | Branches | Branch % |
|------|-------|--------|-------|--------|----------|----------|
| `market_data_bus.cpp` | 49 | **89.8%** | 4 | 100% | 178 | 46.6% |
| `credential_store.cpp` | 212 | **25.0%** | 14 | 35.7% | 540 | 13.0% |
| `database_pooling.cpp` | 90 | **1.1%** | 8 | 62.5% | 392 | 0.0% |
| `postgres_database.cpp` | 1,398 | **1.8%** | 60 | 23.3% | 4,777 | 0.4% |
| `postgres_database_extensions.cpp` | 430 | **0.2%** | 15 | 33.3% | 1,924 | 0.0% |
| `conversion_utils.cpp` | 110 | **0.0%** | 4 | 0.0% | 336 | 0.0% |

**Assessment**: `market_data_bus` is the lone bright spot at ~90% line coverage. The database layer (`postgres_database.cpp` — 1,398 lines) has **<2% line coverage**, which is the single largest coverage gap by absolute line count in the data module. Tests exist but are likely guarded behind DB connection availability.

**Priority gaps**:
- `postgres_database.cpp` — core DB access; 1,398 lines at 1.8%
- `conversion_utils.cpp` — 0% coverage, data processing utility
- `database_pooling.cpp` — connection pooling logic barely tested

---

### 3. Strategy (`src/strategy/` + `include/trade_ngin/strategy/`)

**Test files**: `test_base_strategy.cpp`, `test_trend_following.cpp`

| File | Lines | Line % | Funcs | Func % | Branches | Branch % |
|------|-------|--------|-------|--------|----------|----------|
| `base_strategy.cpp` | 324 | **76.9%** | 34 | 88.2% | 692 | 36.7% |
| `trend_following.cpp` | 860 | **65.6%** | 28 | 85.7% | 2,617 | 22.8% |
| `trend_following_fast.cpp` | 860 | **0.1%** | 28 | 17.9% | 2,617 | 0.0% |
| `trend_following_slow.cpp` | 854 | **0.1%** | 27 | 18.5% | 2,615 | 0.0% |
| `regime_detector.cpp` | 285 | **0.0%** | 17 | 0.0% | 417 | 0.0% |

**Assessment**: The base strategy and the primary `trend_following` strategy have reasonable coverage. However, **`trend_following_fast.cpp` and `trend_following_slow.cpp` are near-identical 860-line variants with 0% coverage**. `regime_detector.cpp` (285 lines) has no tests at all.

**Priority gaps**:
- `trend_following_fast.cpp` — 860 lines, 0.1% coverage; needs dedicated test file
- `trend_following_slow.cpp` — 854 lines, 0.1% coverage; needs dedicated test file
- `regime_detector.cpp` — 285 lines, 0% coverage; new test file required

---

### 4. Backtest (`src/backtest/` + `include/trade_ngin/backtest/`)

**Test files**: `test_transaction_cost_analysis.cpp`

| File | Lines | Line % | Funcs | Func % | Branches | Branch % |
|------|-------|--------|-------|--------|----------|----------|
| `transaction_cost_analysis.cpp` | 241 | **75.5%** | 16 | 93.8% | 328 | 42.4% |
| `backtest_coordinator.cpp` | 574 | **0.2%** | 26 | 19.2% | 1,526 | 0.0% |
| `backtest_data_loader.cpp` | 149 | **0.7%** | 15 | 33.3% | 368 | 0.0% |
| `backtest_pnl_manager.cpp` | 262 | **0.4%** | 21 | 23.8% | 490 | 0.0% |
| `backtest_metrics_calculator.cpp` | 331 | **0.0%** | 23 | 0.0% | 344 | 0.0% |
| `backtest_portfolio_constraints.cpp` | 177 | **0.0%** | 12 | 0.0% | 347 | 0.0% |
| `backtest_price_manager.cpp` | 75 | **0.0%** | 10 | 0.0% | 44 | 0.0% |
| `backtest_execution_manager.cpp` | 64 | **0.0%** | 10 | 0.0% | 72 | 0.0% |
| `slippage_model.cpp` | 64 | **0.0%** | 10 | 0.0% | 28 | 0.0% |

**Assessment**: Only `transaction_cost_analysis.cpp` has meaningful coverage (75.5%). **Every other backtest file is at or near 0%**. The `backtest_coordinator.cpp` (574 lines) is the orchestrator and has effectively no coverage. The deprecated `test_engine.cpp` was removed but nothing replaced it.

**Priority gaps**:
- `backtest_coordinator.cpp` — 574 lines, central orchestrator, ~0% coverage
- `backtest_metrics_calculator.cpp` — 331 lines, all metrics calculations untested
- `backtest_pnl_manager.cpp` — 262 lines, PnL calculations at ~0%
- `backtest_portfolio_constraints.cpp` — 177 lines, constraint logic untested
- `slippage_model.cpp` — slippage model calculations completely untested

---

### 5. Execution (`src/execution/`)

**Test files**: `test_execution_engine.cpp`

| File | Lines | Line % | Funcs | Func % | Branches | Branch % |
|------|-------|--------|-------|--------|----------|----------|
| `execution_engine.cpp` | 609 | **58.9%** | 30 | 86.7% | 1,590 | 27.5% |

**Assessment**: Decent coverage — 58.9% line, 86.7% function. Branch coverage at 27.5% suggests many error/edge-case paths are not exercised. The test suite includes stress tests and metrics accuracy tests.

**Priority gaps**:
- Improve branch coverage (error handling paths, edge cases)

---

### 6. Order (`src/order/`)

**Test files**: `test_order_manager.cpp`, `test_utils.cpp`

| File | Lines | Line % | Funcs | Func % | Branches | Branch % |
|------|-------|--------|-------|--------|----------|----------|
| `order_manager.cpp` | 142 | **85.9%** | 12 | 100% | 212 | 45.3% |

**Assessment**: **Strong coverage** — 85.9% line, 100% function. Branch coverage could be improved (45.3%), but this module is in good shape.

---

### 7. Risk (`src/risk/`)

**Test files**: `test_risk_manager.cpp`

| File | Lines | Line % | Funcs | Func % | Branches | Branch % |
|------|-------|--------|-------|--------|----------|----------|
| `risk_manager.cpp` | 365 | **64.4%** | 18 | 72.2% | 596 | 32.9% |

**Assessment**: Moderate coverage. 64.4% line coverage is reasonable but leaves ~130 lines untested. Branch coverage at 32.9% indicates many conditional paths (risk limits, thresholds, alert conditions) are not exercised.

**Priority gaps**:
- Test edge-case risk scenarios (limit breaches, extreme inputs)

---

### 8. Optimization (`src/optimization/`)

**Test files**: `test_dynamic_optimizer.cpp`

| File | Lines | Line % | Funcs | Func % | Branches | Branch % |
|------|-------|--------|-------|--------|----------|----------|
| `dynamic_optimizer.cpp` | 185 | **65.4%** | 10 | 80.0% | 370 | 26.8% |

**Assessment**: Decent line coverage (65.4%) but low branch coverage (26.8%). The optimizer has multiple code paths (convergence conditions, corner cases) that need more test variety.

---

### 9. Portfolio (`src/portfolio/`)

**Test files**: `test_portfolio_manager.cpp`

| File | Lines | Line % | Funcs | Func % | Branches | Branch % |
|------|-------|--------|-------|--------|----------|----------|
| `portfolio_manager.cpp` | 843 | **33.2%** | 29 | 48.3% | 3,231 | 15.4% |

**Assessment**: Large file (843 lines) with only **33.2% line coverage**. Fewer than half of the functions are tested. This is a critical module that handles portfolio-level logic including position management, execution generation, and rebalancing.

**Priority gaps**:
- Multi-strategy rebalancing logic
- Position tracking and reconciliation
- Execution generation pathways

---

### 10. Statistics (`src/statistics/`)

**Test files**: `test_statistics_tools.cpp`

| File | Lines | Line % | Funcs | Func % | Branches | Branch % |
|------|-------|--------|-------|--------|----------|----------|
| `statistics_tools.cpp` | 859 | **81.7%** | 59 | 93.2% | 1,467 | 45.3% |

**Assessment**: **Strongest coverage in the entire project** — 81.7% line, 93.2% function. Good branch coverage at 45.3%. This module is a model for what other modules should aim for.

---

### 11. Transaction Cost (`src/transaction_cost/`)

**No dedicated test file** (tested indirectly via `test_transaction_cost_analysis.cpp` in backtesting)

| File | Lines | Line % | Funcs | Func % | Branches | Branch % |
|------|-------|--------|-------|--------|----------|----------|
| `asset_cost_config.cpp` | 445 | **96.9%** | 7 | 71.4% | 266 | 47.0% |
| `transaction_cost_manager.cpp` | 51 | **56.9%** | 10 | 40.0% | 20 | 40.0% |
| `impact_model.cpp` | 49 | **46.9%** | 9 | 55.6% | 16 | 31.2% |
| `spread_model.cpp` | 55 | **21.8%** | 10 | 40.0% | 16 | 12.5% |

**Assessment**: `asset_cost_config.cpp` has excellent coverage (96.9% line). However, the actual cost calculation models (`impact_model`, `spread_model`) have low coverage. These are small files but critical for correctness.

**Priority gaps**:
- `spread_model.cpp` — 21.8% line coverage; spread calculations underexercised
- `impact_model.cpp` — 46.9%; market impact calcs need more test cases

---

### 12. Instruments (`src/instruments/`)

**No test files**

| File | Lines | Line % | Funcs | Func % | Branches | Branch % |
|------|-------|--------|-------|--------|----------|----------|
| `instrument_registry.cpp` | 241 | **13.3%** | 17 | 41.2% | 780 | 5.0% |
| `futures.cpp` | 56 | **3.6%** | 10 | 20.0% | 62 | 0.0% |
| `equity.cpp` | 39 | **0.0%** | 9 | 0.0% | 50 | 0.0% |
| `option.cpp` | 111 | **0.0%** | 16 | 0.0% | 72 | 0.0% |

**Assessment**: **No test file exists for this module**. Coverage is negligible, coming only from indirect usage. This module contains instrument abstractions used across the system.

**Priority gaps**:
- Entire module needs a test file (`tests/instruments/test_instrument_registry.cpp`)
- Contract specifications, margin calculations, point value lookups need testing

---

### 13. Storage (`src/storage/`)

**No test files**

| File | Lines | Line % | Funcs | Func % | Branches | Branch % |
|------|-------|--------|-------|--------|----------|----------|
| `backtest_results_manager.cpp` | 197 | **0.5%** | 17 | 29.4% | 930 | 0.0% |
| `live_results_manager.cpp` | 163 | **0.6%** | 17 | 29.4% | 703 | 0.0% |
| `results_manager_base.cpp` | 80 | **1.2%** | 10 | 50.0% | 288 | 0.0% |

**Assessment**: **No test file exists for this module**. All three files are at ~0-1% coverage. This module persists results to the database and is critical for data integrity.

**Priority gaps**:
- All 3 files need test coverage
- DB mocking/stubbing required for unit tests

---

### 14. Live (`src/live/`)

**No test files**

| File | Lines | Line % | Funcs | Func % | Branches | Branch % |
|------|-------|--------|-------|--------|----------|----------|
| `live_data_loader.cpp` | 448 | **0.2%** | 22 | 22.7% | 1,630 | 0.0% |
| `csv_exporter.cpp` | 339 | **0.3%** | 17 | 29.4% | 1,092 | 0.0% |
| `live_pnl_manager.cpp` | 203 | **0.5%** | 12 | 41.7% | 654 | 0.0% |
| `live_trading_coordinator.cpp` | 176 | **0.6%** | 18 | 27.8% | 250 | 0.0% |
| `margin_manager.cpp` | 176 | **0.6%** | 15 | 33.3% | 488 | 0.0% |
| `live_metrics_calculator.cpp` | 198 | **0.0%** | 23 | 0.0% | 84 | 0.0% |
| `live_price_manager.cpp` | 100 | **1.0%** | 15 | 33.3% | 374 | 0.0% |
| `execution_manager.cpp` | 92 | **0.0%** | 5 | 0.0% | 400 | 0.0% |

**Assessment**: **No test file exists for this module**. All 8 files are at ~0% coverage. This is the largest untested module by file count. The live system handles real trading coordination, PnL calculation, data loading, and margin management.

**Priority gaps**:
- Entire module needs test infrastructure
- `csv_exporter.cpp` — easiest to test (no DB/network dependencies)
- `margin_manager.cpp` — pure calculation logic, testable

---

## Coverage Improvement Priority Matrix

Ranked by **impact × feasibility** (higher = more urgent):

| Priority | Module | Files | Reason |
|----------|--------|-------|--------|
| 🔴 **P0** | **Backtest** | coordinator, metrics_calculator, pnl_manager | Core engine, 0% coverage, pure logic, highly testable |
| 🔴 **P0** | **Strategy** | trend_following_fast, trend_following_slow, regime_detector | ~1,700 lines at 0%, strategy variants need parity |
| 🟠 **P1** | **Portfolio** | portfolio_manager | 843 lines at 33.2%, critical rebalancing logic |
| 🟠 **P1** | **Instruments** | All 4 files | No test file at all; used across every module |
| 🟡 **P2** | **Live** | csv_exporter, margin_manager | Easiest live files to test (no external deps) |
| 🟡 **P2** | **Storage** | All 3 files | DB persistence layer; needs mock infrastructure |
| 🟡 **P2** | **Transaction Cost** | spread_model, impact_model | Small files, critical for correctness |
| 🔵 **P3** | **Data** | postgres_database, conversion_utils | Large but requires DB mock to test meaningfully |
| 🔵 **P3** | **Core** | config_loader, chart_generator, email_sender | External dependencies (filesystem, gnuplot, SMTP) |

---

## Key Observations

1. **3 modules have zero test files**: Instruments, Storage, Live
2. **Backtest module has 8 files at ~0% coverage** despite being the core simulation engine; the deprecated `test_engine.cpp` was removed but not replaced
3. **Strategy variants** (`trend_following_fast`, `trend_following_slow`) share ~860 lines each with the base `trend_following` but have no dedicated tests
4. **Statistics is the gold standard** at 81.7% line / 93.2% function — other modules should emulate its test structure
5. **Branch coverage is universally low** (8.3% project-wide), indicating error paths, edge cases, and conditional logic are broadly untested
6. **External library noise**: Eigen, nlohmann/json, and pqxx headers inflate the 272-file report to 36.2% overall; project-only coverage is actually 23.0%
