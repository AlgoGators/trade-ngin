# chart_generator.cpp — Refactor Plan for Unit Test Compatibility

**Status:** deferred from Phase 3 — planning doc only
**Current coverage:** 0.1% line, 0.0% branch (1230 lines, 0 effective covered)
**Target after refactor:** 75% line, 50% branch (~860 lines covered)
**Effort estimate:** 3–4 hours source refactor + ~600 LOC of tests

## Why it can't be unit-tested today

The `render_*` chart methods generate gnuplot script strings, write them to a temp file, and execute the system `gnuplot` binary via `popen()` (helper at `src/core/chart_generator.cpp:1237` `execute_gnuplot()`). Running `gnuplot` in a unit-test environment is unreliable (binary may not be installed in CI; the subprocess writes images to disk; the result is base64-encoded PNG bytes which we can't meaningfully assert on without rendering).

The `fetch_*_data` methods call `db->execute_query(sql)` which returns an arrow table — those are already mockable via `MockPostgresDatabase::execute_query` (already mocked).

## Proposed refactor

Extract the gnuplot subprocess call behind a small interface so tests can substitute a fake.

### 1. New header `include/trade_ngin/core/gnuplot_executor.hpp`

```cpp
#pragma once
#include <string>
namespace trade_ngin::core {
class IGnuplotExecutor {
public:
    virtual ~IGnuplotExecutor() = default;
    /// Execute a gnuplot script with optional inline data payload; return
    /// base64-encoded PNG output (or empty string on failure).
    virtual std::string run(const std::string& script_content,
                            const std::string& data_content) = 0;
};
class GnuplotExecutor : public IGnuplotExecutor {
public:
    std::string run(const std::string& script, const std::string& data) override;
};
}  // namespace trade_ngin::core
```

### 2. Move existing `execute_gnuplot()` into `GnuplotExecutor::run()`

Cut the body of the current anonymous-namespace `execute_gnuplot()` (~30 lines around `chart_generator.cpp:1237`) into the new class. No behavior change.

### 3. Make `ChartGenerator` an instance class with injected executor

Two options — pick whichever the team prefers:

**Option A (preferred — minimal API churn):** Keep static methods, add an `executor_` static pointer set via `ChartGenerator::set_executor(std::shared_ptr<IGnuplotExecutor>)` (default constructed lazily to a real `GnuplotExecutor`). All `render_*` methods call `executor_->run(...)` instead of the free function. Tests call `set_executor(mock)` in `SetUp()` and `set_executor(nullptr)` in `TearDown()`.

**Option B (cleaner OO):** Convert to instance methods, ChartGenerator constructor takes `std::unique_ptr<IGnuplotExecutor>` (default constructed to real). Caller sites need updating in `src/backtest/backtest_results_manager.cpp` and `src/core/email_sender.cpp` where charts are generated. Larger blast radius.

### 4. Source change line counts

| File | Lines changed |
|---|---|
| `include/trade_ngin/core/gnuplot_executor.hpp` | +25 (new) |
| `src/core/gnuplot_executor.cpp` | +30 (new, cut from chart_generator) |
| `include/trade_ngin/core/chart_generator.hpp` | +5 (executor setter / param) |
| `src/core/chart_generator.cpp` | -30 (removed helper), +5 (use injected executor) |
| `src/core/CMakeLists.txt` | +1 (new source file) |
| **Total** | ~40 lines source change |

Production behavior is identical — pure indirection.

## What gets unit-tested

| Surface | Mock used | Coverage gain |
|---|---|---|
| 7 `fetch_*_data` methods | MockPostgresDatabase::execute_query → arrow tables | ~250 lines |
| 4 `render_*` methods | FakeGnuplotExecutor capturing script string + returning canned PNG | ~300 lines |
| 7 `generate_*` orchestrators | both mocks | ~250 lines |
| `escape_gnuplot_string` and other helpers | direct call | ~30 lines |
| Subprocess IO loop in `GnuplotExecutor::run` | NOT covered (real popen) | (untested ~30 lines) |

## Test file plan

- `tests/core/test_chart_generator.cpp` — main tests (~500 LOC)
- `tests/core/fake_gnuplot_executor.hpp` — test helper that captures the last script + data and returns a fixed string

## Things to verify in tests

For each `render_*` method:
- Generated script contains expected gnuplot directives (`set terminal png`, `set xrange`, `plot ...`)
- Empty input → safe fallback (current code has guards at lines 820-ish for single-point x-range)
- Special chars in symbol names are escaped via `escape_gnuplot_string`
- Time-series data with various lengths produces valid script

For each `fetch_*_data`:
- DB error → propagated as ChartData with error flag (or empty result, depending on contract)
- Empty result set → empty ChartData
- Multi-row result → correct transformation

## Out of scope (explicitly not tested)

- Whether the gnuplot binary actually produces a valid PNG — that's an integration concern
- Visual correctness of rendered charts
- Filesystem error paths in the temp-file write (production already has try/catch wrapping)
