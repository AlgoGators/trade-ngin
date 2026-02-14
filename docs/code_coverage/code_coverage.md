# Code Coverage

This project supports building and measuring C++ code coverage using `lcov` and `genhtml` (GCC/Clang only).

## Prerequisites

- GCC or Clang
- [lcov](https://github.com/linux-test-project/lcov) (and `genhtml`, usually bundled with lcov)

Install on macOS (Homebrew):

```bash
brew install lcov
```

## Building with coverage enabled

From the repo root:

```bash
mkdir -p build_coverage
cd build_coverage
cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON ..
make
```

Use a separate directory (e.g. `build_coverage/`) so coverage builds don't mix with normal `build/` output.

## Generating the coverage report

From inside `build_coverage/`:

```bash
make coverage
```

This will:

1. Reset coverage counters
2. Run the unit tests
3. Capture coverage data and produce `coverage.info.cleaned`
4. Generate an HTML report in `build_coverage/coverage/`
5. Print a short summary to the terminal

Open the report in a browser, e.g.:

```bash
open coverage/index.html   # macOS
```

## Notes

- Coverage is only reliable with a **Debug** build; the CMake module will warn if `ENABLE_COVERAGE` is used with another build type.
- System headers, test code, and gtest are excluded from the report.
- Branch coverage is enabled in the report when supported.

---

## Project Structure for Tests

Tests live under `tests/` and mirror the source tree:

```
tests/
├── CMakeLists.txt          # registers all test source files
├── backtesting/            # backtest module tests
│   └── test_transaction_cost_analysis.cpp
├── core/                   # core utilities tests
│   ├── test_result.cpp
│   ├── test_state_manager.cpp
│   ├── test_logger.cpp
│   ├── test_config_manager.cpp
│   ├── test_config_version.cpp
│   └── test_config_base.cpp
├── data/                   # data layer tests
│   ├── test_db_utils.cpp
│   ├── test_postgres_database.cpp
│   ├── test_database_pooling.cpp
│   ├── test_market_data_bus.cpp
│   └── test_credential_store.cpp
├── execution/
│   └── test_execution_engine.cpp
├── optimization/
│   └── test_dynamic_optimizer.cpp
├── order/
│   ├── test_order_manager.cpp
│   └── test_utils.cpp
├── portfolio/
│   └── test_portfolio_manager.cpp
├── risk/
│   └── test_risk_manager.cpp
├── statistics/
│   └── test_statistics_tools.cpp
└── strategy/
    ├── test_base_strategy.cpp
    └── test_trend_following.cpp
```

### Modules with NO test file yet

| Module | Source dir | What to create |
|--------|-----------|----------------|
| **Instruments** | `src/instruments/` | `tests/instruments/test_instrument_registry.cpp` |
| **Storage** | `src/storage/` | `tests/storage/test_results_manager.cpp` |
| **Live** | `src/live/` | `tests/live/test_csv_exporter.cpp`, etc. |
| **Backtest (most files)** | `src/backtest/` | `tests/backtesting/test_backtest_coordinator.cpp`, etc. |

---

## How to Add Tests for a New Module

### Step 1 — Create the test file

Create a file under `tests/<module>/test_<component>.cpp`.  Use Google Test:

```cpp
#include <gtest/gtest.h>
#include "trade_ngin/<module>/<component>.hpp"

class MyComponentTest : public ::testing::Test {
protected:
    void SetUp() override {
        // shared setup
    }
};

TEST_F(MyComponentTest, BasicBehavior) {
    // Arrange
    // Act
    // Assert
    EXPECT_EQ(expected, actual);
}
```

### Step 2 — Register in CMakeLists.txt

Open `tests/CMakeLists.txt` and add your file to `TEST_SOURCES`:

```cmake
set(TEST_SOURCES
    # ... existing files ...
    <module>/test_<component>.cpp   # ← add here
)
```

That's it — no other CMake changes are needed. The test executable and Google Test discovery are already configured.

### Step 3 — Build and run

```bash
cd build_coverage
cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON ..
make
./bin/Debug/trade_ngin_tests --gtest_filter="MyComponentTest.*"
```

### Step 4 — Generate coverage to verify

```bash
make coverage
open coverage/index.html
```

---

## Test Conventions

1. **File naming**: `test_<source_basename>.cpp` (e.g. `test_order_manager.cpp` tests `order_manager.cpp`)
2. **Test fixture naming**: `<ClassName>Test` (e.g. `OrderManagerTest`)
3. **Compile definitions**: Tests compile with `TESTING` and `USE_DIRECT_DB_CONNECTION` defined — use `#ifdef TESTING` guards in production code when needed
4. **Include paths**: Tests include headers via `trade_ngin/<module>/<header>.hpp`
5. **No database in unit tests**: Use mocks or stubs. If a test needs a real DB connection, guard it with `GTEST_SKIP()` when the DB is unavailable
6. **Log suppression**: You may see `WARNING: Logger not initialized` during tests — this is expected and harmless

---

## Filtering External Libraries from Coverage

The default report includes coverage for Eigen, nlohmann/json, and pqxx headers. To get project-only numbers:

```bash
cd build_coverage

# Extract only project files
lcov --extract coverage.info.cleaned '*/tradeengine*' \
     --output-file project_only.info \
     --rc lcov_branch_coverage=1 \
     --ignore-errors inconsistent,unsupported,format,count,unused

# Print project-only summary
lcov --summary project_only.info \
     --rc lcov_branch_coverage=1 \
     --ignore-errors inconsistent,unsupported,format,count
```

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `lcov not found` | `brew install lcov` |
| `genhtml: ERROR: category` | Already handled — `cmake/CodeCoverage.cmake` passes `--ignore-errors category` |
| Very low coverage numbers | Check whether external library headers are inflating totals — use the project-only filter above |
| `WARNING: Logger not initialized` | Harmless — Logger singleton isn't set up in test harness |
| Tests fail to link | Rebuild from scratch: `rm -rf build_coverage && mkdir build_coverage && cd build_coverage && cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON .. && make` |

---

## Detailed Analysis

For a full module-by-module coverage breakdown with per-file tables and improvement priorities, see:

📊 **[Coverage Analysis](coverage_analysis.md)**
