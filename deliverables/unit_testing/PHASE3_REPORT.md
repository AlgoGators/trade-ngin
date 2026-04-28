# Phase 3 — Unit Test Coverage Report and Forward Plan

Branch: `feature/test-coverage-improvements`
Last commit at time of writing: `cef6108` (`tests(live): cover historical_metrics, ...`)

This document captures (1) the state of unit-test coverage at the close of Phase 3, (2) the per-file picture across the repo, (3) the FIXMEs documented during the work, and (4) a concrete forward plan with prioritized refinements.

---

## 1. Headline numbers

| Scope | Lines covered | Rate |
|---|---|---|
| All `src/` files | 15,912 / 27,802 | **57.2%** |
| `src/` minus deferred files (chart_generator, email_sender, postgres_database*, live_trading_coordinator, execution_engine) | 15,878 / 21,402 | **74.2%** |
| Test count (excluding pre-existing ExecutionEngine crashes) | 1,161 passing | — |

Starting baseline at the start of this branch was approximately **20–25% in-scope lines**.

The gap between 57.2% and 74.2% is almost entirely the five files documented as out-of-scope or DB-DI-blocked:

| File | LOC | Coverage | Why deferred |
|---|---|---|---|
| `src/core/email_sender.cpp` | 2,555 | 0.1% | SMTP-coupled, `email_sender_refactor.md` |
| `src/data/postgres_database.cpp` | 1,401 | 2.1% | needs pqxx connection injection (Option B) |
| `src/core/chart_generator.cpp` | 1,229 | 0.1% | rendering-coupled, `chart_generator_refactor.md` |
| `src/execution/execution_engine.cpp` | 609 | 0.0% | pre-existing test crashes (FIXME-6, FIXME-7) |
| `src/data/postgres_database_extensions.cpp` | 430 | 0.2% | same DI blocker |
| **Total deferred** | **6,224** | | |

---

## 2. How to reproduce the coverage numbers

All commands run from `/Users/hemduttrao/tw/unit-tests`. Build directory is `build_coverage` (separate from `build/` which holds Release binaries for the backtest sentinel).

### One-time build setup

```sh
# Initial coverage configure (only needed once; re-runs auto-pick up source changes)
cmake -B build_coverage \
      -DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)" \
      -DENABLE_COVERAGE=ON
```

### After any source/test change

```sh
# 1. Build (re-applies macOS SDK fix if cmake regenerated build files)
cmake --build build_coverage --target trade_ngin_tests -j 8

# If build fails with MacOSX26.sdk/libm.tbd missing:
SDK=$(xcrun --show-sdk-path)
for f in $(grep -rl "MacOSX26.sdk" build_coverage); do
  sed -i.bak "s|/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk|$SDK|g" "$f"
done
find build_coverage -name "*.bak" -delete
cmake --build build_coverage --target trade_ngin_tests -j 8

# 2. Clear stale gcda counters before each run
find build_coverage -name "*.gcda" -delete

# 3. Run the full suite (ExecutionEngineTest filtered — pre-existing crashes)
build_coverage/bin/Debug/trade_ngin_tests \
  --gtest_filter='-ExecutionEngineTest.*:ExecutionEngineExtendedTest.*'
# expect: [  PASSED  ] 1161 tests
```

### Capture coverage numbers

The `--ignore-errors` flags suppress lcov format/inconsistent warnings caused by the older gcov on this machine; they don't change the underlying counts.

```sh
# Capture
lcov --capture --directory build_coverage \
     --output-file /tmp/cov_full.info \
     --ignore-errors unsupported,inconsistent,range,format,count --quiet

# Extract project-only (src/ tree)
lcov --extract /tmp/cov_full.info '*/src/*' \
     --output-file /tmp/cov_src.info \
     --ignore-errors unsupported,inconsistent,range,format,count --quiet
```

#### The 57.2% number (all src/)

```sh
lcov --summary /tmp/cov_src.info \
     --ignore-errors unsupported,inconsistent,format
# lines.......: 57.2% (15912 of 27802 lines)
```

#### The 74.2% number (in-scope only, excludes deferred files)

```sh
lcov --remove /tmp/cov_src.info \
     '*chart_generator*' '*email_sender*' '*postgres_database*' \
     '*live_trading_coordinator*' '*execution_engine*' \
     --output-file /tmp/cov_inscope.info \
     --ignore-errors unsupported,inconsistent,format,unused --quiet

lcov --summary /tmp/cov_inscope.info \
     --ignore-errors unsupported,inconsistent,format
# lines.......: 74.2% (15878 of 21402 lines)
```

#### Module-specific numbers

Replace `<module>` with one of: `backtest`, `core`, `data`, `execution`, `instruments`, `live`, `optimization`, `order`, `portfolio`, `risk`, `statistics`, `storage`, `strategy`, `transaction_cost`.

```sh
lcov --extract /tmp/cov_full.info "*src/<module>/*" \
     --output-file /tmp/cov_<module>.info \
     --ignore-errors unsupported,inconsistent,range,format,count --quiet

lcov --list /tmp/cov_<module>.info \
     --ignore-errors unsupported,inconsistent,format
```

#### HTML browser

```sh
genhtml /tmp/cov_src.info -o /tmp/cov_html \
        --ignore-errors unsupported,inconsistent,format
open /tmp/cov_html/index.html
```

---

## 3. Per-module per-file table (current state)

Coverage as of `cef6108`. ✅ = ≥80% line. The "why-not-80%" column is the operative thing for the forward plan.

### backtest (10 files)

| File | LOC | Line | Status / reason |
|---|---|---|---|
| `slippage_model.cpp` | 64 | 95.3% | ✅ |
| `backtest_execution_manager.cpp` | 64 | 96.9% | ✅ |
| `backtest_price_manager.cpp` | 75 | 100% | ✅ |
| `backtest_data_loader.cpp` | 149 | 83.9% | ✅ |
| `backtest_csv_exporter.cpp` | 177 | 81.4% | ✅ |
| `backtest_pnl_manager.cpp` | 262 | 94.7% | ✅ |
| `backtest_metrics_calculator.cpp` | 332 | 93.1% | ✅ |
| `backtest_portfolio_constraints.cpp` | 177 | 71.8% | iterative buffering paths need realistic vol/cov |
| `transaction_cost_analysis.cpp` | 241 | 75.5% | branch coverage on attribution paths |
| `backtest_coordinator.cpp` | 619 | 9.9% | DB init + run loop — DI blocker |

### core (9 files)

| File | LOC | Line | Status / reason |
|---|---|---|---|
| `state_manager.cpp` | 101 | 99.0% | ✅ |
| `run_id_generator.cpp` | 50 | 98.0% | ✅ |
| `config_base.cpp` | 27 | 88.9% | ✅ |
| `config_version.cpp` | 159 | 87.4% | ✅ |
| `config_loader.cpp` | 209 | 86.6% | ✅ |
| `logger.cpp` | 120 | 83.3% | ✅ |
| `config_manager.cpp` | 442 | 77.6% | dead helper `validate_numeric_range` + deadlock bug (FIXME-1) blocks empty-path init |
| `chart_generator.cpp` | 1,229 | 0.1% | **deferred** — rendering-coupled |
| `email_sender.cpp` | 2,555 | 0.1% | **deferred** — SMTP-coupled |

### data (6 files)

| File | LOC | Line | Status / reason |
|---|---|---|---|
| `market_data_bus.cpp` | 49 | 89.8% | ✅ |
| `credential_store.cpp` | 212 | 88.2% | ✅ |
| `conversion_utils.cpp` | 110 | 74.5% | unreachable static_pointer_cast null branches |
| `database_pooling.cpp` | 90 | 24.4% | needs PostgresDatabase DI to test pool ops |
| `postgres_database.cpp` | 1,401 | 2.1% | **deferred** — pqxx injection refactor |
| `postgres_database_extensions.cpp` | 430 | 0.2% | **deferred** — same DI blocker |

### execution (1 file)

| File | LOC | Line | Status / reason |
|---|---|---|---|
| `execution_engine.cpp` | 609 | 0.0% | **deferred** — pre-existing crashes (FIXME-6, FIXME-7) |

### instruments (4 files)

| File | LOC | Line | Status / reason |
|---|---|---|---|
| `option.cpp` | 111 | 91.0% | ✅ |
| `futures.cpp` | 56 | 82.1% | ✅ |
| `equity.cpp` | 39 | 74.4% | minor — tradability + dividend branches |
| `instrument_registry.cpp` | 241 | 73.9% | DB-load path is the gap |

### live (9 files)

| File | LOC | Line | Status / reason |
|---|---|---|---|
| `live_historical_metrics.cpp` | 112 | 100% | ✅ |
| `execution_manager.cpp` | 92 | 97.8% | ✅ |
| `live_price_manager.cpp` | 104 | 96.2% | ✅ |
| `live_metrics_calculator.cpp` | 198 | 90.4% | ✅ |
| `live_pnl_manager.cpp` | 203 | 81.8% | ✅ |
| `margin_manager.cpp` | 176 | 68.2% | `print_margin_summary` (cout) + DB-loaded instrument paths |
| `csv_exporter.cpp` | 383 | 18.3% | export methods need ITrendFollowingStrategy* / IDatabase* (DI blocker) |
| `live_data_loader.cpp` | 623 | 10.1% | every method is a SQL query — DI blocker |
| `live_trading_coordinator.cpp` | 176 | 0.6% | **explicitly excluded by you** at task start |

### optimization, order, portfolio, risk

| File | LOC | Line | Status / reason |
|---|---|---|---|
| `optimization/dynamic_optimizer.cpp` | 224 | 85.3% | ✅ |
| `order/order_manager.cpp` | 144 | 83.3% | ✅ |
| `portfolio/portfolio_manager.cpp` | 874 | 67.0% | iterative opt→risk loop, branch coverage 30.6% |
| `risk/risk_manager.cpp` | 380 | 76.1% | close — VaR calc edge cases |

### statistics (20 files)

20 files total. 17 are at ≥80%. The three that lag:

| File | LOC | Line | Status / reason |
|---|---|---|---|
| `statistics_utils.cpp` | 57 | 40.4% | small file but several utility branches untouched |
| `missing_data_handler.cpp` | 208 | 61.1% | interpolation strategies underexercised |
| `normalizer.cpp` | 98 | 64.3% | min-max + standardize paths partially covered |

### storage (3 files)

| File | LOC | Line | Status / reason |
|---|---|---|---|
| `backtest_results_manager.cpp` | 198 | 76.8% | bind_*_params SQL fan-out |
| `live_results_manager.cpp` | 174 | 74.7% | same — bind_*_params SQL fan-out |
| `results_manager_base.cpp` | 81 | 72.8% | base helpers exercised through subclasses |

### strategy (5 files)

| File | LOC | Line | Status / reason |
|---|---|---|---|
| `regime_detector.cpp` | 286 | 83.2% | ✅ |
| `base_strategy.cpp` | 321 | 76.0% | close — risk override paths |
| `trend_following_slow.cpp` | 897 | 67.9% | regime-coupling branches need full regime state |
| `trend_following.cpp` | 912 | 66.2% | same |
| `trend_following_fast.cpp` | 906 | 63.7% | same |

### transaction_cost (4 files)

All four ≥80%. ✅

---

## 4. FIXMEs documented during Phase 3

These are production bugs/oddities discovered while writing tests. Per the task contract, tests document the observed behavior; no production code changes were made.

| # | Severity | Location | Description |
|---|---|---|---|
| 1 | **High (deadlock)** | `core/config_manager.cpp` `initialize` + `save_configs` | Both take the same non-recursive `std::mutex`. Calling `initialize` on a non-existent config path deadlocks because `load_config_files` calls `save_configs` while holding the mutex. Fix: switch to `std::recursive_mutex` or unlock before the inner call. |
| 2 | Low (dead code) | `backtest/slippage_model.cpp` `VolumeSlippageModel::calculate_slippage` | A clamp branch is dead under realistic inputs. Fix: either prove unreachable and `assert`, or remove. |
| 3 | Medium (silent shadowing) | `storage/backtest_results_manager.cpp` | Declares its own `portfolio_id_` shadowing the base-class field. The base reads stale data. Fix: remove the subclass field, use base class accessor. |
| 4 | Low (subtle ordering) | `portfolio/portfolio_manager.cpp` (internal helper) | Iteration order dependency captured in `tests/portfolio/test_portfolio_manager_internals.cpp:187`. Captured for follow-up; no test added. |
| 5 | **Medium (test pollution)** | `data/database_pooling.hpp` `retry_with_backoff` | Calls unseeded `std::rand() % 100` for jitter, advancing global RNG state. Breaks `TransactionCostAnalyzerTest.ImplementationShortfall` non-deterministically. Fix: use a thread-local `std::mt19937` with its own seed. |
| 6 | Medium (idempotency) | `execution/execution_engine.cpp` `initialize` | Not idempotent — second call leaves the engine in inconsistent state. Fix: guard with `if (initialized_) return;` or reset state. |
| 7 | **High (correctness)** | `execution/execution_engine.cpp` `submit_execution` | Generates colliding job IDs within the same tick. Fix: include monotonic counter or sequence in ID. |
| 8 | Pre-existing | `tests/execution/test_execution_engine.cpp` `SimpleMarketOrder` | Test crashes (`vector` exception). Pre-existing — not introduced by this branch. Filtered from default test runs. |

---

## 5. Forward plan — what to do next, in priority order

The plan below is ordered by **value-per-effort**, not by line count.

### Priority A — Production fixes (small, high-value)

These are FIXMEs from the table above that should be fixed in the source, with regression tests added at the same time. Each is small.

| Task | File | Effort | Why first |
|---|---|---|---|
| A1 | Fix FIXME-1 (config_manager deadlock) — switch to recursive_mutex or unlock-before-save | `src/core/config_manager.cpp` | 1h | Production crash hazard. Unlocks `InitializeOnEmptyPathSeedsDefaultsAndPersists` test, lifts config_manager from 77.6% to ~85%. |
| A2 | Fix FIXME-7 (execution_engine ID collision) | `src/execution/execution_engine.cpp` | 1–2h | Correctness bug in live trading codepath. |
| A3 | Fix FIXME-5 (database_pooling unseeded rand) — use `std::mt19937` | `include/trade_ngin/data/database_pooling.hpp` | 30min | Lets us add the retry-path tests that are currently disabled, lifts database_pooling and removes a flake source. |
| A4 | Fix FIXME-6 (execution_engine init idempotency) + FIXME-8 root cause | `src/execution/execution_engine.cpp` | 2–3h | Unblocks the ExecutionEngine test suite (currently filtered). Once fixed, execution_engine moves from 0% → expected 75%+. |
| A5 | Fix FIXME-3 (results_manager portfolio_id_ shadowing) | `src/storage/backtest_results_manager.cpp` | 30min | Silent correctness bug. |

**Expected coverage delta after A1–A5:** ~23,000 → ~24,500 lines covered (a ~5% bump in the in-scope number, not from new tests but from the bugs no longer blocking existing tests).

### Priority B — DB injection refactor (large, unlocks 5,000 lines)

This is `deliverables/unit_testing/postgres_database_refactor.md` Option B. **One refactor, six files become testable.**

| Task | What | Effort | Files unlocked |
|---|---|---|---|
| B1 | Extract `IPgConnection` interface around the pqxx::connection use sites | 1 day | postgres_database.cpp |
| B2 | Inject the connection through PostgresDatabase / DatabasePool / LiveDataLoader / BacktestCoordinator constructors | 0.5 day | postgres_database_extensions, database_pooling, live_data_loader, backtest_coordinator |
| B3 | Build a `FakePgConnection` for tests with scriptable result sets | 0.5 day | All of the above |
| B4 | Write the actual tests against the fake | 1–2 days | 5,000+ lines of file content |

**Expected coverage delta after B1–B4:** in-scope coverage rises from 74.2% to **~88–92%** (this single refactor is the biggest lever in the project).

### Priority C — Push the existing sub-80% files

Each is small, contained work. Listed in increasing effort order.

| Task | File | Current | Target | Effort | Strategy |
|---|---|---|---|---|---|
| C1 | `statistics/statistics_utils.cpp` | 40.4% | 80% | 1h | Pure-math helpers, just need exhaustive cases |
| C2 | `instruments/equity.cpp` | 74.4% | 85% | 30min | Tradability + dividend branches |
| C3 | `risk/risk_manager.cpp` | 76.1% | 85% | 1.5h | VaR edge cases, position-limit override paths |
| C4 | `strategy/base_strategy.cpp` | 76.0% | 85% | 2h | Risk-override + state-transition branches |
| C5 | `live/margin_manager.cpp` | 68.2% | 85% | 2h | Redirect cout to cover `print_margin_summary`; inject more instruments for the loaded path |
| C6 | `instruments/instrument_registry.cpp` | 73.9% | 85% | 2h | Mock the DB path to cover `load_instruments` (depends on B refactor or fake) |
| C7 | `statistics/missing_data_handler.cpp` | 61.1% | 80% | 2h | Each interpolation strategy + edge cases |
| C8 | `statistics/normalizer.cpp` | 64.3% | 80% | 1h | Min-max + standardize round-trips on edge inputs |
| C9 | `backtest/transaction_cost_analysis.cpp` | 75.5% | 85% | 2h | Attribution-path branch coverage |
| C10 | `backtest/backtest_portfolio_constraints.cpp` | 71.8% | 85% | 2h | Iterative buffering with synthetic vol/cov inputs |
| C11 | `portfolio/portfolio_manager.cpp` | 67.0% | 80% line / 50% branch | 4–5h | Iterative opt→risk loop with carefully constructed inputs |
| C12 | `strategy/trend_following*.cpp` (3 files) | 64–68% | 80% each | 5–8h | Need full regime_detector state to exercise regime-coupling branches; covered indirectly by backtest sentinel |

**Expected coverage delta after C1–C12 (assuming B not done):** 74.2% → ~80% in-scope. With B done first, several of these become substantially easier.

### Priority D — chart_generator / email_sender refactors

| Task | Effort | Notes |
|---|---|---|
| D1 | `chart_generator_refactor.md` (extract `IChartRenderer`) | 3–4h | 1,229 lines from 0.1% → ~75% |
| D2 | `email_sender_refactor.md` (extract `ISmtpClient`) | 2–3h | 2,555 lines from 0.1% → ~85% |

**Expected coverage delta after D1+D2:** repo-wide rises from 57.2% to **~75%+**, in-scope from 74.2% to **~80%**.

---

## 6. Recommended sequence

If you want to maximize coverage gain per hour:

1. **A1, A3, A5** in one session (~2h). Unlocks bugs without much new testing work.
2. **B (full DB injection refactor)** as one focused mini-project (3–4 days). Single biggest win.
3. **A2, A4** alongside B (the execution_engine refactor and DB refactor are independent, can be done in parallel).
4. **C1, C2, C7, C8** as small-batch follow-ups (~6h total).
5. **D1, D2** as separate refactor PRs.
6. **C11, C12** last — these are the largest and lowest value-per-effort.

If you'd rather minimize change risk: do **A** only, ship as a coverage bump from 74% → ~78%, then plan B/D as separate scoped projects.

---

## 7. What this branch does NOT change

- No source code changes other than the 13 `virtual` qualifiers added to `PostgresDatabase` storage methods (commit `02d114f`) — that was the one production change required to enable mocking, pre-authorized.
- No production fixes — every FIXME is documented in tests and listed above; none are patched.
- No `live_trading_coordinator.cpp` tests — explicitly excluded at task start.
- No deletion of the pre-existing `ExecutionEngineTest.SimpleMarketOrder` failure — captured as FIXME-8.

---

## 8. Pointers

- Refactor plans: `deliverables/unit_testing/{chart_generator,email_sender,postgres_database}_refactor.md`
- Sentinel + per-phase coverage history: `.baselines/`
- Per-phase commits: `git log feature/test-coverage-improvements --oneline`
- Branch tip: `cef6108`
