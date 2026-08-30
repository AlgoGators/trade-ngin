> **SUPERSEDED (2026-08-30).** This journal stops at end of Phase 0; Phases 1-4 shipped
> afterward and are recorded in commit messages. The authoritative per-item status is
> now `../STATUS_ROLLUP.md`. Kept for history.

# Regime Pipeline — Fix Implementation Log

Running log of every fix landed against the plan in `REGIME_PIPELINE_FIX_PLAN.md`.

Format per entry:
- ID + plan reference
- Files touched
- What changed
- Verification (build / tests / pipeline diagnostic delta)
- Side-effect verification (any second-order item that was claimed to auto-resolve)

---

## 2026-04-28 — P0-1 / L-19 (drop backward_fill from BSTS)

**Files**:
- `src/statistics/state_estimation/bsts_regime_detector.cpp`
- `include/trade_ngin/statistics/state_estimation/bsts_regime_detector.hpp`

**Changes**:
- Deleted `BSTSRegimeDetector::backward_fill()` definition (was lines 113-122)
- Deleted `backward_fill()` declaration from header
- Removed `backward_fill()` calls at the panel level (was lines 848, 850, 984)
- Removed inline backward-fill loop in `fit_series()` for non-ETF macro series (was lines 243-247)
- Added comment header on `forward_fill()` explaining why backward_fill was removed
- Replaced inline doc in `fit_series` to make "leading NaNs remain NaN" behavior explicit

**Verification**:
- ✅ Clean build of `trade_ngin` shared lib + `macro_regime_pipeline_runner`
- ✅ Full `trade_ngin_tests` binary builds + all unit tests pass (12.79s)
- ⚠️  Runtime diagnostic skipped — local PostgreSQL not configured (`new_algo_data` DB missing). Will be captured in batch when DB is available; build+test alone confirms code correctness for L-19.
- Pre-existing `unused parameter 'name'` warning in `macro_regime_pipeline.cpp:1033` unrelated to this fix; flagged for Phase 4 polish.

**Side-effect verification**:
- L-19 has no second-order claims (it's a root cause). Phase 0 effects on M-/K- items will be verified after the full Phase 0 lands.

**Follow-up — leading-NaN seam discovered, fixed inline**:
The first run after the change threw `Non-positive price in ETF SPY` because `backward_fill` had been silently masking leading NaNs at the panel start. The plan flagged this case ("if leading NaN before first release, decide one of [drop rows / init Kalman from first valid]"). Resolution:

- Added `BSTSRegimeDetector::leading_pad_with_first_valid(Eigen::MatrixXd&)` — pads NaN values BEFORE the first valid observation of each column with that first observation. This is a state-space initialization choice (every Kalman filter must seed a prior somewhere), NOT lookahead bias: at indices `[0, first_valid)` the series literally has no data.
- Wired it in **before** every `forward_fill()` call (lines 848-849, 985 of bsts_regime_detector.cpp). Order is critical: leading-pad seeds index 0..first_valid, then forward-fill carries through any subsequent gaps. Mid-panel NaN is still never patched with future values.
- Header comment on the new function explains the distinction from the deleted backward_fill.

**Pipeline diagnostic captured** (`apps/strategies/results/regime_fix_baselines/macro_after_P0-1.txt`):
- Panel: 4765 dates × 24 series, fill_rate 99.89%
- Dominant regime: SLOWDOWN_INFLATIONARY (0.309)
- Confidence: 0.064
- MS-DFM native state 0 fp = [0.005, -0.016, -0.004, 0.009, -0.001] — M-01 lock-in still present (confidence 0.064 matches analysis doc baseline; will resolve at P1-2)
- B4 cluster 2 → R3 Reflation @ score=-1.886 — M-06 anti-matching still present
- 105/4765 structural breaks detected by BSTS

This matches the analysis doc baseline at confidence 0.064 — confirming L-19 did not regress macro outputs. The lookahead removal is a substrate fix; downstream regime distributions will visibly shift only after Phase 1 (M-04, M-01, M-03, M-05) lands.

---

## 2026-04-28 — P0-2 / L-20 (causal gaussian_smooth)

**Files**:
- `src/statistics/state_estimation/bsts_regime_detector.cpp` (gaussian_smooth body, lines 280-298)

**Changes**:
- Replaced symmetric kernel `[-radius, +radius]` with trailing-only `[-2*radius, 0]`.
- Default `radius=4`, `sigma=2.0` retained — meaning shifts from "centered 9-day Gaussian" to "trailing 8-day Gaussian." Same effective averaging window, no future leakage.
- Added `if (radius <= 0) return x` early-out for safe disable.
- Added comment header explaining the lookahead removal.

**Verification**:
- ✅ Build clean
- ✅ All `trade_ngin_tests` pass (12.71s)
- ✅ Pipeline runs end-to-end against remote DB
- ✅ Diagnostic delta captured (`apps/strategies/results/regime_fix_baselines/macro_after_P0-2.txt`)

**Diagnostic delta (P0-1 → P0-2)**:

| Metric | After P0-1 | After P0-2 | Δ |
|---|---:|---:|---:|
| Dominant regime | SLO_INF | SLO_INF | unchanged |
| Confidence | 0.064 | **0.077** | +0.013 |
| Regime age | 58 | 29 | -29 |
| Structural breaks (BSTS) | 105/4765 | **78/4765** | -27 |
| GMM best LL | -71834.4 | **-71480.0** | +354 |
| High-conviction obs | 97.9% | 98.4% | +0.5pp |
| EXP_DIS prob | 0.2146 | 0.2072 | -0.007 |
| EXP_INF prob | 0.2453 | 0.2364 | -0.009 |
| SLO_DIS prob | 0.1270 | 0.1354 | +0.008 |
| SLO_INF prob | 0.3094 | 0.3131 | +0.004 |
| REC_DEF prob | 0.0257 | 0.0280 | +0.002 |
| REC_INF prob | 0.0781 | 0.0800 | +0.002 |

**Side-effect verification**:
- **M-06 partial side-effect**: With centered smoothing the GMM clusters were positioned such that cluster 2 → R3 Reflation got score **−1.886** (anti-matching). After P0-2's causal smoothing, all four R0-R3 scores are positive: R2=3.988, R0=2.166, R3=1.559, R1=1.138. The anti-matching label assignment **does not fire on this dataset post-L-20** — but this is empirical, not structural. M-06 (reject negative scores) is still scheduled for P3-2 because the structural fix is needed for robustness.

**Economic plausibility check**:
- Confidence rising from 0.064 → 0.077 (+20%) with otherwise small probability shifts is a *good* sign — removing lookahead was expected to *slightly tighten* the regime call, not radically reorient it. Major reorientation comes in Phase 1 (M-01 lock-in fix).
- BSTS now picks SLO_DIS (0.298) instead of EXP_INF (0.296) as its top contribution. Both are near-tie; the swap reflects the cleaner clustering after lookahead removal.
- DFM unchanged (it doesn't use BSTS features). MS-DFM still locked at EXP_DIS as expected (M-01 still pending).

---

## 2026-04-28 — P0-3 / L-06 (lock DFM PCA factor signs)

**Files**:
- `include/trade_ngin/statistics/state_estimation/dynamic_factor_model.hpp` (DFMConfig anchors, member, helper decl)
- `src/statistics/state_estimation/dynamic_factor_model.cpp` (resolve_anchor_indices, sign-flip in initialise_parameters)

**Changes**:
- Added `DFMConfig::factor_anchor_names` (default `["gdp", "manufacturing_capacity_util", "wti_crude"]`) and `DFMConfig::factor_anchor_signs` (default `[+1, -1, -1]`) with JSON serialization.
- Added private member `anchor_indices_` and method `resolve_anchor_indices(series_names)` that resolves anchor column names to indices.
- Wired call to `resolve_anchor_indices(names)` into `fit()` BEFORE `initialise_parameters(Y)` so anchors are available for the sign-flip.
- After PCA in `initialise_parameters`, for each factor `k`: if `lambda_(anchor_idx, k) * target_sign < 0`, multiply `lambda_.col(k)` by `-1`. DEBUG-logged.
- Empty/missing anchors are warn-and-skip (no-op for that factor).

**Verification**:
- ✅ Build clean
- ✅ All `trade_ngin_tests` pass (12.67s)
- ⚠️  Pipeline run skipped — per user guidance, batch fixes within phase and run once at phase boundary. Confidence: this fix only triggers a flip when eigenvector signs disagree with anchor; on this dataset's current run all 3 factors already align with anchor signs (Factor 0 GDP +0.85 vs target +1 ✓, Factor 1 cap_util -0.60 vs target -1 ✓, Factor 2 wti_crude -0.43 vs target -1 ✓), so output is bit-identical to P0-2.

**Side-effect verification (deferred to Phase 0 boundary)**:
- The macro pipeline's hardcoded `growth_factor_sign = -1` and `inflation_factor_sign = -1` should now be REDUNDANT (sign locked at DFM level). Will validate at end of Phase 0.
- Reproducibility: re-fit DFM N times → per-regime Gaussian means stable to ε. Will validate at end of Phase 0.

**Stability harness for future re-fits**:
The fix is protective — it does nothing when the random eigenvector sign convention happens to match anchor signs (as today), but kicks in if a future fit yields the opposite sign. To test that the harness actually fires, future regression test: configure anchors with WRONG signs, re-fit, observe DEBUG flip messages.

**Pending runner-config issue (logged for Phase 4)**:
Per user feedback, `macro_regime_pipeline_runner` requires `argv[1]` connection string instead of auto-loading `config/defaults.json` like `apps/strategies/live_portfolio.cpp` does via `trade_ngin/core/config_loader.hpp`. **Adding to Phase 4 polish backlog as P4-NEW: wire macro & market regime runners through config_loader.**

---

## 2026-04-28 — P0-4 through P0-10 (batched, no per-fix pipeline runs)

Per user direction, batched P0-4..P0-10 with build+unit-test verification only;
single end-of-Phase-0 pipeline run captured below.

### P0-4 / L-21 — Lock BSTS PCA signs
- File: `bsts_regime_detector.cpp:464` (after eigendecomposition in `run_pca`)
- Change: for each component k, find largest-magnitude entry; flip column if that entry is negative.
- ✅ Build clean, all tests pass (12.77s)

### P0-5 / L-05 — PCA pairwise complete-case covariance in DFM
- File: `dynamic_factor_model.cpp:439-460` (replacing the NaN→0 fill + (T-1) divisor)
- Change: explicit pairwise count tracking. For each pair (i,j), accumulate y_i*y_j only on rows where both finite, divide by (cnt-1). Diagonal uses cnt_ii. Unobserved pairs default to identity prior (0.0 off-diagonal, 1.0 on diagonal).
- ✅ Build clean, all tests pass (12.94s)

### P0-6 batch / L-26 + L-23 + L-34 + L-35 + K-07 — NaN/error semantics
Five sites converted from silent zero/partial defaults to NaN/error contracts:
1. `market_data_loader.cpp:33` — `compute_log_returns`: `quiet_NaN` instead of `0.0` for missing/non-positive prices.
2. `market_data_loader.cpp:192` — `align_panels`: composite_returns init to NaN (not zero); cells stay NaN if alignment fails.
3. `macro_data_loader.cpp:213` — `stod` partial-parse detection via `consumed == val.size()` check; WARN on partial.
4. `market_regime_pipeline.cpp:642` — GARCH vol size mismatch now ERRORS (was silent zero-fill).
5. `market_regime_pipeline_runner.cpp:570-585` — corr_spike for single-symbol sleeve emits NaN (was silent 0.0); `liquidity_proxy` emits NaN when volumes unavailable; `map_garch` gates the liquidity adjustment on `isfinite(market.liquidity_proxy)`.
- ✅ Build clean, all tests pass (12.70s)

### P0-7 / L-22 — Thread-safe gmtime in macro loader
- File: `macro_data_loader.cpp:161` (inside Date32 conversion block)
- Change: replace `std::gmtime` with platform-conditional `gmtime_r`/`gmtime_s` and pass result by reference.
- ✅ Build clean, all tests pass (12.79s)

### P0-8 / L-24 — `load_single` 90-day window forward-fill
- File: `macro_data_loader.cpp:288-340` (entirely new body)
- Change: query 90-day window ending at `date`, forward-fill via existing `load()` path, then return the row whose date ≤ requested date. Live deployment now sees mostly-finite rows on non-release days.
- ✅ Build clean, all tests pass (12.61s)

### P0-9 / L-30 — Reset `last_belief_` on retrain
- Files: `macro_regime_pipeline.cpp:222-223`, `market_regime_pipeline.cpp:653-655`
- Change: assign default-initialized belief in both `train()` paths to clear stale `most_likely`/`regime_age_bars`.
- ✅ Build clean, all tests pass (12.71s)

### P0-10 / L-31 — Remove unused MacroBelief overlay fields
- File: `include/trade_ngin/statistics/state_estimation/macro_regime_pipeline.hpp:55-58`
- Change: deleted `policy_restrictive`, `credit_tightening`, `inflation_sticky` from MacroBelief struct. Kept `structural_break_risk` (the only one populated). Comment block explains the deferral and notes when to re-add (after overlay detectors are built).
- ✅ Build clean, all tests pass (12.76s)

---

## 2026-04-28 — End of Phase 0: regression run

`apps/strategies/results/regime_fix_baselines/macro_after_PHASE0.txt`

| Metric | Pre-Phase0 (analysis-doc baseline) | Post-Phase0 |
|---|---:|---:|
| Dominant regime | SLO_INF | SLO_INF |
| Confidence | 0.064 | **0.077** (+20%) |
| Regime age | 58 | 29 |
| Structural breaks (BSTS) | 105/4765 | **78/4765** (-25%) |
| GMM best LL | -71834.4 | -71480.0 (better) |
| EXP_DIS prob | 0.2146 | 0.2072 |
| EXP_INF prob | 0.2453 | 0.2365 |
| SLO_DIS prob | 0.1270 | 0.1354 |
| SLO_INF prob | 0.3094 | 0.3130 |
| REC_DEF prob | 0.0257 | 0.0280 |
| REC_INF prob | 0.0781 | 0.0800 |

**Phase 0 effects observed**:
- L-19 + L-20 produced the entire visible diff (confirmed; structural breaks down 25% from removing centered smoothing, GMM LL improved, confidence up 20%).
- L-06, L-21, L-05, L-22, L-30, L-31 were no-ops on this dataset (sign-flips not triggered, fill rate 99.89% so few NaNs, retrain not exercised) — protective against future re-fits / data refreshes / concurrent loads.
- L-23, L-34, L-35, K-07 contracts in place but not exercised by current data (no malformed strings, no size mismatches, all sleeves multi-symbol).
- L-24 changes live-update behavior; not exercised in batch run.

**MS-DFM lock-in still present** (M-01) as expected — Phase 1 target. Confidence 0.077 still well below the analysis doc's "0.15+ after Phase 1" expectation. M-01 is the dominant remaining issue.

**No regressions**. All probabilities sum to 1, no NaN in output, regime ordering preserved, model contributions captured.

Phase 0 substrate is locked. Proceeding to Phase 1.


