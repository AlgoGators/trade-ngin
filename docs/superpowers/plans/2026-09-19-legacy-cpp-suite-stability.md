# Legacy C++ Suite Stability Implementation Record

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:systematic-debugging and superpowers:test-driven-development. Execute inline; do not delegate this shared-state investigation.

**Goal:** Make the monolithic `trade_ngin_tests` run deterministic, failure-free, and crash-free without weakening or skipping tests.

**Implemented approach:** Reproduce the all-tests-only failure in the normal binary, retain line-buffered per-test output to locate the crash, add a deterministic failing regression for the uninitialized state, then correct initialization at the type boundary. Keep QT reporting code unchanged.

**Tech Stack:** C++20, GoogleTest, CMake/Ninja, GCC 13, Ubuntu WSL.

**Spec:** `docs/audits/2026-09-19-qt-platform-verification.md`

## Global Constraints

- Do not disable, exclude, reorder, or relax a failing test to obtain green output.
- Do not change production behavior before a failing regression test demonstrates the root cause.
- Keep production database access out of this investigation.
- Run the final binary as one process, because process isolation would hide shared-state defects.

---

### Task 1: Establish deterministic reproductions — complete

**Files:**
- Read: `tests/CMakeLists.txt`
- Read: `tests/strategy/test_base_strategy.cpp`
- Read: `include/trade_ngin/core/state_manager.hpp`

**Interfaces:**
- Consumes: the registered GoogleTest order in `trade_ngin_tests`.
- Produces: exact filters that reproduce the BaseStrategy failure and the later crash.

- [x] Configured and built the normal Debug test binary in `build-wsl-debug`.
- [x] Reproduced `BaseStrategyTest.CheckRiskLimits_FailsOnMaxDrawdown` in the monolithic process.
- [x] Confirmed `BaseStrategyTest.*` passed 12/12 in isolation before the fix.
- [x] Used line-buffered per-test output to prove the segmentation fault occurred inside the same failing test, immediately after its non-fatal assertion, rather than in a later suite.
- [x] Inspected the failure path and found `StrategyMetrics` had 17 uninitialized scalar members. The differing full-suite result was allocator-history-dependent, not a persistent singleton leak, so predecessor bisection was no longer necessary.

### Task 2: Obtain initialization evidence — complete

**Files:**
- Read: the production and test files named by the failing stack traces.

**Evidence:**
- A placement-construction regression prefilled the `StrategyMetrics` storage with `0xA5`.
- Before the production fix, all fields retained the sentinel representation; `total_trades` was `-1515870811`, and all 16 doubles were nonzero.
- The crash path was direct: `EXPECT_TRUE(result.is_error())` was non-fatal, then `result.error()->code()` dereferenced null.
- ASan/UBSan was not used as acceptance evidence. AddressSanitizer does not diagnose uninitialized scalar values, and the deterministic red test plus the exact null-dereference control flow established both causes directly.

### Task 3: Add regression coverage and fix the root causes — complete

**Files:**
- Modify only the test and production files directly identified by Tasks 1–2.
- Update: `docs/audits/2026-09-19-qt-platform-verification.md`

**Interfaces:**
- Consumes: confirmed root causes and exact minimal reproductions.
- Produces: deterministic lifecycle behavior that remains correct in the monolithic process.

- [x] Added `StrategyMetricsTest.DefaultConstructionInitializesEveryField` and observed it fail before the fix.
- [x] Added explicit zero member initializers to every `StrategyMetrics` scalar and observed the regression pass.
- [x] Changed the risk-limit test to require successful execution setup and use fatal assertions before accessing an error object.
- [x] Ran the initialization regression and the formerly failing risk-limit test together: 2/2 passed.

### Task 4: Verify and document — complete

**Files:**
- Update: `docs/audits/2026-09-19-qt-platform-verification.md`

**Interfaces:**
- Consumes: fixed normal and sanitizer binaries.
- Produces: auditable evidence that the monolithic suite is green.

- [x] Full normal run 1: 1,277/1,277 passed in 27.485 seconds.
- [x] Full normal run 2: 1,277/1,277 passed in 26.155 seconds.
- [x] Final rebuilt run: 1,277/1,277 passed in 27.259 seconds.
- [x] QT/report-focused filter: 54/54 passed.
- [x] `git diff --check` passed; no tests were disabled, skipped, filtered from the full run, reordered, or relaxed.
- [x] Updated `docs/audits/2026-09-19-qt-platform-verification.md` with root cause and evidence.

## Files changed

- `include/trade_ngin/strategy/types.hpp` — explicit zero defaults for all strategy metrics.
- `tests/strategy/test_base_strategy.cpp` — deterministic initialization regression and guarded risk-limit assertions.
- `docs/audits/2026-09-19-qt-platform-verification.md` — full-suite result and root-cause record.

## Scope boundary

This repair does not modify QT reporting, the daily email template/sender, SQL, migrations, or any production database. The separate approved read-only production audit remains blocked until an approved local connection profile or procedure is supplied.
