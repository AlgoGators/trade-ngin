# #103 Step 0 — census and rebase readiness

Method: a throwaway clone of `AlgoGators/trade-ngin`, all refs fetched from origin, work done in
detached worktrees. Every claim below names the command that produces it. Step 0(d) and §8(ii)–(iv)
are blocked; see the end.

## Refs verified

Every sha in section 2 still resolves as documented: `main` `08b15c00`, RDU `1efd363b`,
`origin/regime-detection` `e3d04081`, PR #41 `4078c48a`, PR #99 `0738fd7e`.
`git merge-base main origin/regime_detection_updated` = `0c623ad8` (2026-04-03).
`git rev-list --left-right --count main...origin/regime_detection_updated` = `98  34`.
The history is linear — no merges in the 34.

## (a) Build of `1efd363b` — PASSES

| | |
|---|---|
| host | macOS 26.6.2, arm64 (darwin25.6.0) |
| cmake | 4.2.3 |
| compiler | Apple clang 21.0.0 (clang-2100.3.34.2) |
| generator | Ninja 1.13.1 |
| result | **`[163/163]`, 0 errors** — all targets link |

Binaries produced include all four regime runners: `macro_dfm_runner`, `macro_msdfm_runner`,
`macro_regime_pipeline_runner`, `market_regime_pipeline_runner`, plus `bsts_regime_detector`.

**Toolchain note for anyone repeating this on macOS.** `/usr/bin/c++` and `/usr/bin/make` are
Xcode-license-gated and exit 69 until `sudo xcodebuild -license` is run. That is avoidable — the
Command Line Tools compiler works without it:

```
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER=/Library/Developer/CommandLineTools/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/Library/Developer/CommandLineTools/usr/bin/clang++ \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DCMAKE_BUILD_TYPE=Release
```

Ninja is required: the default Makefiles generator fails its try-compile on the gated `make`.
`DEVELOPER_DIR` is required or the Threads probe fails and takes `find_package(GTest)` with it.
Dependencies came from Homebrew (`apache-arrow` 23.0.0, `eigen`, `googletest`, `libpqxx` 7.10.5,
`nlohmann-json` 3.12.0, `nlopt`); CURL resolves from the SDK. No vcpkg tree is needed.

## (b) `trade_ngin_tests` — 430 / 430

```
[==========] Running 430 tests from 63 test suites.
[==========] 430 tests from 63 test suites ran. (12723 ms total)
[  PASSED  ] 430 tests.
```

**Exactly the 430 expected. Zero failures, zero skips.** Run with `TRADE_NGIN_TEST_DSN` unset; no
test skipped for a missing DSN, so nothing in the current suite is database-gated.

## (c) Three-way conflict preview

`git merge-tree --write-tree origin/regime_detection_updated <target>`

| target | result |
|---|---|
| `main` (`08b15c00`) | no conflicts |
| stage-3 tip, PR #99 (`0738fd7e`) | no conflicts |
| PR #41 (`4078c48a`) | 2 conflicts: `CMakeLists.txt`, `tests/CMakeLists.txt` |

PR #41 also edits `hmm.cpp`, `garch.cpp`, `garch.hpp` and `test_garch.cpp`, all of which RDU
modifies, and those auto-merge without conflict — the overlap is semantic, not positional, so a
clean merge there would be misleading. Coordinate with #41's author before either lands.

## §8(i) Rebase — and a recommendation to merge instead

**All 34 commits replay onto main. The conflict count depends entirely on how the first one is
resolved, and no mechanical resolution is usable.**

| resolution of the `CMakeLists.txt` conflict | conflicting commits | outcome |
|---|---|---|
| take RDU's side (`--theirs`) | 1 | **silently reverts main.** `git diff main <result> -- CMakeLists.txt` = 18 insertions, 47 deletions. Drops `add_subdirectory(benchmarks)`, `CodeCoverage`, `CMAKE_BUILD_RPATH`, `USE_DIRECT_DB_CONNECTION`, and five sources including `market_data_utils.cpp` and `broker_frame.cpp` |
| union | 3 — `3661fa6`, `10a43fd`, `b33913e` | keeps both sides' content and duplicates no source entry, but emits `add_subdirectory(apps)` **twice** (lines 231 and 238). CMake then fails: "The binary directory .../apps is already used to build a source directory." |

Only the first hunk is additive (main's `add_subdirectory(benchmarks)` against RDU's `add_executable(test_dfm …)` at EOF). The two downstream conflicts are delete/replace: RDU's own history churns the statistics source list — `10a43fd` collapses the modular `src/statistics/**` entries into a single `statistics_tools.cpp`, `b33913e` re-expands them. A human has to resolve these.

**The merge does not have this problem.** The auto-merged tree keeps both sides — `CMAKE_BUILD_RPATH`, `CodeCoverage`, `benchmarks`, `market_data_utils.cpp`, `broker_frame.cpp`, all eight `src/regime_detection/**` entries and `clustering/gmm.cpp` — with a single `add_subdirectory(apps)`. I took it end to end:

| merge of RDU into main | result |
|---|---|
| configure | exit 0 |
| build | **`[287/287]`, 0 errors** |
| combined suite | 1980 tests from 233 suites, **1907 passed, 0 failed**, remainder skipped as database-gated with no `TRADE_NGIN_TEST_DSN` |

So the two lanes integrate cleanly today: RDU's 430 tests and main's suite coexist with no failures and no build breakage.

Recommendation: bring RDU and main together with a merge, not a rebase. If D7 requires a rebase, budget for three hand-resolved `CMakeLists.txt` conflicts and re-verify the configure afterwards; do not accept a mechanical resolution.

## §8(i) Files RDU touches outside `src/regime_detection`

Your list is confirmed — `CMakeLists.txt`, `README.md`, `apps/CMakeLists.txt`,
`src/statistics/README.md`, `tests/CMakeLists.txt`, the shared statistics libraries and their
tests — and **`market_data_bus.hpp` is genuinely untouched** (the file exists on both sides).

Also changed, and worth adding to the list:

| path | status | note |
|---|---|---|
| `include/trade_ngin/statistics/clustering/gmm.hpp` | **added** | a *new shared library*, not an edit to an existing one |
| `src/statistics/clustering/gmm.cpp` | **added** | 209 lines; no `GMM`/`GaussianMixture` exists anywhere on main |
| `include/trade_ngin/statistics/volatility/garch.hpp` | modified | +4 |
| `include/trade_ngin/statistics/volatility/egarch.hpp` | modified | +2 |
| `scripts/fetch_macro_data.py` | added | |
| `scripts/download_weekly_macro_data.py` | added | CRLF line endings |
| `assets/results/bsts_regime_latest.txt` | added | a committed run artefact |

The GMM arriving as a **shared** library collides with #107 Part A, which moves an ML GMM to main.
Those two need reconciling before either lands.

(Not listed above because they are regime paths by name, but they are new: eight
`include/trade_ngin/regime_detection/**` headers — including `msar.hpp` promoted into the public
include tree — five `apps/regime_detection/*` files, and eleven new `tests/statistics/test_*.cpp`.)

## §8(i) Do the shared-library edits change any number MAIN produces?

**No — but not because the edits are behaviour-neutral. They are not.**

| file | change | effect |
|---|---|---|
| `garch.cpp` | L-09: `update()` demeans with a persisted `mean_return_`, previously used the raw return | changes `new_var` and `residuals_` on every `update()` (whenever the training mean is nonzero) |
| `egarch.cpp` | L-09: same demean, applied to `z` | changes `new_log_h`, `current_volatility_`, `residuals_` |
| `markov_switching.cpp` | `var_sum/gamma_sum + 1e-6` → `max(var_sum/gamma_sum, max(1e-6, 0.01*global_var))` | the old form *unconditionally added* 1e-6; the new one adds nothing unless the floor binds. Every non-degenerate state shifts by 1e-6 on **every** fit, and it feeds back through EM. On demeaned daily returns (var ≈ 1e-4) that is a ~1% relative shift |
| `hmm.cpp` | zero-`row_sum` / zero-`gamma_sum` guards; a relative covariance ridge; an LDLT log-det floor in `log_emission_probability` | guards change transition rows and emissions only when a state collects no posterior. The ridge is `max(1e-6, 0.01*mean_diag_scale/diag_count)` — it changes covariances only where it binds, and is bit-identical to the old fixed `1e-6` where it does not |
| `kalman_filter.cpp` | covariance update to Joseph form; `JacobiSVD` condition check → LLT-diagonal approximation | Joseph is algebraically equivalent but differs in floating point. The condition check is warning-only and does not touch `x_`/`P_`, but being approximate it now warns on different inputs |

**Main has no consumer.**

```
git grep -nE '#include.*statistics/(state_estimation|volatility|clustering)' main \
  -- src apps include ':(exclude)src/statistics' ':(exclude)include/trade_ngin/statistics'
```

returns nothing. Per symbol across the whole tree, `GARCH`, `EGARCH`, `MarkovSwitching` and
`KalmanFilter` appear only in the statistics library, its README and `tests/statistics/**`. The
three `HMM` hits in `core/` are the `YYYYMMDD_HHMMSS` timestamp format, not the class.
`benchmarks/benchmark_main.cpp` does include the umbrella `statistics.hpp`, but only exercises
`Normalizer` and the ADF test, neither of which RDU touches.

**So no stored engine number can move from these edits today.** But the exposure is wider than the
tests RDU modifies. These three existing tests consume the changed classes and RDU leaves them
untouched:

- `tests/statistics/test_egarch.cpp` — calls `egarch.update(-0.05)`, the exact path L-09 changes
- `tests/statistics/test_convergence_monitoring.cpp` — builds `HMM`, `MarkovSwitching`, `GARCH`, `EGARCH`, `GJR` and calls `fit_with_diagnostics`
- `tests/statistics/test_statistics_integration.cpp` — constructs `GARCH` through the umbrella header

Their assertions are loose (`EXPECT_GT(vol_after, 0.0)`), which is consistent with the full suite
passing 430/430 — but they are unguarded against these numerical changes, and a reviewer told the
modified-test set is complete would be misled.

Recommendation: land the shared-library fixes as their own PR together with the statistics tests,
so the numerical changes get reviewed on their own rather than inside the regime merge. That also
decouples them from PR #41, which edits two of the same files.

## §8(iii) Credit spreads reconciliation

Method: read-only query against `new_algo_data` (PostgreSQL 16.14 + TimescaleDB), run through
pgAdmin. No writes, no runner started.

### What is populated

| table | rows | first date | last date | `ig_credit_spread` non-null | `high_yield_spread` non-null |
|---|---|---|---|---|---|
| `macro_data.credit_spreads` | 3,977 | 2011-03-23 | 2026-04-07 | 3,928 | 3,928 |
| `macro_data.growth` | 941 | 2011-01-01 | 2026-03-28 | | |
| `macro_data.inflation` | 3,979 | 2011-03-01 | 2026-04-08 | | |
| `macro_data.liquidity` | 3,128 | 2011-03-01 | 2026-04-01 | | |
| `macro_data.market` | 3,942 | 2011-03-23 | 2026-04-07 | | |
| `macro_data.yield_curve` | 3,980 | 2011-03-01 | 2026-04-08 | | |

`credit_spreads` is **populated, not empty**: both columns carry 3,928 of 3,977 rows (98.8%), and
the two counts are identical, which is consistent with one FRED fetch writing both. 49 rows carry
neither value.

### Which pipeline reads it

The **macro** pipeline only. Both columns are joined in `macro_data_loader.cpp:62` and enter the
24-series panel as `"ig_credit_spread"` and `"high_yield_spread"`
(`macro_regime_pipeline.hpp:190`). The **market** pipeline does not read this table at all — which
is precisely what Gap 6 is about.

### What Gap 6 still needs

`ENHANCEMENTS.md:388` framed Gap 6 as: verify the table is populated and refreshed, then wire a
market-side funding-stress detector to it; "if stale/empty, it is an ingest ask."

It is **not empty**, so Gap 6 is not an ingest ask for existence — it is the market-side detector
work, and that work is unblocked on data grounds.

It **is stale**. The last observation is 2026-04-07, roughly six months before this run. That is
not specific to credit spreads: the whole panel stops between 2026-03-28 and 2026-04-08, which
matches the macro ingest port still being outstanding (D8; `DATA_OWNER_ASKS.md` item 7, sent
2026-09-11). So the refresh-cadence question in item 10 resolves to the same dependency as the
rest of the panel rather than to anything particular to this table.

### Can the ICE BofA series be backfilled before 2011 for the 2007 fixture?

**The history exists, but the route the repo uses can no longer reach it.**

`scripts/fetch_macro_data.py:39-40` sources both columns from FRED:

| column | FRED series |
|---|---|
| `ig_credit_spread` | `BAMLC0A4CBBB` — ICE BofA BBB US Corp Index OAS |
| `high_yield_spread` | `BAMLH0A0HYM2` — ICE BofA US High Yield Index OAS |

`BAMLH0A0HYM2` begins **1996-12-31**, comfortably before 2007. But both FRED series pages now
carry this note, verbatim and identical on each:

> "Starting in April 2026, this series will only include 3 years of observations. For more data,
> go to the source."

So as of April 2026 FRED serves a rolling three-year window. `fetch_macro_data.py` cannot backfill
pre-2011 from FRED, and can no longer even reproduce the 2011-2023 span already in the table. A
pre-2011 backfill has to come from ICE Data Indices directly.

Two consequences worth a ruling before anyone plans the 2007 fixture:

1. ICE Data Indices is a **new vendor**, and §4 lists "new data that costs money or needs a new
   vendor" as a decision that comes back to HD.
2. Backfilling `credit_spreads` alone would not deliver a 2007 fixture anyway. **The 2011 floor is
   shared by the entire macro panel** (growth 2011-01-01; inflation, yield_curve and liquidity
   2011-03-01; credit_spreads and market 2011-03-23). Any pre-2011 fixture needs the whole panel
   backfilled, not one table.

There is also a narrower risk that is independent of the 2007 question: because FRED has truncated
to three years, the *existing* history in this table is now only reproducible from what is already
stored. It should be treated as the system of record and backed up before any reload runs.

## Blocked

| part | blocker |
|---|---|
| (d) timeline gate diff, `TZ=UTC` | needs the scratch database copy. Section 12 says it comes from you — requesting one; please name the snapshot date |
| §8(ii) baseline reproduction against `market_timeline_K05plus.csv` | same copy |
| §8(iv) macro ingest timing | needs the data owners, not code |
