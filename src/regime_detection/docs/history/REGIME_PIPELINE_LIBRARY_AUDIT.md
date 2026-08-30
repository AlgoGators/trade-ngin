# Regime Pipeline — Library Audit (L-# Backlog)

**Audit date**: 2026-04-27
**Branch**: `regime_detection_updated`
**Scope**: Underlying numerical libraries beneath the regime pipeline (not the pipeline itself)
**Companion to**: `docs/REGIME_PIPELINE_ANALYSIS.md` (M-01..M-13, K-01..K-17)

## 0. Why this doc exists

The 30-item backlog in `REGIME_PIPELINE_ANALYSIS.md` is a **pipeline-level** audit. It identifies lock-ins (M-01), labeling bugs (M-06), feature mismatches (K-03), and so on — all in `macro_regime_pipeline.cpp` and `market_regime_pipeline.cpp`.

But the pipeline rests on a stack of shared numerical libraries:
- `src/statistics/state_estimation/dynamic_factor_model.cpp` (DFM — also feeds B1)
- `src/statistics/state_estimation/ms_dfm.cpp` (MS-DFM — feeds B2)
- `src/statistics/state_estimation/hmm.cpp` (HMM — separate from MarkovSwitching)
- `src/statistics/state_estimation/markov_switching.cpp` (MarketMSAR + MarketHMM EM)
- `src/statistics/state_estimation/kalman_filter.cpp` (used by BSTS for per-series Kalman)
- `src/statistics/volatility/garch.cpp`, `egarch.cpp` (A3 vol input)
- `src/statistics/clustering/gmm.cpp` (used by BSTS clustering and A4 GMM mapping)

This doc audits those libraries for issues **not captured by the pipeline-level backlog** but that propagate up into the pipeline's outputs. The same rule applies: spec-vs-code divergence is **not** automatically a fix — only flag math errors, economically incorrect behavior, and silent failure modes.

---

## 1. Active Fixes (math broken, silent failures, or correctness bugs)

### L-01 — HMM M-step has no zero-gamma guard

**File**: `src/statistics/state_estimation/hmm.cpp:121, 134, 142`

The HMM EM updates run unguarded division:
```cpp
transition_matrix_(i, j) = xi_sum / row_sum;       // line 121
means_[k] /= gamma_sum;                            // line 134
covariances_[k] /= gamma_sum;                      // line 142
```

If a state collects zero posterior in any iteration, every entry becomes NaN/Inf and propagates through the whole filter.

The peer file `markov_switching.cpp:188, 207` has proper `if (gamma_sum > 1e-10)` guards.

**Fix**: Add `if (gamma_sum > 1e-10)` guards around all three M-step updates.
**Severity**: High — silent NaN propagation.

### L-02 — HMM covariance ridge is absolute, not relative to data scale

**File**: `src/statistics/state_estimation/hmm.cpp:151`

```cpp
covariances_[k] += Eigen::MatrixXd::Identity(D, D) * 1e-6;
```

For demeaned daily returns (~0.01 std), 1e-6 is well below noise. Same shape as **K-01** but for the HMM class, not MarkovSwitching — used by separate code paths.

**Fix**: `cov_floor = max(1e-6, 0.01 * mean_diagonal)` — same pattern as K-01.
**Severity**: Medium.

### L-03 — LDLT log-determinant fallback can produce -inf

**File**: `src/statistics/state_estimation/hmm.cpp:323-328`

```cpp
double log_det = ldlt.vectorD().array().abs().log().sum();
```

No floor before `log()`. If any LDLT D-diagonal entry is exactly 0, `log(0) = -inf` propagates into emission probabilities.

**Fix**: `D.array().abs().max(1e-300).log().sum()`.
**Severity**: Medium.

### L-04 — MarkovSwitching xi formula has redundant emit_j cancellation

**File**: `src/statistics/state_estimation/markov_switching.cpp:212-216`

```cpp
double emit_j = std::exp(emission_log_prob(data[t + 1], j));
xi_sum += smoothed(t, i) * transition_matrix_(i, j) * emit_j *
          smoothed(t + 1, j) / (pred_j * std::max(emit_j, 1e-300));
```

The `emit_j` multiplies and then divides — they cancel mathematically. But if `emit_j < 1e-300`, the `max()` floor takes over and the ratio becomes ~1, producing the wrong xi.

The clean form (already implemented correctly in `ms_dfm.cpp:280-281`):
```cpp
xi_sum += smoothed(t, i) * P_(i, j) * smoothed(t + 1, j) / pred_j;
```

**Fix**: Remove the emit_j multiplication/division. Port the clean MS-DFM form.
**Severity**: Medium — correct in normal operation, brittle near boundaries.

### L-05 — DFM PCA covariance biased by NaN→0 fill

**File**: `src/statistics/state_estimation/dynamic_factor_model.cpp:451-452`

```cpp
// Line 449: NaN replaced with 0
Eigen::MatrixXd C = (Y_clean.transpose() * Y_clean) / static_cast<double>(T - 1);
```

After standardization, NaNs become 0s (column mean post-standardize). The covariance computation then divides by `T-1` regardless of how many cells were originally NaN. Series with many missing values get covariance biased toward zero — Lambda init is wrong for incomplete columns.

The macro panel routinely has missing data (different release schedules — GDP quarterly, NFP monthly, daily series).

**Fix**: Use pairwise-complete-case covariance, scaling each pair (i,j) by the count of timesteps where both are observed.
**Severity**: High — biased Lambda init affects EM convergence basin and final factor structure.

### L-06 — DFM PCA factor signs are arbitrary; pipeline relies on hardcoded sign flips

**File**: `src/statistics/state_estimation/dynamic_factor_model.cpp:458-462`

PCA-based Lambda init uses `evec * sqrt(eigenvalue)`. **Eigenvector signs are arbitrary** — re-fits flip them.

The pipeline at `macro_regime_pipeline.cpp` applies hardcoded `growth_factor_sign = -1`, `inflation_factor_sign = -1` based on the *current* fit's sign convention. **If a re-fit flips factor signs, the hardcoded flips become wrong**, silently inverting growth/inflation labeling.

The author of `dynamic_factor_model.cpp` even acknowledges this risk in a comment block at line 583-585.

**Fix**: Lock factor signs to dominant-loading economic interpretation, e.g.:
```cpp
// After PCA, flip eigenvector if dominant economic anchor loads negatively
// Example: factor 1 (real_activity) — flip if cap_util loading > 0
//          (so high factor 1 = weak activity)
for (int k = 0; k < K; ++k) {
    int anchor_idx = config_.factor_anchor_idx[k];   // configurable
    int anchor_sign = config_.factor_anchor_sign[k]; // +1 or -1
    if (lambda_(anchor_idx, k) * anchor_sign < 0)
        lambda_.col(k) *= -1;
}
```

Then the pipeline doesn't need hardcoded sign flips.

**Severity**: **High** — would silently corrupt every macro regime label after a retrain.

### L-07 — Kalman covariance updates use the simple form (not Joseph form)

**File**: `kalman_filter.cpp:127`, `dynamic_factor_model.cpp:287, 354, 397`

```cpp
P = (I - K * H) * P;            // simple form
```

Joseph form preserves symmetry and PD even under floating-point roundoff:
```cpp
P = (I - K*H) * P * (I - K*H).transpose() + K * R * K.transpose();
```

BSTS Kalman runs the simple form on every macro series at every timestep — many updates, high blast radius. Long-running re-fits or live updates can produce non-PD covariance silently, breaking subsequent Cholesky decompositions.

**Fix**: Use Joseph form everywhere `(I - K*H) * P` appears.
**Severity**: High — numerical drift, can silently produce non-PD covariance.

### L-08 — DFM update() and filter() have inconsistent state semantics

**File**: `src/statistics/state_estimation/dynamic_factor_model.cpp:316-358, 239-308`

`update()` continues from `x_filt_` set in `fit()` (line 178). But `filter()` does **not** update `x_filt_` — it uses local variables (line 264).

So `fit() → filter(test_data) → update(new_obs)` resumes from end-of-training, **silently skipping the test window** without warning.

**Fix**: Either make `filter()` update `x_filt_`, or document explicitly that `update()` always resumes from the fit endpoint, and consider renaming `filter()` to `filter_fresh()`.
**Severity**: Medium — silent state corruption in mixed-mode usage.

### L-09 — GARCH/EGARCH update() doesn't demean new returns

**File**: `src/statistics/volatility/garch.cpp:268-274`, `egarch.cpp:254-262`

`fit()` computes `mean_return` and stores demeaned `residuals_` (line 39-41). But `update(new_return)` uses `new_return` directly with no demean step.

If user passes raw daily returns to `update()` that have non-zero mean (any trending market), the variance dynamics drift from training.

**Fix**: Either store `mean_return` as a member and apply in `update()`, or document the API contract clearly that `update()` expects already-demeaned returns.
**Severity**: Medium — silent regime drift in live runs.

### L-10 — GMM normalized entropy NaN when K=1

**File**: `src/statistics/clustering/gmm.cpp:177`

```cpp
best.entropy(i) = h / std::log((double)K);
```

If `K == 1`, `log(1) = 0`, division by zero → NaN. Edge case but trivially callable from a config file.

**Fix**: `if (K > 1) entropy(i) = h / log(K); else entropy(i) = 0;`
**Severity**: Low.

### L-11 — GMM reproducibility breaks under data refresh

**File**: `src/statistics/clustering/gmm.cpp:144`

`fit(X, K, seed)` is deterministic for fixed `(X, K, seed)`. But `kmeans_plus_plus` consumes a variable amount of randomness per restart depending on `n` (because `discrete_distribution` rejection-samples). If `X` changes between runs (e.g., one extra row of data appears), the restart-to-restart rng state diverges and outputs change discontinuously.

**Fix**: Re-seed the rng deterministically per restart: `rng.seed(seed + restart * 1009);`
**Severity**: Low — reproducibility under data refresh, matters for backtest determinism.

### L-12 — KalmanFilter condition-number check is O(n³) per update

**File**: `src/statistics/state_estimation/kalman_filter.cpp:104-110`

```cpp
Eigen::JacobiSVD<Eigen::MatrixXd> svd(S);
double cond = svd.singularValues()(0) / svd.singularValues()(svd.singularValues().size() - 1);
```

JacobiSVD is O(n³). BSTS calls Kalman per macro series per timestep — many invocations. The LLT factorization on the next line already gives diagonal min/max for free.

**Fix**: Replace SVD with `llt_S.matrixL().diagonal()` min/max ratio (after squaring).
**Severity**: Medium (performance, not correctness).

### L-13 — MS-DFM regime sort doesn't permute regime_labels

**File**: `src/statistics/state_estimation/ms_dfm.cpp:299-352`

`order_regimes_by_volatility()` permutes `A_`, `Q_`, `P_`, `pi0_`, `filtered_probs`, `smoothed_probs`, `decoded_regimes`, **but NOT `out.regime_labels`**.

If user provides `config_.regime_labels = ["custom_a", "custom_b", "custom_c"]` intending to map each label to a specific A_j init, the sort by Q-trace silently reorders the underlying regimes while labels stay in original order.

**Fix**: Either also permute `out.regime_labels` (if user labels are meant to follow init order), or document explicitly that regime labels apply to vol-sorted order regardless of config.
**Severity**: Medium — label/data drift.

---

## 2. Borderline (investigate before classifying)

| ID | File:Line | Issue | Note |
|----|-----------|-------|------|
| L-14 | `dynamic_factor_model.cpp:380` | Diffuse prior `P = 10 * I` | Fine for standardized data; matters only if `standardise_data = false` |
| L-15 | `ms_dfm.cpp:124` | Per-regime Q init scaling `0.5 + j` | Drives sort outcome; could be different but tuning |
| L-16 | `garch.cpp:113-116` | Silent override if user provides `α + β >= 1.0` | Defensive coding vs. silent override choice |
| L-17 | `egarch.cpp:139-140` | nlopt bounds for γ are `[-1, +1]` | γ is theoretically unbounded; restrictive but practical |
| L-18 | `kalman_filter.cpp:40` | Initial `P = I` with no setter for initial P | Constructor API design; may matter for non-standardized state-space |

---

## 3. Documented Divergence (do NOT actively fix)

These are spec-vs-code differences that look like fixes but are empirical/intentional calibration:

| ID | Item |
|----|------|
| L-D-1 | DFM ridge `1e-6` (`dynamic_factor_model.cpp:114, 116`), R-diag floor `1e-4` (line 144) |
| L-D-2 | MS-DFM ridges `1e-6` (`ms_dfm.cpp:107, 115, 126, 253, 265`) |
| L-D-3 | GMM cov regularization `1e-4` (`gmm.cpp:133`) |
| L-D-4 | MarkovSwitching diagonal init 0.95 (line 59) — strong persistence prior |
| L-D-5 | HMM default uniform transition (line 198) — config flag `init_random` controls |
| L-D-6 | GARCH grid search bounds (line 120-121) — empirical tuning |

---

## 4. Recommended Sequencing (relative to existing backlog)

**Before** any of the existing P0 macro fixes (M-01..M-04):

1. **L-06** — Lock DFM factor signs to economic anchors. Otherwise the hardcoded `growth_factor_sign = -1` becomes unsafe across re-fits, and any M-01..M-04 fix can be silently undone by a sign flip.

**During** P0 macro fixes (parallel with M-01..M-04):

2. **L-05** — Fix PCA NaN→0 bias. Macro panel has missing data routinely (release schedule misalignment).
3. **L-01** — Add zero-gamma guards to HMM M-step. Cheap fix, eliminates a NaN hazard.

**Before** any of the existing P0 market fixes (K-01..K-03):

4. **L-04** — Clean up MarkovSwitching xi formula. K-01 (σ floor) lands in the same EM; do them together to avoid touching the same code twice.

**During** market polish:

5. **L-02, L-03** — HMM cov ridge and LDLT log-det floor. Same EM-cleanup pass as L-04.
6. **L-07** — Joseph form across Kalman code paths. One sweeping change with high robustness payoff.
7. **L-09** — GARCH/EGARCH update() demean. Affects live deployment.

**Polish phase**:

8. **L-08, L-13** — API contracts (filter/update state continuity, regime label permutation).
9. **L-12** — Replace JacobiSVD condition check.
10. **L-10, L-11** — GMM edge cases.

---

## 5. What this means for the existing backlog

The pipeline-level backlog (`REGIME_PIPELINE_ANALYSIS.md`) operates one level too high to catch these. Several pipeline-level fixes interact with library-level issues:

- **M-01/M-02** (cross-state std lock-in) is silently undone by **L-06** (PCA sign flip) on retrain
- **K-01** (σ floor in MarketMSAR) lives in the same M-step as **L-04** (xi formula)
- **M-04** (DFM 2D Gaussian) sits on top of **L-05** (PCA NaN→0 bias) and **L-07** (Kalman simple form)
- **K-09** (smoothing init contamination) is ultimately resolved by accumulator state — same family as **L-08** (filter/update state semantics)

**Recommendation**: Fold L-06, L-05, L-01, L-04 into Phase 1 of the existing sequencing. They are prerequisites, not optional polish.

---

---

# Round 2 — BSTS, data loaders, tests

**Audit date**: 2026-04-27 (same session, continuing from round 1)
**Scope**: `bsts_regime_detector.cpp`, `macro_data_loader.cpp`, `market_data_loader.cpp`, test files

## 7. Active Fixes (round 2)

### L-19 — BSTS `backward_fill()` is lookahead bias on training data

**File**: `src/statistics/state_estimation/bsts_regime_detector.cpp:113-122` (definition); called at lines **848, 850, 984** (used unconditionally in `fit()` and `fit_from_db()`); also called per-series at lines 243-247 in `fit_series` for non-ETF (macro) series.

```cpp
void BSTSRegimeDetector::backward_fill(Eigen::MatrixXd& X) {
    for (int j = 0; j < X.cols(); ++j) {
        double next = std::numeric_limits<double>::quiet_NaN();
        for (int i = X.rows()-1; i >= 0; --i) {
            if (std::isfinite(X(i,j))) next = X(i,j);
            else if (std::isfinite(next)) X(i,j) = next;   // ← FILLS PAST WITH FUTURE
        }
    }
}
```

For any NaN cell at row `t`, this fills with the next non-NaN value at row `t' > t`. The fitted BSTS state-space model and downstream regime classifier therefore train on a panel where past observations have been retro-corrected with future information.

**Why this matters**: The model trained this way performs differently in production (where backward fill is not possible — there is no future). Reported in-sample regime accuracy is biased upward. The most relevant impact is on the **R0-R3 cluster labels** (the same labels that M-06 already flags as misassigned) — they are trained on contaminated features.

**Fix**: Drop `backward_fill()` from the training pipeline entirely. For NaN gaps in macro series, rely on `forward_fill()` only (which is the legitimate "carry-forward" practice — yesterday's known value as today's best estimate). If a series has NaN at the start of the panel before its first release, drop those leading rows or use a nan-aware downstream model.

**Severity**: **High** — fundamental statistical correctness.

### L-20 — BSTS `gaussian_smooth()` is centered, applied to regime features by default

**File**: `src/statistics/state_estimation/bsts_regime_detector.cpp:280-298` (definition); called at lines **408-413** in `build_feature_matrix()` for `growth_score`, `inflation_score`, `growth_inflation_quad`, `yield_curve`, `financial_stress`, `labor_slack`, `ros`, `growth_v`, `inflation_v`, `gi_quad`.

```cpp
for (int k = -radius; k <= radius; ++k)
    acc += kernel[k+radius] * x(std::clamp(i+k, 0, n-1));   // uses i+k for k > 0
```

The kernel is symmetric around index `i`, meaning the smoothed feature at time `t` includes values from `t+1, t+2, ..., t+radius`. Default `radius = 4`, `sigma = 2.0` (header line 34-35) — every regime feature uses **4 future days** by default.

Affected:
- The Quadrant model's `growth_score` and `inflation_score` — feed into `map_quadrant()` directly
- `growth_inflation_quad` — used as the `gi_quad` axis in BSTS R0-R3 scoring (lines 364-365 of analysis doc Appendix 6.6)
- `ros` — Risk-On Score in the same R0-R3 scoring
- The Block 3 regime polarity composites that go into PCA/GMM clustering

**Effect on backlog**: This is upstream of M-03 (Quadrant cap-util scaling), M-06 (R0-R3 greedy assignment), and the BSTS cluster posteriors. Fixing M-03 with smoothed-with-future inputs gives a different `growth_score` than the "real-time" version.

**Fix**: Replace centered Gaussian with **causal** EWMA or trailing kernel:
```cpp
// causal: kernel only over [-2*radius, 0]
for (int k = -2*radius; k <= 0; ++k)
    acc += kernel[k + 2*radius] * x(std::clamp(i+k, 0, n-1));
```
Or expose a `causal_smoothing` flag and default it to `true`.

**Severity**: **High** — silent lookahead in regime decision features.

### L-21 — BSTS PCA has same arbitrary sign convention as DFM (L-06 sibling)

**File**: `src/statistics/state_estimation/bsts_regime_detector.cpp:434-468`

`SelfAdjointEigenSolver` produces eigenvectors with arbitrary sign. Re-fits flip signs.

Less severe than L-06 because:
- BSTS doesn't assign labels to PCA components directly
- Cluster centers move with the sign flip but distances/responsibilities are sign-invariant
- The R0-R3 scoring uses macro composites (gs, is, gi, fs, ros), not PCA components

But the PCA-transformed matrix is what feeds GMM clustering, so cluster identities and the M-06-flagged label-permutation behavior depend on PCA convention.

**Fix**: Apply a deterministic sign convention — e.g., flip eigenvector if its largest-magnitude entry is negative.

**Severity**: Medium — affects reproducibility, not real-time correctness.

### L-22 — `gmtime` is not thread-safe (data race in macro loader)

**File**: `src/statistics/state_estimation/macro_data_loader.cpp:161`

```cpp
std::tm* tm = std::gmtime(&epoch_seconds);   // ← returns pointer to STATIC buffer
char buf[11];
std::strftime(buf, sizeof(buf), "%Y-%m-%d", tm);
```

`std::gmtime` returns a pointer to a static internal buffer — if two threads call it concurrently, dates corrupt. macOS/Linux: use `gmtime_r(&t, &tm_local)`. Windows: use `gmtime_s`. The companion `market_data_loader.cpp:118` already uses `localtime_r`/`localtime_s` with `#ifdef _WIN32` — apply the same pattern here.

**Severity**: Medium — manifests only under concurrent loader use, but a real race condition.

### L-23 — `std::stod` silently truncates on non-numeric suffixes

**File**: `src/statistics/state_estimation/macro_data_loader.cpp:213`

```cpp
panel.data(row_idx, c) = std::stod(val);
```

`stod` returns the leading numeric prefix and ignores the rest. So `"5.2%"` → `5.2`, `"3.4 (estimate)"` → `3.4`, `"NaN-but-not-really"` → throws (caught at line 214). **Silent partial conversion** when a DB column has units mixed in.

**Fix**: Validate that the entire string was consumed:
```cpp
size_t consumed = 0;
double parsed = std::stod(val, &consumed);
if (consumed == val.size()) {
    panel.data(row_idx, c) = parsed;
} else {
    WARN("[MacroDataLoader] partial parse of '" << val << "' in column " << panel.column_names[c]);
}
```

**Severity**: Medium — depends on whether your DB ever stores mixed-format strings. Cheap defensive fix.

### L-24 — Macro `load_single()` returns mostly-NaN rows on non-release days, breaking DFM `update()`

**File**: `src/statistics/state_estimation/macro_data_loader.cpp:288-311` (load_single); `src/statistics/state_estimation/ms_dfm.cpp:391-396` (DFM update strict NaN rejection)

`load_single(date)` queries the loader with `start = end = date` — gets one day. Forward-fill at lines 257-268 only fills WITHIN that single-day result, so any series not released on that exact date stays NaN.

But `MS-DFM::update()` requires all-finite input (`if (!std::isfinite(...)) return error`). And `DynamicFactorModel::update()` (line 336) silently skips NaN observations in the Kalman update — but the outer caller might still treat that as a bug.

In live operation, **most days are non-release days for most macro series**. So `load_single()` returns mostly-NaN rows almost every day, and the live update path likely fails to update DFM/MS-DFM correctly.

**Fix**: `load_single()` should query a window backward (e.g., 90 days) and forward-fill to the requested date, then return only the last row. This preserves the legitimate carry-forward practice for live use.

**Severity**: **High for live deployment**, low for offline analysis.

### L-25 — Market loader date parsing uses local timezone (cross-platform reproducibility)

**File**: `src/statistics/state_estimation/market_data_loader.cpp:51-58`

```cpp
auto parse_iso_date = [](const std::string& s) -> Timestamp {
    std::tm tm = {};
    std::istringstream ss(s);
    ss >> std::get_time(&tm, "%Y-%m-%d");
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    std::time_t t = std::mktime(&tm);   // ← LOCAL TIMEZONE
    return std::chrono::system_clock::from_time_t(t);
};
```

`std::mktime` interprets `tm` as local time. So `"2026-04-27"` parses to a different absolute Timestamp on a server in UTC vs. EST. Bars are then sorted by `bar.timestamp` and reformatted via `localtime_r` (line 118) — also local-time. Near midnight, this can cause **off-by-one date assignments** depending on where the loader runs.

**Fix**: Use `timegm()` (POSIX) or platform-equivalent for UTC interpretation:
```cpp
std::time_t t = timegm(&tm);   // POSIX; not in C++ standard but on macOS/Linux
```
Or convert explicitly through `std::chrono` UTC primitives.

**Severity**: Medium — manifests only on cross-machine deployments or DST boundaries.

### L-26 — Market loader emits 0.0 for missing/non-positive prices (silent zeros in returns)

**File**: `src/statistics/state_estimation/market_data_loader.cpp:33` and `:192, 209`

```cpp
// compute_log_returns
if (p_prev > 0.0 && p_curr > 0.0) {
    returns.push_back(std::log(p_curr / p_prev));
} else {
    returns.push_back(0.0);  // ← silent zero return
}

// align_panels initializes composite_returns to zeros (line 192)
// and the loop at 200-213 only writes when alignment succeeds — gaps stay 0.0
```

If a bar has missing/zero/negative price (data error), the loader emits **a zero return** instead of NaN or error. Downstream HMM/MSAR EM treat this as a real observation: zero-return → fits as TREND_LOWVOL. Effect: **silently bias toward low-vol regime** during data outages.

**Fix**: Emit NaN (use `std::nan("")` or `std::numeric_limits<double>::quiet_NaN()`), and have downstream filters explicitly skip NaN observations. The HMM `forward_backward` already log-clamps but doesn't natively skip — needs per-call NaN check.

**Severity**: High during data outages; low under normal operation.

### L-27 — Tests don't exercise lookahead, NaN, σ-collapse, or stress regimes

**Files**: `tests/statistics/test_macro_regime_pipeline.cpp`, `tests/statistics/test_market_regime_pipeline.cpp`

The existing tests verify:
- Configuration serialization round-trip
- Train/update API contract (rejects too-few-timesteps, rejects update-before-train)
- `is_ok()` after train, `probs_sum_to_one`
- DFM Gaussian dominant pick on synthetic expansion data
- Quadrant goldilocks mapping > 0.5
- BSTS uniform-when-disabled vs always-contribute behavior

What's NOT tested:
- That `backward_fill` and `gaussian_smooth` produce same training-time and inference-time features (would catch L-19, L-20)
- That HMM/MSAR don't degenerate to σ < 1% (would catch K-01)
- That MS-DFM doesn't lock 90% on a single regime when input is balanced (would catch M-01)
- That STRESS regimes get non-trivial probability under stress synthetic data (would catch K-04)
- NaN propagation paths (would catch L-01, L-03, L-26)
- Sign-flip stability across re-fits (would catch L-06, L-21)
- `load_single()` on non-release dates (would catch L-24)

This means the test suite passes happily while every active backlog item could be present.

**Fix**: Add a `test_regime_correctness.cpp` with adversarial cases — synthetic stress periods, NaN injections, repeated re-fits with sign-flip checks, lookahead detection (compare batch-trained features against walk-forward features at each timestep).

**Severity**: Medium — tests don't *cause* bugs, but they aren't catching the ones in the backlog. Worth investing once you've fixed the backlog items, so regressions are caught.

---

## 8. Updated sequencing (Phase 0 — prerequisites before any backlog item)

Insert **before** any of the existing M-/K- macro/market fixes:

| Order | ID | Why first |
|------:|----|-----------|
| 1 | **L-19** | Drop `backward_fill()` from BSTS training. Otherwise every M-/K- macro fix is verified against contaminated features. |
| 2 | **L-20** | Make `gaussian_smooth` causal (or default to disabled). Otherwise M-03 (Quadrant `growth_score` rescaling) is computed on lookahead inputs and won't reflect real-time behavior. |
| 3 | **L-06** | Lock DFM factor signs to economic anchors. Otherwise M-01..M-04 fixes can be silently undone by a sign flip on retrain. |
| 4 | **L-05** | Fix PCA NaN→0 bias. Affects DFM Lambda init when macro panel has missing data (routine condition). |
| 5 | **L-26, L-23** | Emit NaN, not 0, on bad data. Otherwise stress detection is silently muted during outages. |

Then proceed with existing **Phase 1 — Macro unlocks** (M-04, M-01, M-02, M-03), but now with confidence the substrate is correct.

---

## 9. Summary of L-# items added by round 2

| ID | Location | Severity |
|----|----------|---------:|
| L-19 | `bsts_regime_detector.cpp:848,850,984` | **High** — lookahead in training |
| L-20 | `bsts_regime_detector.cpp:408-413` | **High** — lookahead in features at runtime |
| L-21 | `bsts_regime_detector.cpp:434-468` | Medium — PCA sign reproducibility |
| L-22 | `macro_data_loader.cpp:161` | Medium — gmtime thread-unsafe |
| L-23 | `macro_data_loader.cpp:213` | Medium — silent partial parse |
| L-24 | `macro_data_loader.cpp:288-311` | **High for live** — load_single returns NaN-heavy rows |
| L-25 | `market_data_loader.cpp:51-58` | Medium — local-tz date parse |
| L-26 | `market_data_loader.cpp:33,192,209` | High in outage — silent 0.0 returns |
| L-27 | test suite | Medium — coverage gap, doesn't catch backlog items |

**Round 1 + 2 total: 27 active findings + 5 borderline + 6 documented divergences.**

---

---

# Round 3 — Pipeline impl + runners

**Audit date**: 2026-04-27 (same session, continuing from round 2)
**Scope**: `macro_regime_pipeline.cpp` (1052 lines), `market_regime_pipeline.cpp` (801 lines), `apps/macro/market_regime_pipeline_runner.cpp` (595 lines), `apps/macro/macro_regime_pipeline_runner.cpp` (520 lines)

> Note: round 3 items live in the **pipeline layer** (orchestration), not the **library layer** (numerical libs). Keeping the L-# prefix for continuity. Some are operational/integration bugs rather than math bugs — flagged accordingly.

## 10. Active Fixes (round 3)

### L-28 — Division by zero if `min_dwell_bars = 0`

**File**: `src/statistics/state_estimation/macro_regime_pipeline.cpp:858-859`

```cpp
if (dwell_counter_ < config_.min_dwell_bars) {
    double decay = 1.0 - static_cast<double>(dwell_counter_) / config_.min_dwell_bars;  // ÷0 if = 0
```

Config exposes `min_dwell_bars` via JSON (line 67). User sets to 0 to disable dwell → divide-by-zero, `decay = nan`, `bonus = nan`, `p_smooth(current_idx) += nan` → entire smoothed vector becomes NaN, hysteresis returns garbage.

**Fix**: Guard the whole block with `if (config_.min_dwell_bars > 0 && dwell_counter_ < config_.min_dwell_bars)`.
**Severity**: Medium — only triggers under specific config but silently corrupts everything.

### L-29 — Race condition between `trained_` check and mutex acquisition

**Files**:
- `macro_regime_pipeline.cpp:924-929` — `if (!trained_) return error; ... lock_guard lock(mutex_);`
- `market_regime_pipeline.cpp:672-678` — same pattern

```cpp
if (!sleeve_states_[s].trained) {
    return make_error(...);
}
std::lock_guard<std::mutex> lock(mutex_);  // ← lock acquired AFTER unsafe read
```

If thread A is in `train()` (line 159 macro / 611 market — both lock the mutex but `trained_` is set inside the locked region), and thread B calls `update()`, thread B reads `trained_` without the lock — could observe partial state, or read stale `false` and return error during legitimate concurrent training.

**Fix**: Acquire the lock first, then check `trained_`. Atomic on a primitive bool can also work for the read-side, but pairing with the existing lock is cleaner.

**Severity**: Medium — only manifests under concurrent train+update, but real data race.

### L-30 — `last_belief_` not reset on retraining (stale state across retrains)

**Files**:
- `macro_regime_pipeline.cpp:217-222` — `train()` resets `prev_smoothed_`, `current_regime_`, `dwell_counter_`, `update_count_`, but **not** `last_belief_`
- `market_regime_pipeline.cpp:650-652` — `train()` resets `prev_smoothed`, `trained`, but **not** `last_belief`

Effect: After a retrain, the very first `update()` call enters the all-models-failed fallback path:
```cpp
fallback.most_likely = sleeve_states_[s].last_belief.most_likely;          // STALE
fallback.regime_age_bars = sleeve_states_[s].last_belief.regime_age_bars + 1; // STALE
```
The stale `most_likely` from the previous training run leaks into the new run's fallback. Worse, `regime_age_bars` keeps incrementing across retrains as if no transition occurred.

The non-failure path (line 1026-1030 macro, 777-780 market) compares `dominant == last_belief.most_likely` to compute `regime_age_bars` — initial post-retrain ages are also wrong because of stale `last_belief.most_likely`.

**Fix**: Reset `last_belief_ = MacroBelief{};` in macro `train()`, and `sleeve_states_[s].last_belief = MarketBelief{};` in market `train()`.

**Severity**: Medium — silent state leakage across training cycles.

### L-31 — MacroBelief overlay fields never populated

**File**: `src/statistics/state_estimation/macro_regime_pipeline.cpp:1015-1030`

The MacroBelief struct (per `regime_aware_portfolio_engine.md` spec) has overlay fields:
- `policy_restrictive`
- `credit_tightening`
- `inflation_sticky`
- `structural_break_risk`

The pipeline only sets `structural_break_risk` (line 1023). The other three are never assigned, so they retain their default-initialized values (`false` for bool). Downstream consumers reading these fields silently get false negatives — they can never trigger logic gated on these overlays.

**Fix**: Either (a) add detector logic in `update()` for the three missing overlays, or (b) explicitly remove the fields from the struct if they're not yet implemented (so consumers can't accidentally rely on them).

**Severity**: Medium — spec-vs-code gap with downstream impact, NOT empirical tuning (your rule applies — these fields exist in the spec and are silently always false).

### L-32 — Quadrant z-score stats baked at training; no live recalibration

**File**: `src/statistics/state_estimation/macro_regime_pipeline.cpp:196-203` (training); `:635-636` (runtime use)

```cpp
// At train():
growth_median_ = percentile(gv, 0.50);
growth_std_ = std::sqrt(g_var / gv.size() + kEps);
// At update() via map_quadrant():
double g_norm = (growth_score - growth_median_) / growth_std_;
```

The Quadrant model normalizes `growth_score` and `inflation_score` using stats fixed at training time. As live data drifts (growth regime shifts, inflation persistence), `g_norm` becomes mis-scaled. The Quadrant probabilities then misrepresent the regime over time.

This is **not in M-/K-** — the existing M-03 fixes the *composition* of growth_score (cap-util scaling), but the *normalization* drift across training boundaries is a separate issue.

**Fix**: Either expose `recalibrate_quadrant_stats()` API for periodic refresh, or use a rolling-window z-score in `map_quadrant` directly. Both have tradeoffs — periodic refresh is simpler, rolling is more responsive.

**Severity**: Medium — silent drift over months/quarters of live operation.

### L-33 — Market pipeline lacks warmup analog (smoothing init contamination)

**File**: `src/statistics/state_estimation/market_regime_pipeline.cpp:557-575` (smooth function)

The macro pipeline has a warmup step (`macro_regime_pipeline.cpp:801-805`) where `lambda = 1.0` for the first 10 updates, bypassing the uniform `prev_smoothed_` init. The market pipeline has **no analog** — `prev_smoothed` starts at `uniform([1/5, 1/5, 1/5, 1/5, 1/5])` and the EWMA smooths from uniform on bar 1.

The existing **K-09** captures this symptom for the runner's 5-bar update loop, but the structural gap is in the pipeline itself: even a long update loop never escapes the uniform init quickly because λ=0.30 has half-life of ~2 bars, so contamination persists.

**Fix**: Mirror the macro warmup logic in market — use `lambda = 1.0` until `update_count >= warmup_steps` (suggest 10), then switch to `config_.lambda`. Per-sleeve update_count needed.

**Severity**: Medium — structural fix that subsumes K-09.

### L-34 — Silent zero fill when GARCH vol series shorter than returns

**File**: `src/statistics/state_estimation/market_regime_pipeline.cpp:642-644`

```cpp
Eigen::MatrixXd garch_feat = Eigen::MatrixXd::Zero(T, 4);
for (int t = 0; t < T; ++t)
    garch_feat(t, 1) = (t < (int)garch_vol_series.size()) ? garch_vol_series[t] : 0.0;
```

If GARCH was fit on shorter data than the regime training window (e.g., GARCH skipped initial NaN bars), the tail of `garch_feat` is **silently zero-filled**. Zero vol → vol_percentile drops, GARCH mapping leans toward TREND_LOWVOL. Silent regime bias.

**Fix**: Either error if `garch_vol_series.size() != T` (strict contract), or warn and use the last valid vol value (carry-forward). The current silent zero-fill is the worst option.

**Severity**: Medium — silent regime bias under data-length mismatch.

### L-35 — Cross-asset correlation silently zero-filled when sleeve has < 2 symbols

**File**: `apps/macro/market_regime_pipeline_runner.cpp:570-576`

```cpp
if (sd.composite_returns.rows() > 0 && sd.composite_returns.cols() >= 2) {
    sleeve_data[i].corr_spike = compute_corr_spike(sd.composite_returns);
} else {
    sleeve_data[i].corr_spike.assign(p.returns.size(), 0.0);  // ← silent zero
}
```

If a sleeve only has one usable symbol (e.g., rates with only ZN data, ZF disabled), `corr_spike` becomes a zero vector. GMM feature column 4 (`corr_spike`) is then identically zero across that sleeve — the GMM clustering operates in 4D effectively, and the "correlation_stress" axis is dead.

This is related to **K-08** (within-sleeve correlation is useless for equities), but K-08 addresses the *case where ≥2 symbols exist*. L-35 is the case where the sleeve degenerates to 1 usable symbol — silently masked.

**Fix**: Either skip the GMM feature dimension (drop col 4) when corr_spike isn't computable, or emit NaN and have GMM skip that dim. Don't fill with zeros.

**Severity**: Medium — silent feature degeneracy in single-symbol sleeves.

---

## 11. Verified clean (round 3)

Things I checked that look correct:

- **Macro DFM Gaussian fit** (`macro_regime_pipeline.cpp:280-329`): proper sample-count guard (`< 5 → fallback`), per-regime mean/cov computed from valid indices only.
- **Macro `aggregate()` and failure-degradation logic** (line 940-998): `is_valid()` check + weight redistribution work as designed; uniform fallback when all models fail.
- **Macro `apply_hysteresis()`** (line 843-897): the relative-lead escape hatch (`lead > 0.05`) prevents stale-regime lock-in; this is a documented divergence from spec, not a bug.
- **Market `compute_stability()`** (line 593-605): correctly normalized entropy [0, 1]; guards against `H_max < kEps` for K=1 edge case.
- **Market provenance** (`market_regime_pipeline.cpp:768-774`): per-model contributions are correctly captured for all 4 models.
- **Both pipelines' `update()`** correctly check `trained_` (modulo L-29 race) and fall back gracefully when models fail.

## 12. Cross-round summary (all 3 rounds)

| Round | New active findings | Cumulative active |
|------:|---------------------:|-----------------:|
| 1 — Numerical libs (DFM, MS-DFM, HMM, MS, GARCH, EGARCH, Kalman, GMM) | 13 | 13 |
| 2 — BSTS state-space, data loaders, tests | 9 | 22 |
| 3 — Pipeline orchestration + runners | 8 | 30 |

Plus 5 borderline + 6 documented divergences across all 3 rounds.

Combined with the existing pipeline-level backlog (M-01..M-13 + K-01..K-17, after applying your spec-vs-tuning rule = ~17 active), **total backlog ~47 items**.

## 13. Phase-0 prerequisites — final list

These should land before any P0 fix from `REGIME_PIPELINE_ANALYSIS.md`:

| Order | ID | What |
|------:|----|------|
| 1 | **L-19** | Drop `backward_fill()` from BSTS training |
| 2 | **L-20** | Make `gaussian_smooth()` causal (or default `radius=0`) |
| 3 | **L-06** | Lock DFM factor signs to economic anchors |
| 4 | **L-05** | Fix PCA NaN→0 bias (pairwise complete-case covariance) |
| 5 | **L-26, L-23, L-34, L-35** | Replace silent zero/partial-parse defaults with NaN/error |
| 6 | **L-30, L-31** | Reset `last_belief_` on retrain; populate or remove unused MacroBelief fields |

Then Phase 1 from the existing analysis doc (M-04, M-01, M-02, M-03) — now operating on a substrate that doesn't lookahead, doesn't sign-flip, doesn't silently zero-fill.

---

*End of L-# round 3. The audit is now substantively complete. Remaining work: a deeper read of the underlying `markov_switching` and `egarch` test paths if needed, and a review of `regime_aware_portfolio_engine.md` to verify which MacroBelief overlay fields are actually consumed downstream (informs whether L-31 fix is "implement detectors" or "remove dead fields").*
