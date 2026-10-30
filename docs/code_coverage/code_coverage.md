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
cmake -S . -B build_coverage -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
cmake --build build_coverage
```

Use a separate directory (e.g. `build_coverage/`) so coverage builds don't mix with normal `build/` output.

## Generating the coverage report

From the repo root:

```bash
cmake --build build_coverage --target coverage
```

This will:

1. Reset coverage counters
2. Run the full CTest suite serially, with Google Test cases isolated
3. Capture coverage data and extract only this checkout's `src/` into `coverage.info.cleaned`
4. Generate an HTML report in `build_coverage/coverage/`
5. Print a short summary to the terminal

Open the report in a browser, e.g.:

```bash
open build_coverage/coverage/index.html   # macOS
```

For the weekly baseline, run one command after building:

```bash
./scripts/coverage_baseline.sh build_coverage
```

The helper runs the `coverage` target to reset counters, run the full CTest suite serially with temporary files under `build_coverage/coverage-tmp/`, and capture the `trade_ngin` library's coverage once. It then prints line-coverage summaries for all repository `src/` files and the in-scope files, followed by a per-file list. It writes `build_coverage/cov_full.info`, `cov_src.info`, and `cov_inscope.info`. The in-scope report removes files matching `*chart_generator*`, `*email_sender*`, `*postgres_database*`, `*live_trading_coordinator*`, and `*execution_engine*`. These are five patterns, currently matching six source files because `*postgres_database*` matches two. Pass another build directory as the first argument if needed. When lcov is outside `PATH`, configure with `-DLCOV_PATH=/path/to/lcov -DGENHTML_PATH=/path/to/genhtml` and invoke the helper with `LCOV=/path/to/lcov`.

## Notes

- Coverage is only reliable with a **Debug** build; the CMake module will warn if `ENABLE_COVERAGE` is used with another build type.
- Only source files beneath this checkout's absolute `src/` path are included; external libraries can also have a `src/` directory.
- Branch coverage is enabled in the report when supported.

---

## Project Structure for Tests

See [`tests/`](../../tests/) for the current module folders and [`tests/CMakeLists.txt`](../../tests/CMakeLists.txt) for the registered `TEST_SOURCES` list. The backtest tests are under `tests/backtest/`.

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

Reconfigure after adding every new test file. Without reconfiguration the file is not compiled or discovered, so check that its new test names appear in the run.

### Step 3 — Build and run

```bash
cmake -S . -B build_coverage -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
cmake --build build_coverage
./build_coverage/bin/Debug/trade_ngin_tests --gtest_filter="MyComponentTest.*"
```

### Step 4 — Generate coverage to verify

```bash
cmake --build build_coverage --target coverage
open build_coverage/coverage/index.html
```

---

## Test Conventions

1. **File naming**: `test_<source_basename>.cpp` (e.g. `test_order_manager.cpp` tests `order_manager.cpp`)
2. **Test fixture naming**: `<ClassName>Test` (e.g. `OrderManagerTest`)
3. **Compile definitions**: Tests compile with `TESTING` and `USE_DIRECT_DB_CONNECTION` defined — use `#ifdef TESTING` guards in production code when needed
4. **Include paths**: Tests include headers via `trade_ngin/<module>/<header>.hpp`
5. **Database safety (issue #123 rule 4)**: Never point a test or runner at production. Database-backed tests use `TRADE_NGIN_TEST_DSN` for a disposable test database and skip without it. Set `TRADE_NGIN_REQUIRE_DB=1` when a required database test must fail instead of skip.
6. **Log suppression**: You may see `WARNING: Logger not initialized` during tests — this is expected and harmless
7. **New files (issue #123 rule 6)**: Reconfigure with `cmake -S . -B <build dir>` before building, then verify the new test names ran.

---

## Filtering External Libraries from Coverage

`make coverage` and `scripts/coverage_baseline.sh` extract only `"$(git rev-parse --show-toplevel)/src/*"` from the raw capture. Keep the absolute checkout prefix: a bare `*/src/*` also matches source trees inside third-party libraries.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `lcov not found` | `brew install lcov` |
| `genhtml: ERROR: category` | Already handled — `cmake/CodeCoverage.cmake` passes `--ignore-errors category` |
| Very low coverage numbers | Check `cov_src.info` from the baseline helper; it contains only this checkout's `src/` files |
| `WARNING: Logger not initialized` | Harmless — Logger singleton isn't set up in test harness |
| Tests fail to link | Rebuild in a fresh coverage directory and run `cmake -S . -B <build dir> -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON` before `cmake --build <build dir>` |

---

## Detailed Analysis

For a full module-by-module coverage breakdown with per-file tables and improvement priorities, see:

📊 **[Coverage Analysis](coverage_analysis.md)**
