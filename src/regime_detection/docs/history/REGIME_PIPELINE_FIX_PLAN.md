# Regime Pipeline — Implementation Plan with Root-Cause Analysis

**Plan date**: 2026-04-28
**Inputs**: `docs/REGIME_PIPELINE_ANALYSIS.md` (M-01..M-13, K-01..K-17), `docs/REGIME_PIPELINE_LIBRARY_AUDIT.md` (L-01..L-35)
**Rule** (carried over): spec ≠ code is not a bug unless economically/mathematically wrong. Empirical tuning stays.

## 0. How this plan is built

Three classifications applied to every active item:

- **ROOT** — first-order bug at the place it's reported. Fix here; symptoms elsewhere may go away.
- **SECOND-ORDER** — the symptom is real but it's caused by another item. Fixing the root makes this disappear; do not fix in isolation.
- **NOT-A-BUG** — looked like one on first audit; on re-read the math is correct, the API contract is reasonable, or the divergence is empirical tuning. Drop from active list.

Then items are grouped into phases that respect dependencies. The earlier the phase, the more downstream verification depends on it being correct.

---

## 1. Re-classification (all 47 active items)

### 1.1 Items demoted to NOT-A-BUG

| ID | Original claim | Why it's not a bug |
|----|----------------|--------------------|
| **L-04** | MarkovSwitching xi formula multiplies and divides by emit_j (lines 212-216) | The two cancel exactly. The math is correct. The code is obfuscated but the result is identical to the clean Kim approximation. **Demote to optional cosmetic cleanup** under tech-debt; not a fix. |
| **L-16** | GARCH silently overrides if user provides α+β≥1.0 | Defensive coding — the user's config would crash the optimizer. Override + warning is the standard response. Add a WARN log if not present, but the behavior is correct. |
| **L-17** | EGARCH γ bounds [-1, +1] | γ is theoretically unbounded but practically tiny. Bounded grid search is a calibration choice. Not a math bug. |
| **L-18** | KalmanFilter initial P = I | Default identity prior is the standard convention; user can override via API if needed. Not a bug. |

That's 4 items dropped. **43 remain active.**

### 1.2 Items downgraded to SECOND-ORDER (fixed by another item)

| ID | Original claim | Root cause is | Action |
|----|----------------|---------------|--------|
| **M-02** | BSTS 0% EXP_DIS | M-01 (cross-state std + ±1.5 target scale). M-02 reuses the same target fingerprints (line 715) | Fix M-01; verify M-02 resolves automatically |
| **K-09** | Smoothing init contamination from 5-bar update loop | L-33 (market pipeline lacks warmup). Once warmup is in place, the runner's 5-bar loop becomes a non-issue | Fix L-33; K-09 disappears |
| **K-07** | `liquidity_proxy` default 1.0 mutes STRESS_LIQUIDITY | Same family as L-26 (silent default-to-zero/one when data missing). Both must use NaN/error semantics | Fold K-07 into the L-26 batch fix |
| **L-02** | HMM cov ridge 1e-6 too small | Same root as K-01 (relative σ floor needed in M-step). Different code path, same fix shape | Apply K-01 fix in two places: `markov_switching.cpp:196` AND `hmm.cpp:151` |
| **L-21** | BSTS PCA sign flip | Less critical sibling of L-06; BSTS doesn't label PCA components | Apply same fix pattern as L-06 in `bsts_regime_detector.cpp:434-468`; bundled |

That's 5 items consolidated. **38 remain that need direct attention** (some bundled as small batches).

### 1.3 Items that are real, but where the fix shape needs decision

| ID | Decision required |
|----|-------------------|
| **L-31** | MacroBelief overlay fields (`policy_restrictive`, `credit_tightening`, `inflation_sticky`) — implement detectors, or remove fields? **Recommendation: REMOVE the fields**. The spec says they should come from dedicated overlay detectors that don't yet exist. Leaving the fields silently `false` is worse than having no fields at all (downstream code can't accidentally rely on them). Re-add when detectors are built. |
| **M-06** | BSTS R0-R3 greedy anti-matching label assignment — fix or just relabel? **Recommendation: REJECT NEGATIVE SCORES, mark "Unclassified"**. The pipeline uses cluster posteriors (not these labels), so the cost of accuracy is zero, and the diagnostic dashboard becomes honest. |
| **M-08** | DFM labels services-blind industrial weakness as RECESSION — relabel, or add services proxies? **Recommendation: relabel for now** ("INDUSTRIAL_WEAKNESS_*" or document the bias). Adding ISM-services to the DFM panel changes the factor structure and requires re-fitting Gaussians; that's Phase 3+ work. The relabel is reversible. |
| **L-31** vs **K-13** | If GARCH probability adjustments are kept additive (K-13's "leave it") then no work; if converted to log-odds, plan for renormalization-aware tests. **Recommendation: leave additive** (author themselves said "should still work after normalize but brittle"). Cosmetic, not fixing. **Demote to NOT-A-BUG**. |

That's another 1 demotion (K-13). **42 active items remain after analysis.**

### 1.4 Final active list (post-analysis)

37 items go through implementation phases, grouped by root-cause dependency:

```
PHASE 0 (substrate prerequisites)               — 13 items
PHASE 1 (macro pipeline correctness)            —  5 items
PHASE 2 (market pipeline data + EM)             —  6 items
PHASE 3 (economic refinement)                   —  9 items (some optional)
PHASE 4 (polish + ops)                          —  9 items
PHASE 5 (tests)                                 —  1 meta-item
```

(Total = 43 line entries; some Phase 3/4 items can be parallelized or deferred without breaking the pipeline.)

---

## 2. Phase 0 — Substrate prerequisites (Phase A: data+state honesty)

These are the items where every later phase's verification depends on them. **No M-/K- fix is meaningfully testable until Phase 0 lands.** Estimated 2-3 days.

### 2.1 P0-1 / L-19 — Drop `backward_fill()` from BSTS training

**Classification**: ROOT (real bug — lookahead)
**File**: `src/statistics/state_estimation/bsts_regime_detector.cpp:113-122, 243-247, 848, 850, 984`

**Solution**:
1. Remove call sites at lines **848, 850, 984** (only forward_fill remains).
2. Inline call at lines **243-247** in `fit_series` — keep the forward-fill loop, delete the backward-fill loop.
3. Delete the `backward_fill()` definition entirely (lines 113-122).
4. If a series has leading NaN before its first release, decide one of:
   - (a) drop those leading rows from the panel
   - (b) initialize Kalman filter from the first valid observation onward
5. Add a comment at the top of the file: `// FORWARD-FILL ONLY: legitimate carry-forward of last released value. Backward-fill removed 2026-04-28 (lookahead bias).`

### 2.2 P0-2 / L-20 — Make `gaussian_smooth()` causal by default

**Classification**: ROOT (real bug — lookahead)
**File**: `src/statistics/state_estimation/bsts_regime_detector.cpp:280-298, 408-413`; header `:34-35`

**Solution**:
- **Decision**: change kernel from symmetric `[-radius, +radius]` to trailing `[-2*radius, 0]`. This preserves the same effective averaging window while using only causal data.
- Update the function body:
  ```cpp
  // Causal Gaussian smoothing: kernel over [-2*radius, 0]
  std::vector<double> kernel;
  double ks = 0;
  for (int k = -2 * radius; k <= 0; ++k) {
      double v = std::exp(-(double)(k*k) / (2.0 * sigma * sigma));
      kernel.push_back(v); ks += v;
  }
  for (auto& v : kernel) v /= ks;

  Eigen::VectorXd out(n);
  for (int i = 0; i < n; ++i) {
      double acc = 0;
      for (int k = -2 * radius; k <= 0; ++k)
          acc += kernel[k + 2 * radius] * x(std::clamp(i + k, 0, n - 1));
      out(i) = acc;
  }
  ```
- Default `radius` stays at 4 (now means "trailing 8 days") — keeps existing calibration intent without lookahead.
- Add a config field `causal_smoothing = true` for transparency; leave at true.
- Document the change in `regime_detection_architecture.md`.

### 2.3 P0-3 / L-06 — Lock DFM PCA factor signs to economic anchors

**Classification**: ROOT (silent reproducibility bug)
**File**: `src/statistics/state_estimation/dynamic_factor_model.cpp:458-462`

**Solution**:
- Add config fields `factor_anchor_idx[]` and `factor_anchor_sign[]` to `DFMConfig`. Defaults set so that:
  - Factor 0 anchor: GDP column (positive load)
  - Factor 1 anchor: capacity_utilization (positive load → recession when factor is HIGH)
  - Factor 2 anchor: WTI crude (negative load → high factor 2 = disinflationary)
  Each anchor pinned to match the comment block at lines 540-585.
- After PCA in `initialise_parameters`:
  ```cpp
  for (int k = 0; k < K; ++k) {
      int anchor_idx = config_.factor_anchor_idx[k];
      int target_sign = config_.factor_anchor_sign[k];
      if (lambda_(anchor_idx, k) * target_sign < 0)
          lambda_.col(k) *= -1;
  }
  ```
- Then in `macro_regime_pipeline.cpp`, the hardcoded `growth_factor_sign = -1` and `inflation_factor_sign = -1` become **redundant** and can be removed (signs are now stable by construction).
- Bonus: add a unit test that re-fits DFM 10 times and asserts that the per-regime Gaussian means stay within ε.

### 2.4 P0-4 / L-21 — Lock BSTS PCA signs

**Classification**: ROOT (less impact; same family as P0-3)
**File**: `bsts_regime_detector.cpp:434-468`

**Solution**: After eigendecomposition, flip each component if its largest-magnitude entry is negative:
```cpp
for (int k = 0; k < n_comp; ++k) {
    Eigen::Index max_abs_idx;
    vecs.col(k).cwiseAbs().maxCoeff(&max_abs_idx);
    if (vecs(max_abs_idx, k) < 0) vecs.col(k) *= -1;
}
```
Less load-bearing than P0-3 because BSTS doesn't label PCA components, but stabilizes cluster centers across re-runs.

### 2.5 P0-5 / L-05 — Pairwise complete-case PCA covariance

**Classification**: ROOT (silent bias)
**File**: `dynamic_factor_model.cpp:451-452`

**Solution**: Replace
```cpp
Eigen::MatrixXd C = (Y_clean.transpose() * Y_clean) / static_cast<double>(T - 1);
```
with explicit pairwise count tracking:
```cpp
Eigen::MatrixXd C = Eigen::MatrixXd::Zero(N, N);
Eigen::MatrixXi cnt = Eigen::MatrixXi::Zero(N, N);
for (int t = 0; t < T; ++t) {
    for (int i = 0; i < N; ++i) {
        if (!std::isfinite(Y_std(t, i))) continue;
        for (int j = i; j < N; ++j) {
            if (!std::isfinite(Y_std(t, j))) continue;
            C(i, j) += Y_std(t, i) * Y_std(t, j);
            cnt(i, j)++;
        }
    }
}
for (int i = 0; i < N; ++i) for (int j = i; j < N; ++j) {
    if (cnt(i, j) > 1) C(i, j) /= (cnt(i, j) - 1);
    else C(i, j) = (i == j) ? 1.0 : 0.0;
    if (i != j) C(j, i) = C(i, j);
}
```
Drop the NaN→0 fill at line 449 — no longer needed.

### 2.6 P0-6 batch / L-26 + L-23 + L-34 + L-35 + K-07 — Replace silent zero/partial defaults with NaN or error

**Classification**: ROOT (5 instances of the same anti-pattern)

| File:line | Replace |
|-----------|---------|
| `market_data_loader.cpp:33` | `returns.push_back(0.0)` → `std::numeric_limits<double>::quiet_NaN()` |
| `market_data_loader.cpp:192,209` | Init `composite_returns` to NaN; emit warn if any cell stays NaN after loop |
| `macro_data_loader.cpp:213` | `std::stod(val)` → `std::stod(val, &consumed); if (consumed != val.size()) WARN(...)` |
| `market_regime_pipeline.cpp:642-644` | `garch_vol[t] : 0.0` → error if size mismatch |
| `market_regime_pipeline_runner.cpp:570-576` | Single-symbol sleeves: error or skip GMM col 4, NOT zero-fill |
| `market_regime_pipeline.cpp:437-442` (K-07) | `liquidity_proxy = 1.0` default → `quiet_NaN`; gate downstream stress adjustments on `std::isfinite()` |

Downstream models (HMM, MSAR, GMM) need NaN-skip logic — fold into Phase 2 EM cleanup.

### 2.7 P0-7 / L-22 — Thread-safe gmtime in macro loader

**Classification**: ROOT
**File**: `macro_data_loader.cpp:161`

**Solution**: replace `std::gmtime(&t)` with platform-conditional:
```cpp
std::tm tm_local;
#ifdef _WIN32
gmtime_s(&tm_local, &epoch_seconds);
#else
gmtime_r(&epoch_seconds, &tm_local);
#endif
char buf[11];
std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_local);
```

### 2.8 P0-8 / L-24 — `load_single()` window-back forward-fill

**Classification**: ROOT for live deployment
**File**: `macro_data_loader.cpp:288-311`

**Solution**: change implementation to query a 90-day window ending at the requested date, run forward-fill on the result, return the last row. Public API unchanged.

### 2.9 P0-9 / L-30 — Reset `last_belief_` on retrain

**Classification**: ROOT
**Files**: `macro_regime_pipeline.cpp:217-222`, `market_regime_pipeline.cpp:650-652`

**Solution**: add to both `train()` resets:
- Macro: `last_belief_ = MacroBelief{};`
- Market: `sleeve_states_[s].last_belief = MarketBelief{};`

### 2.10 P0-10 / L-31 — Remove unused MacroBelief overlay fields

**Classification**: ROOT (silent always-false)
**File**: `include/trade_ngin/statistics/state_estimation/macro_regime_pipeline.hpp` (struct definition)

**Solution**: delete `bool policy_restrictive`, `bool credit_tightening`, `bool inflation_sticky` fields. Keep `bool structural_break_risk` (it IS populated). When detectors are built later, add the fields back along with their populating logic. Update spec docs to mark these overlays as "deferred."

---

## 3. Phase 1 — Macro pipeline correctness (5 items, ~2 days)

**Prerequisite**: Phase 0 complete. After Phase 0, the pipeline trains on lookahead-free, sign-stable, NaN-correct features.

### 3.1 P1-1 / M-04 — DFM Gaussian to 2D (drop factor 0 from classifier only)

**Classification**: ROOT
**Files**: `macro_regime_pipeline.cpp:232-354` (train), `:336-354` (map_dfm), header `:189-195`

**Solution**:
- Change `Eigen::Vector3d` to `Eigen::Vector2d` in `dfm_gaussians_[r].mean` and `cov`.
- In `train_dfm_gaussians`, accumulate from `F.row(idx).segment(1, 2)` (skip factor 0).
- In `map_dfm`, take `f_t.segment(1, 2)`.
- Keep DFM decomposition at K=3 — MS-DFM still uses all 3 factors.
- Remove the `0.5 * I` ridge if 2D Gaussians are well-separated (verify in test, may need to keep).

### 3.2 P1-2 / M-01 — Drop cross-state std (root of MS-DFM and BSTS lock-in)

**Classification**: ROOT (also resolves M-02 automatically)
**File**: `macro_regime_pipeline.cpp:538-561` (MS-DFM), `:689-712` (BSTS, same code shape)

**Solution**: Remove the cross-state standardization block entirely. Native fingerprints are already in z-score units after `prepare_fingerprint_data` (line 398-462). The cross-state /std step amplifies tiny offsets to ±1.4. Keep the de-mean step (subtract `ns_mean`) so the targets centered at 0 line up with the natives; delete the /std step.

```cpp
// Stage 2: cross-state DE-MEAN ONLY (no std rescaling)
{
    Eigen::VectorXd ns_mean = Eigen::VectorXd::Zero(kFingerprintDim);
    for (int j = 0; j < J; ++j) ns_mean += msdfm_mapping_.native_fingerprints[j];
    ns_mean /= J;
    for (int j = 0; j < J; ++j)
        msdfm_mapping_.native_fingerprints[j] -= ns_mean;
}
```

If after this, native fingerprints still cluster near zero relative to ±1.5 targets, **also rescale targets to ±0.8** (analysis doc M-01 alternative).

### 3.3 P1-3 (auto, M-02) — verify

After P1-2, BSTS at line 689-712 inherits the fix (same code pattern). Verify MS-DFM dominant pick distribution is no longer 90% EXP_DIS. Verify BSTS dominant pick now includes EXP_DIS > 0.

### 3.4 P1-4 / M-03 — Z-score growth_score components before averaging

**Classification**: ROOT (variable-scale arithmetic bug)
**File**: `bsts_regime_detector.cpp:366-378`

**Solution**:
```cpp
// Before averaging, z-score each component using training-time stats.
// Stats are stored as members; recompute periodically (see L-32).
double ip = (mslope("industrial_production", t) - ip_mean_) / ip_std_;
double cu = (mlevel("manufacturing_capacity_util", t) - cu_mean_) / cu_std_;
double gd = (mslope("gdp", t) - gd_mean_) / gd_std_;
const double gs = (ip + cu + gd) / 3.0;
```

Add training-time computation of the means/stds (one pass over the panel during fit).

### 3.5 P1-5 / M-05 — Inflation_score uses YoY changes for CPI/PCE

**Classification**: ROOT (economic correctness)
**File**: `bsts_regime_detector.cpp:372-373`

**Solution**: 
```cpp
const double is = (
    mslope("cpi", t) +              // YoY change
    mslope("core_pce", t) +         // YoY change
    mlevel("breakeven_5y", t)       // keep as level (forward-looking)
) / 3.0;
```
With z-scoring like P1-4 above.

---

## 4. Phase 2 — Market pipeline data + EM robustness (6 items, ~1 day)

**Prerequisite**: Phase 0 complete (NaN semantics correct).

### 4.1 P2-1 / K-02 — Retrain market pipeline on 2020-2025

**Classification**: ROOT (data scarcity)
**File**: `apps/macro/market_regime_pipeline_runner.cpp:525-526`

**Solution**: Change defaults: `start_date = "2020-01-01"`, `end_date = "2025-12-31"`. Document in CLI help. T grows from ~311 to ~1500.

### 4.2 P2-2 batch / K-01 + L-01 + L-02 + L-03 — EM robustness in HMM and MarkovSwitching

**Classification**: ROOT (multiple instances, same shape)

| File:line | Fix |
|-----------|-----|
| `markov_switching.cpp:196` | `state_variances_(k) = max(σ², 0.01 * global_var) + 1e-6` (relative floor) |
| `hmm.cpp:121, 134, 142` | Wrap M-step updates in `if (gamma_sum > 1e-10) { ... }` |
| `hmm.cpp:151` | Same relative ridge as markov_switching: `1e-6 + 0.01 * mean_diagonal` |
| `hmm.cpp:323-328` | `D.array().abs().max(1e-300).log().sum()` |

### 4.3 P2-3 / K-03 — GMM feature col 3 mismatch

**Classification**: ROOT (naming/feature)
**Files**: `market_regime_pipeline_runner.cpp:164-169`, `market_regime_pipeline.cpp:491-495`

**Solution**: rename column 3 from `vol_shock` to `volume_ratio` AND retune target fingerprints in market_regime_pipeline.cpp:491-495 to reflect actual semantics (volume drops in stress, surges in shock — different sign).

### 4.4 P2-4 / L-33 (subsumes K-09) — Market pipeline warmup

**Classification**: ROOT (structural fix; K-09 disappears)
**File**: `market_regime_pipeline.cpp:557-575`

**Solution**: Mirror macro warmup logic. Add per-sleeve `int update_count_ = 0;` counter. In `smooth()`:
```cpp
constexpr int kWarmupSteps = 10;
double lam = (state.update_count < kWarmupSteps) ? 1.0 : config_.lambda;
state.update_count++;
// ... rest unchanged
```
Reset `update_count = 0` in `train()`.

### 4.5 P2-5 / L-34 — covered in P0-6 batch
### 4.6 P2-6 / L-35 — covered in P0-6 batch

---

## 5. Phase 3 — Economic refinement (9 items, ~2-3 days)

**Prerequisite**: Phases 0-2 complete. Pipeline now reports consistent regimes; this phase improves quality.

### 5.1 P3-1 / M-08 — Relabel DFM "RECESSION_*" pending services data

**Classification**: ROOT (decision item from §1.3)
**Files**: `regime_aware_portfolio_engine.md`, label strings in `kRegimeNames[]` at `macro_regime_pipeline.cpp:21-28`, downstream consumers

**Solution**: Rename to `INDUSTRIAL_WEAKNESS_DEFLATIONARY` and `INDUSTRIAL_WEAKNESS_INFLATIONARY` for now. Document the bias in spec. Re-introduce true `RECESSION_*` semantics when ISM-services or PCE-services is added to the macro panel and the DFM is re-fit.

### 5.2 P3-2 / M-06 — BSTS R0-R3 reject negative scores

**Classification**: ROOT (decision item from §1.3, diagnostic-only)
**File**: `bsts_regime_detector.cpp:693-712`

**Solution**: After greedy assignment, if score ≤ 0, mark cluster as `"Unclassified"`. No effect on pipeline (uses cluster posteriors), only affects diagnostic dashboard output.

### 5.3 P3-3 / M-07 — MS-DFM soft-prob-weighted fingerprints

**Classification**: ROOT (train/runtime asymmetry)
**File**: `macro_regime_pipeline.cpp:517` (training), `:616-618` (runtime, already correct)

**Solution**: Replace hard-decoded argmax in fingerprint training with soft-prob weighting:
```cpp
// Old: indices.push_back(t) if decoded_regimes[t] == j
// New: weighted aggregation over all timesteps
double weight_sum = 0;
Eigen::VectorXd weighted_fp = Eigen::VectorXd::Zero(kFingerprintDim);
for (int t = 0; t < T; ++t) {
    double w = msdfm_output.smoothed_probs(t, j);
    if (w < 1e-6) continue;
    weighted_fp += w * compute_fingerprint(panel, {t});
    weight_sum += w;
}
msdfm_mapping_.native_fingerprints[j] = weighted_fp / weight_sum;
```

### 5.4 P3-4 / K-04 — HMM trend dim use |μ| instead of signed μ

**Classification**: ROOT (economic — downtrends mislabeled stress)
**File**: `market_regime_pipeline.cpp:242-265`

**Solution**: In HMM fingerprint construction, replace `μ_j` with `|μ_j|` (or add absolute value as a separate dim). Update target fingerprints accordingly: TREND_LOWVOL = `|+1.5|`, STRESS_PRICE no longer `-1.0`. Detailed retuning required.

### 5.5 P3-5 / K-05 — Add liquidity proxy to HMM 3rd dim

**Classification**: ROOT (depends on P0-6 / K-07 completing first)
**File**: `market_regime_pipeline.cpp:260-264`

**Solution**: Once `liquidity_proxy` is reliably NaN-aware (P0-6), expand HMM fingerprint to 3D `[μ, σ, liq]`. Update target fingerprints. STRESS_PRICE = `[-1, +1.5, 0]`, STRESS_LIQUIDITY = `[-0.5, +2, -1.5]` — now distinguishable.

### 5.6 P3-6 / K-06 — MSAR transition matrix consistency

**Classification**: ROOT (internal inconsistency)
**File**: `apps/macro/market_regime_pipeline_runner.cpp:420-423`

**Solution**: Either expose `MarketMSAR::get_transition_matrix()` and use that consistently in the runner, OR re-fit `MarkovSwitching` with AR-residualized returns. Recommend the former — single source of truth.

### 5.7 P3-7 / K-08 — Cross-asset correlation pooled across sleeves

**Classification**: ROOT (correlation signal currently useless)
**File**: `apps/macro/market_regime_pipeline_runner.cpp:569-576`

**Solution**: Pool composite returns across all sleeves into one global matrix (concat columns). Compute rolling correlation z-score on that pooled matrix. Replace per-sleeve corr_spike with this global signal.

### 5.8 P3-8 / K-10 — Per-sleeve target fingerprints

**Classification**: ROOT (missed customization)
**File**: `market_regime_pipeline.cpp:260-264, 332-336, 491-495` and `SleeveConfig`

**Solution**: Move target fingerprints from hardcoded function-local Eigen vectors into `SleeveConfig` struct fields. Provide sane sleeve-specific defaults from analysis doc Appendix 6.4.

### 5.9 P3-9 / L-09 — GARCH/EGARCH update() apply demean

**Classification**: ROOT (API contract)
**Files**: `garch.cpp:38-41, 268-274`, `egarch.cpp:71-73, 254-262`

**Solution**: Store `mean_return_` as a member, set in `fit()`, apply in `update(new_return) { residuals_.push_back(new_return - mean_return_); ... }`. Document the contract.

### 5.10 P3-10 / L-32 — Quadrant z-score recalibration

**Classification**: ROOT (live drift)
**File**: `macro_regime_pipeline.cpp:196-203, 635-636`

**Solution**: Replace fixed `growth_median_/std_` with rolling 252-day window stats computed in `update()`. Or expose `recalibrate_quadrant_stats(panel)` as an explicit refresh API. Recommend the rolling-window approach — no operator action needed.

---

## 6. Phase 4 — Polish + ops (9 items, ~1-2 days)

These don't block downstream; can be picked off in any order or parallelized.

| ID | Fix | Where |
|----|-----|-------|
| **L-07** | Joseph form Kalman: `P = (I-KH) P (I-KH)' + K R K'` | `kalman_filter.cpp:127`, `dynamic_factor_model.cpp:287, 354, 397` |
| **L-08** | Document or unify `filter()` vs `update()` state | `dynamic_factor_model.cpp:316-358` (add doc comment, OR make filter() update internal state) |
| **L-12** | Replace JacobiSVD condition check with LLT diag min/max | `kalman_filter.cpp:104-110` |
| **L-13** | Permute `regime_labels` in `order_regimes_by_volatility` | `ms_dfm.cpp:299-352` |
| **L-10** | GMM K=1 entropy NaN guard | `gmm.cpp:177` (`if (K > 1) ...`) |
| **L-11** | GMM rng deterministic per restart | `gmm.cpp:144` (`rng.seed(seed + restart * 1009)`) |
| **L-25** | Market loader `timegm()` for UTC dates | `market_data_loader.cpp:51-58` (POSIX `timegm`, Windows `_mkgmtime`) |
| **L-28** | Guard division by zero on `min_dwell_bars=0` | `macro_regime_pipeline.cpp:858` (wrap whole block in `if (min_dwell_bars > 0)`) |
| **L-29** | Race condition: lock first, check trained_ second | macro `:924-929`, market `:672-678` |
| **K-11** | Print `stability` alongside confidence in runner | `market_regime_pipeline_runner.cpp` `print_belief()` |
| **K-15** | Fix printf bugs: rates MSAR σ missing, FX label typo | runner output format |
| **K-12** | EGARCH γ significance via standard error | runner line 501 |
| **K-14** | Incremental drawdown_speed (running cumsum/peak) | runner line 198-203 |
| **K-16** | corr_spike z-score window adapts to T | runner line 255 |
| **M-09** | MacroBelief: add `entropy_concentration` and `top_prob` fields | `macro_regime_pipeline.cpp:903-910` + struct |
| **M-10** | enter_riskoff threshold raise (only if M-01 fix doesn't already help) | hpp:148-153 — **measure first, do not change blindly** |
| **M-11, M-12, M-13** | Explore cov floor sensitivity, dwell penalty, warmup duration | calibration sweep, not code change |
| **K-17** | Persistent state serialization for live deployment | `SleeveTrainedState::prev_smoothed` |

---

## 7. Phase 5 — Tests (1 meta-item, ~1-2 days)

### L-27 — Add adversarial tests
- Lookahead detection: train on full panel, train on `panel[:-1]`, compare features at last common index — must match.
- σ-collapse: synthetic data with one outlier day; assert no state has σ < 1% of global.
- Sign-flip stability: re-fit DFM 10 times with different seeds, assert per-regime Gaussian means within ε.
- Stress regime: synthetic crash period; assert stress regimes get >30% probability.
- NaN propagation: inject NaN into one column, assert no output is NaN/Inf.
- Non-release-day live update: call `load_single` on a date with no growth release; assert returned vector is mostly finite (forward-filled).

These tests act as regression guards for everything fixed in Phases 0-4.

---

## 8. Sequencing diagram

```
PHASE 0 (substrate) ─────────┐
  L-19 backward_fill          │
  L-20 gaussian causal        │
  L-06 DFM sign lock          │
  L-21 BSTS sign lock         │
  L-05 PCA pairwise cov       │
  L-26+L-23+L-34+L-35+K-07    │
       NaN/error semantics    │
  L-22 gmtime_r               │
  L-24 load_single window     │
  L-30 reset last_belief      │
  L-31 remove unused fields   │
                              │
PHASE 1 (macro correctness) ──┤
  M-04 DFM 2D Gaussian        │
  M-01 drop cross-state std   ──→ verifies M-02 auto-resolves
  M-03 z-score growth comp    │
  M-05 YoY inflation comp     │
                              │
PHASE 2 (market data + EM) ───┤
  K-02 2020-2025 retrain      │
  K-01+L-01+L-02+L-03 EM      │
  K-03 GMM col 3 rename       │
  L-33 market warmup          ──→ verifies K-09 auto-resolves
                              │
PHASE 3 (economic) ───────────┤
  M-08 relabel recession      │
  M-06 reject neg R0-R3       │
  M-07 soft-prob fingerprints │
  K-04 |μ| trend dim          │
  K-05 liquidity HMM dim      │
  K-06 MSAR transition fix    │
  K-08 pooled cross-asset     │
  K-10 per-sleeve targets     │
  L-09 GARCH demean           │
  L-32 rolling z-score        │
                              │
PHASE 4 (polish) ─────────────┤  (can parallelize across the 17 items)
                              │
PHASE 5 (tests) ──────────────┘
  L-27 adversarial suite
```

---

## 9. Total time estimate

| Phase | Items | Time (sequential) | Time (parallelized) |
|-------|------:|------------------:|--------------------:|
| 0     |    13 | 2-3 days          | 1-2 days            |
| 1     |     5 | 2 days            | 1 day               |
| 2     |     6 | 1 day             | 0.5 day             |
| 3     |    10 | 2-3 days          | 1.5 days            |
| 4     |    17 | 1-2 days          | 0.5-1 day           |
| 5     |     1 | 1-2 days          | 1-2 days            |
| **Total** | **52** | **9-13 days** | **5.5-8 days** |

---

## 10. Risk register

| Risk | Likelihood | Mitigation |
|------|:----------:|------------|
| Phase 0 surfaces unexpected pipeline regressions | Medium | Run macro and market pipelines end-to-end after each P0 item, compare regime distributions before/after |
| M-04 (DFM 2D) produces worse classifications | Low-Medium | Keep 3D as a config flag; A/B compare |
| M-01 drop-cross-state-std reveals a new lock-in pattern | Medium | The fingerprint targets (Appendix 6.3 of analysis doc) may need rescaling; investigate if MS-DFM still picks one regime > 50% |
| K-04 retuning breaks existing market regime distributions | Medium | Same A/B approach — keep old fingerprints behind config flag |
| Test suite (Phase 5) catches additional issues | High (welcome) | This is a feature, not a risk — adversarial tests should expose any remaining bugs |
| Phase 0 timezone/date fix breaks existing CSV outputs | Low | Date format unchanged; only the conversion path changes |

---

---

## 11. Verification protocol (after every fix, every phase, and end-to-end)

### 11.1 After each individual fix (per-item)

1. **Build clean** — full `cmake --build` from a clean state. No new warnings or errors.
2. **Targeted unit test** — run the test for the specific file/model touched (e.g., after L-19 run `test_macro_regime_pipeline`, `test_bsts_regime_detector` if it exists).
3. **Diagnostic delta** — run the affected pipeline runner. Capture the per-model contribution table (`belief.model_contributions`) before and after. Diff:
   - DFM regime distribution
   - MS-DFM dominant pick distribution (per-regime histogram across the full history)
   - BSTS dominant pick distribution
   - Quadrant dominant pick distribution
   - Final ensemble dominant + confidence
4. **Side-effect check** — for any item I classified as second-order to this fix, verify it actually went away:
   - After M-01 (cross-state std drop) → verify M-02 (BSTS 0% EXP_DIS) is now resolved
   - After L-33 (market warmup) → verify K-09 (smoothing init contamination) is gone
   - After L-26 batch (NaN semantics) → verify K-07 (liquidity_proxy mute) is now gated correctly
   - After L-06 (DFM sign lock) → verify the hardcoded `growth_factor_sign = -1` in pipeline can be removed safely (re-fit DFM 5x, factor signs stable)
5. **Log entry** — append a one-paragraph entry to a running `docs/REGIME_PIPELINE_FIX_LOG.md` (created on first fix). Format:
   ```
   ### 2026-04-28 — P0-1 / L-19 (drop backward_fill)
   File: src/statistics/state_estimation/bsts_regime_detector.cpp
   Verified: macro pipeline runs end-to-end. MS-DFM 90% EXP_DIS lockin
   unchanged (expected — that's M-01). DFM regime histogram shifted by
   <0.5% per bucket — within expected noise from re-fitting on
   forward-filled-only data.
   ```

### 11.2 After each phase

Run a full pipeline regression:

1. **Macro pipeline runner** end-to-end on full panel (2011-01-01 → 2026-04-28).
2. **Market pipeline runner** end-to-end on full panel (2020-01-01 → 2025-12-31, after K-02).
3. **Capture metrics**:
   - Confidence distribution (current-bar + last-30-bars)
   - Stability metric distribution
   - Regime transition counts (how many transitions over the full sample)
   - Per-regime occupancy fraction
4. **Compare to phase baseline** (saved before the phase started). Expected directions:
   - Phase 0: small changes everywhere; no regime should disappear
   - Phase 1: MS-DFM occupancy redistributes from 90% EXP_DIS to balanced; BSTS gains EXP_DIS share; current-bar confidence rises from 0.064 toward 0.15+
   - Phase 2: market sleeves show non-trivial STRESS occupancy during 2020-Q1, 2022-Q4, 2023-Mar; commodities HMM no longer has degenerate state 2
   - Phase 3: regime labels match qualitative economic narrative for known periods (2008-09, 2020, 2022, etc.)
5. **Economic sanity** — for ~10 known historical periods (e.g., 2011 commodity supercycle, 2018 trade war, COVID, 2022 inflation shock, 2023 banking stress), assert the dominant regime label matches the textbook description.

### 11.3 After the whole plan completes (end-to-end validation)

Same regression suite as 11.2, plus:

6. **Walk-forward backtest** — re-train the pipeline at quarterly checkpoints from 2015 onward. At each checkpoint, run forward to the next checkpoint with the model trained only on past data. Verify regime calls match in-sample assignment to within ε. (This is the test that catches lookahead — if there's any future leakage left, walk-forward and full-sample diverge.)
7. **Stress-day detection** — manually inspect 30 highest-vol days across the sample. Pipeline must call STRESS_PRICE or STRESS_LIQUIDITY on at least 25 of them.
8. **Reproducibility** — re-train the pipeline 5 times from scratch with same seed. Per-regime Gaussian means stable to 1e-3.
9. **Live-update simulation** — run `update()` over the last 90 bars, comparing to the batch-trained smoothed_probs at the same indices. Differences should be small and isolated to bars affected by warmup contamination.

If all 9 checks pass, the pipeline is releasable.

---

## 12. Repo reorganization (DEFERRED — execute AFTER all fixes)

The user's call: do the reorg after the fixes so we know what's working before moving files around. I'm capturing the target structure here as a reference.

### 12.1 Current state (the scattering problem)

Regime detection code lives in **at least five directories**:

```
src/statistics/state_estimation/    ← DFM, MS-DFM, HMM, MS, Kalman, EKF, BSTS,
                                       macro/market_data_loader, macro/market_regime_pipeline
src/statistics/volatility/          ← GARCH, EGARCH (used as A3 in market pipeline)
src/statistics/clustering/          ← GMM (used by BSTS and as A4 in market pipeline)
src/models/autoregression/          ← autoregressive.cpp, msar.hpp, autoregressive_debug.cpp
                                       (MarketMSAR — used by market pipeline runner)
src/strategy/                       ← regime_detector.cpp (legacy),
                                       bsts_regime_detection_multiasset.cpp (multi-asset BSTS)
apps/macro/                         ← BOTH macro AND market pipeline runners (market is here!)
deliverables/regime/                ← spec docs (synthesized pipelines, gap analysis)
docs/                               ← analysis and plan docs
```

Specific issues with `src/models/autoregression/`:
- **`msar.hpp` lives in `src/`** instead of `include/trade_ngin/...` — breaks the project's consistent src/include split
- **`autoregressive_debug.cpp` has its own `main()` at line 31** — it's a standalone debug runner that snuck into a library directory; should be in `apps/`
- `autoregressive.cpp` is the actual MarketMSAR implementation; depends on `markov_switching.hpp` (so MSAR builds on top of the basic MS class)

Issues:
1. `market_regime_pipeline_runner.cpp` lives under `apps/macro/` — wrong directory name for what it does.
2. `regime_detector.cpp` and `bsts_regime_detection_multiasset.cpp` in `src/strategy/` likely **dead code** — the canonical impl is in `src/statistics/state_estimation/`. **Verify and delete or document.**
3. `KalmanFilter` (kalman_filter.cpp) is **only referenced by its own test** — BSTS uses its own internal Kalman. Same for ExtendedKalmanFilter likely. Candidates for "general state-space toolkit" or for deletion if unused.
4. No top-level `regime/` module despite this being a major subsystem.
5. Specs in `deliverables/regime/`, analysis in `docs/`, plan in `docs/` — split.

### 12.2 Target structure

```
src/
  regime/                                          ← NEW: top-level regime subsystem
    macro/
      pipeline.cpp                                 ← was state_estimation/macro_regime_pipeline.cpp
      data_loader.cpp                              ← was state_estimation/macro_data_loader.cpp
    market/
      pipeline.cpp                                 ← was state_estimation/market_regime_pipeline.cpp
      data_loader.cpp                              ← was state_estimation/market_data_loader.cpp
    models/
      dfm.cpp                                      ← was state_estimation/dynamic_factor_model.cpp
      ms_dfm.cpp                                   ← was state_estimation/ms_dfm.cpp
      hmm.cpp                                      ← was state_estimation/hmm.cpp
      markov_switching.cpp                         ← was state_estimation/markov_switching.cpp
      msar.cpp                                     ← was models/autoregression/autoregressive.cpp
                                                     (renamed: file is MarketMSAR, not generic AR)
      bsts.cpp                                     ← was state_estimation/bsts_regime_detector.cpp
      bsts_multiasset.cpp                          ← was strategy/bsts_regime_detection_multiasset.cpp
                                                     (multi-asset BSTS, currently in src/strategy/)
    common/
      ontology.hpp                                 ← MacroRegimeL1, MarketRegimeL1, kRegimeNames
      belief.hpp                                   ← MacroBelief, MarketBelief structs
      fingerprint.cpp                              ← shared fingerprint compute (currently inlined in pipeline)
  statistics/                                      ← KEEP: general-purpose stats (not regime-specific)
    volatility/                                    ← GARCH/EGARCH/GJR/DCC stay (general vol models)
    clustering/                                    ← GMM stays (general clustering)
    regression/                                    ← OLS/Ridge/Lasso stay
    state_estimation/                              ← KEEP for general state-space tools:
      kalman_filter.cpp                            ← (decide: keep here or delete if unused)
      extended_kalman_filter.cpp                   ← (decide: keep here or delete if unused)
    statistics_tools.cpp
    statistics_utils.cpp
    validation.cpp
    critical_values.cpp
  strategy/
    _legacy_regime_detector.cpp                    ← was regime_detector.cpp; RENAMED with leading
                                                     underscore + comment header marking it legacy.
                                                     KEPT (not deleted) per user direction —
                                                     superseded by regime/macro/pipeline.cpp but
                                                     retained for reference and possible reuse.
                                                     (Not wired into any current runner.)
    [bsts_regime_detection_multiasset.cpp moves to regime/models/bsts_multiasset.cpp —
     KEPT as part of macro per user direction]

include/trade_ngin/
  regime/                                          ← mirrors src/regime/
    macro/
      pipeline.hpp
      data_loader.hpp
    market/
      pipeline.hpp
      data_loader.hpp
    models/
      dfm.hpp
      ms_dfm.hpp
      hmm.hpp
      markov_switching.hpp
      msar.hpp                                     ← was src/models/autoregression/msar.hpp
                                                     (LIVED IN SRC; moves to proper include/)
      bsts.hpp
      bsts_multiasset.hpp                          ← extract from currently-bundled .cpp
    common/
      ontology.hpp
      belief.hpp
      fingerprint.hpp
  statistics/
    [unchanged]

apps/
  regime/                                          ← NEW: regime runners live here
    macro_pipeline_runner.cpp                      ← was apps/macro/macro_regime_pipeline_runner.cpp
    market_pipeline_runner.cpp                     ← was apps/macro/market_regime_pipeline_runner.cpp
                                                     (FIXED: no longer under macro/)
    dfm_runner.cpp                                 ← was apps/macro/macro_dfm_runner.cpp
    msdfm_runner.cpp                               ← was apps/macro/macro_msdfm_runner.cpp
    msar_debug_runner.cpp                          ← was src/models/autoregression/autoregressive_debug.cpp
                                                     (had its own main(); moves to apps/)
    bsts_runner.cpp                                ← if a standalone runner exists
  macro/                                           ← KEEP for non-regime macro tools (data fetch, etc.)

tests/
  regime/
    macro/
      test_pipeline.cpp
      test_data_loader.cpp
    market/
      test_pipeline.cpp
      test_data_loader.cpp
    models/
      test_dfm.cpp
      test_ms_dfm.cpp
      test_hmm.cpp
      test_markov_switching.cpp
      test_bsts.cpp
    test_correctness_adversarial.cpp               ← from L-27 (Phase 5)
  statistics/                                      ← KEEP for general stats tests

docs/
  regime/                                          ← consolidate all regime docs here
    REGIME_PIPELINE_ANALYSIS.md                    ← move from docs/
    REGIME_PIPELINE_LIBRARY_AUDIT.md
    REGIME_PIPELINE_FIX_PLAN.md                    ← this doc
    REGIME_PIPELINE_FIX_LOG.md                     ← created during fixes
    SYNTHESIZED_MACRO_PIPELINE.md                  ← move from deliverables/regime/
    SYNTHESIZED_MARKET_PIPELINE.md
    MARKET_PIPELINE_GAP_ANALYSIS.md
    regime_detection_architecture.md
    regime_aware_portfolio_engine.md
```

### 12.3 What stays vs. moves vs. legacy-renamed (no deletions)

**Stays in `statistics/` (general-purpose, not regime-specific):**
- `garch.cpp`, `egarch.cpp`, `gjr_garch.cpp`, `dcc_garch.cpp` — vol models, used in regime but also general
- `gmm.cpp` — clustering, used in regime but also general
- `ols`/`ridge`/`lasso` — regression
- `validation.cpp`, `statistics_utils.cpp`, `statistics_tools.cpp`, `critical_values.cpp` — utilities
- **`kalman_filter.cpp` and `extended_kalman_filter.cpp`** — KEEP per user direction.
  Currently only referenced by their own tests, but kept as the project's general
  state-space toolkit for future use. Add a header comment indicating they're
  available as a generic Kalman implementation (BSTS rolls its own internal Kalman
  for tight integration). No deletion.

**Moves to `regime/`:**
- `dynamic_factor_model.*` (only used by macro pipeline)
- `ms_dfm.*` (only used by macro pipeline)
- `bsts_regime_detector.*` (only used by macro pipeline / BSTS module)
- `markov_switching.*` (used by market pipeline as MSAR backend; also by MarketMSAR via include)
- `hmm.*` (only used by market pipeline)
- `src/models/autoregression/autoregressive.cpp` → `regime/models/msar.cpp`
  (rename: the file IS MarketMSAR, not generic AR)
- `src/models/autoregression/msar.hpp` → `include/trade_ngin/regime/models/msar.hpp`
  (the header was misplaced under `src/`, not `include/`)
- `src/models/autoregression/autoregressive_debug.cpp` → `apps/regime/msar_debug_runner.cpp`
  (it has its own `main()` — it's a runner, not library code)
- All four pipeline + data_loader files
- `src/strategy/bsts_regime_detection_multiasset.cpp` → `regime/models/bsts_multiasset.cpp`
  (KEPT per user direction — confirmed as part of macro)

**Renamed to indicate legacy status (not deleted):**
- `src/strategy/regime_detector.cpp` → rename to `_legacy_regime_detector.cpp` (or move to
  a `legacy/` subfolder under `strategy/`). Add a comment header at the top of the file:
  ```cpp
  // ============================================================================
  // LEGACY — NOT IN ACTIVE USE
  // ----------------------------------------------------------------------------
  // This file predates the modern regime pipeline. The canonical regime
  // detector is now src/regime/macro/pipeline.cpp + src/regime/market/pipeline.cpp.
  //
  // Kept for reference. Not wired into any runner. Do not extend.
  // If you reach for this file: use the regime/ pipelines instead.
  // ============================================================================
  ```
  Verify it's removed from CMakeLists.txt builds if it isn't already. The file's
  history is preserved; future readers will see the marker and know to look elsewhere.

**No files are deleted in this reorg.**
- `src/statistics/state_estimation/extended_kalman_filter.cpp` — likely same; verify.

### 12.4 Reorg execution checklist (when ready)

1. **Verify usage** — grep all of `src/`, `include/`, `apps/` for references to:
   - `regime_detector.cpp` → confirm not in active runner; mark legacy (don't delete)
   - `bsts_regime_detection_multiasset.cpp` → identify which runner uses it; treat as part of macro
   - `KalmanFilter`, `ExtendedKalmanFilter` → confirm used only by tests; KEEP as general toolkit
   - `MarketMSAR`, `models/autoregression` → confirm market pipeline runner is the only consumer
2. **Move files** in one PR per directory rename, using `git mv` to preserve history. Suggested order:
   - PR 1: pipelines + data loaders → `src/regime/macro/`, `src/regime/market/`
   - PR 2: models → `src/regime/models/` (DFM, MS-DFM, HMM, MS, BSTS)
   - PR 3: autoregression module → `src/regime/models/msar.cpp` + proper header location;
     `autoregressive_debug.cpp` → `apps/regime/msar_debug_runner.cpp`;
     `bsts_regime_detection_multiasset.cpp` → `regime/models/bsts_multiasset.cpp`
   - PR 4: legacy markers — rename `regime_detector.cpp` to `_legacy_regime_detector.cpp` with
     comment header; verify not in CMakeLists active build
   - PR 5: extract common types (ontology, belief structs) → `src/regime/common/`
   - PR 6: runners → `apps/regime/`
   - PR 7: tests → `tests/regime/`
   - PR 8: docs → `docs/regime/`
3. **Update CMakeLists.txt and includes** at each step. Keep the build green between PRs.
4. **Update specs** to point to new file paths (the existing specs cite e.g. `src/statistics/state_estimation/macro_regime_pipeline.cpp` — update when files move).
5. **Run the full Phase 5 adversarial test suite** between each PR. If anything breaks, the move surfaced a hidden dependency.

### 12.5 Why this organization is better

- **One home for the regime subsystem**: `src/regime/`, `include/trade_ngin/regime/`, `apps/regime/`, `tests/regime/`, `docs/regime/` — all consistent.
- **Separation of concerns within regime**: `pipelines/` orchestrate, `models/` are statistical building blocks, `common/` is shared types, `data_loader/` is I/O.
- **Statistics library stays general**: GARCH, GMM, regression, validation utilities don't get pulled into a regime-specific module just because the regime pipeline happens to use them. This makes them reusable for other strategies (vol-targeting, factor models, classification).
- **Apps directory accurately named**: `apps/regime/market_pipeline_runner.cpp` is unambiguously a regime tool. No more "market runner under macro/".
- **Discoverability**: a new contributor sees `src/regime/` and immediately understands the subsystem boundaries.

### 12.6 Why deferred

Doing the reorg before the fixes risks:
- File moves obscuring git blame for in-flight bug fixes
- Each fix touching multiple file paths during the move
- Confusing fix log entries (line numbers shift constantly)
- Premature commitment to a structure before knowing which files are actually load-bearing

After the fixes complete, we know:
- Which files survived (some Strategy/ orphans may delete)
- Which model files are actually wired in (KalmanFilter unused → delete or relocate)
- What the final API surface looks like (some structs may grow/shrink during fixes)

So reorg happens **between Phase 5 (tests) and any further feature work** (e.g., the Phase 2 spec items: MBFS, ML confirmer, cross-asset overlays).

---

*End of plan. Open for review. Once approved, begin with Phase 0 P0-1 (drop backward_fill).*
