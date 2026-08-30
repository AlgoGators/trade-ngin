# Regime Detection — M/K/L Status Roll-up

One row per item across all three audit ledgers: **M-01..M-13** and **K-01..K-17**
(defined in `history/REGIME_PIPELINE_ANALYSIS.md`) and **L-01..L-35** (defined in
`history/REGIME_PIPELINE_LIBRARY_AUDIT.md`). Classifications follow
`history/REGIME_PIPELINE_FIX_PLAN.md` §1 (its demotions override ANALYSIS where they
disagree). Closing commits reconstructed from this branch's history; where a commit
message does not name the item individually, the fix is verified in code and cited by
file:line ("code-verified").

Status legend: **FIXED** · **FIXED-VIA** (second-order, resolved by the named root fix) ·
**DEMOTED** (NOT-A-BUG per FIX_PLAN) · **TUNING-NOTE** (never active; recorded design
choice) · **DEFERRED-CONFIG-GATED** (fix exists behind a flag; default flip pending —
in-lane) · **OPEN-BUG** (in current lane scope) · **OPEN-MEASURE** (measure first, change
only if warranted — in-lane) · **OPEN-ENHANCEMENT** (roadmap, see ENHANCEMENTS.md).

## Macro items (M)

| ID | Description | Status | Closing commit / evidence | Cross-ref |
|----|-------------|--------|---------------------------|-----------|
| M-01 | MS-DFM EXP_DIS lock-in via cross-state standardization | FIXED | `6f9ffc1` (drop cross-state /std, keep de-mean) | |
| M-02 | BSTS 0% EXP_DIS (same mechanism, shared targets) | FIXED-VIA M-01 | `6f9ffc1`; FIX_PLAN §1.2 | |
| M-03 | Quadrant growth_score cap-util-dominated | FIXED | `6f9ffc1` (z-score each component) | |
| M-04 | DFM factor 0 non-discriminative in Gaussian | FIXED | `6f9ffc1` (2-D Gaussians, factor 0 dropped) | |
| M-05 | Quadrant inflation_score uses levels not changes | FIXED | `6f9ffc1` (CPI/PCE slope) | |
| M-06 | BSTS R0-R3 greedy anti-matching labels | FIXED | `95e4944` (reject ≤0 scores → Unclassified; FIX_PLAN §1.3 decision) | |
| M-07 | MS-DFM fingerprints hard-argmax vs runtime soft probs | FIXED | `95e4944` (soft-prob-weighted) | |
| M-08 | DFM "RECESSION" label for services-blind industrial slowdown | FIXED (interim relabel) | `2157c96` (RECESSION_* → INDUSTRIAL_WEAKNESS_*); full fix = services proxies | Gap 9 (in-lane finish), Enh 6 |
| M-09 | Confidence metric only top-minus-second | FIXED | `0414833` (entropy_concentration + top_prob fields) | |
| M-10 | enter_riskoff only 2pp above enter_defensive | OPEN-MEASURE | FIX_PLAN: "measure first, do not change blindly" — measurement never run; in-lane phase 2 | |
| M-11 | DFM cov floor 0.5·I sensitivity unexplored | OPEN-ENHANCEMENT | calibration sweep (A/B 0.2–0.7) | |
| M-12 | Dwell penalty 0.3 may mask legitimate transitions | OPEN-ENHANCEMENT | calibration sweep (0.15 candidate) | |
| M-13 | Cold-start warmup for live deployment | OPEN-ENHANCEMENT | ties to live-deploy work (K-17 shipped the state API) | |

## Market items (K)

| ID | Description | Status | Closing commit / evidence | Cross-ref |
|----|-------------|--------|---------------------------|-----------|
| K-01 | HMM/MSAR σ collapse (relative floor needed) | FIXED | `b66e279` (relative σ floor in markov_switching) | |
| K-02 | Market training data only 2024 (T=311) | FIXED | code-verified: runner default `2020-01-01` (`market_regime_pipeline_runner.cpp:617`); not individually named in a commit message | |
| K-03 | GMM col 3 mislabeled vol_shock | FIXED | `b66e279` (volume_ratio rename + retuned targets) | |
| K-04 | HMM trend = signed drift misclassifies downtrends | FIXED | `95e4944` (\|μ\|) + `138d0d6` (v2: 4-D fingerprint) | |
| K-05 | STRESS_PRICE vs STRESS_LIQUIDITY near-collinear | FIXED | `138d0d6` (retuned geometry; K-05+ = current baseline) | |
| K-06 | MSAR transition-matrix inconsistency | FIXED | `95e4944` (recompute from smoothed posteriors) | |
| K-07 | liquidity_proxy defaults 1.0, mutes STRESS_LIQUIDITY | FIXED | `9daf485` (NaN-gated liquidity adj; L-26 family per FIX_PLAN §1.2) | |
| K-08 | Correlation spike within-sleeve (useless) | FIXED | `d477bb2` (pooled cross-sleeve) | |
| K-09 | Smoothing init contamination (5-bar loop) | FIXED-VIA L-33 | `b66e279` (warmup counter subsumes; FIX_PLAN §1.2) | |
| K-10 | Target fingerprints identical across sleeves | OPEN-BUG (in-lane phase 2) | only P1 item never implemented | Gap 8 + Gap 10, Enh 7 |
| K-11 | Stability metric computed, never printed | FIXED | `0414833` | |
| K-12 | EGARCH γ significance threshold arbitrary | DEFERRED-CONFIG-GATED | dropped in `0414833` (baseline-shifting); in-lane phase 2: flip ONLY if A/B shows no degradation | Enh (config-gated) |
| K-13 | GARCH adjustments additive not log-odds | DEMOTED | FIX_PLAN §1.3: leave additive | |
| K-14 | O(T³) drawdown_speed | FIXED | `0414833` (incremental, bit-identical) | |
| K-15 | Printf bugs: rates MSAR σ missing; FX MSAR label typo | **OPEN-BUG (in-lane phase 2)** | listed Phase 4, absent from the phase-4 commit | |
| K-16 | corr_spike z-window vs short T | FIXED | `0414833` (window adapts) | |
| K-17 | No persistent state for live deployment | FIXED | `0414833` (checkpoint/restore API) | M-13 remains for warmup policy |

## Library items (L)

| ID | Description | Status | Closing commit / evidence | Cross-ref |
|----|-------------|--------|---------------------------|-----------|
| L-01 | HMM M-step zero-gamma guard | FIXED | `b66e279` | |
| L-02 | HMM cov ridge absolute not relative | FIXED-VIA K-01 shape | `b66e279` (relative cov ridge in hmm; FIX_PLAN §1.2) | |
| L-03 | LDLT log-det −inf fallback | FIXED | `b66e279` (floor) | |
| L-04 | MarkovSwitching xi redundant emit_j | DEMOTED | FIX_PLAN §1.1: cancels exactly, math correct | |
| L-05 | DFM PCA cov biased by NaN→0 fill | FIXED | `0ac89c7` (pairwise PCA cov) | |
| L-06 | DFM PCA arbitrary signs / hardcoded flips | FIXED | `0ac89c7` (lock pca signs) | |
| L-07 | Kalman simple-form covariance update | FIXED | `0414833` (Joseph form) | |
| L-08 | DFM update() vs filter() state semantics | FIXED | `0414833` (documented contract) | |
| L-09 | GARCH/EGARCH update() doesn't demean | FIXED | `95e4944` (consistent with fit()) | |
| L-10 | GMM entropy NaN when K=1 | FIXED | `0414833` (guard) | |
| L-11 | GMM reproducibility across restarts | DEFERRED-CONFIG-GATED | dropped in `0414833` (baseline-shifting); in-lane phase 2 default flip (seed per restart) | Enh (config-gated) |
| L-12 | O(n³) condition check per update | FIXED | `0414833` (LLT cond check) | |
| L-13 | MS-DFM sort doesn't permute regime_labels | FIXED | `0414833` | |
| L-14 | Diffuse prior P = 10·I | TUNING-NOTE | LIBRARY_AUDIT design-note table; never active | |
| L-15 | Per-regime Q init scaling 0.5+j | TUNING-NOTE | same | |
| L-16 | GARCH override when α+β ≥ 1 | DEMOTED | FIX_PLAN §1.1: correct defensive coding | |
| L-17 | EGARCH γ bounds [−1, 1] | DEMOTED | FIX_PLAN §1.1: calibration choice | |
| L-18 | Kalman initial P = I, no setter | DEMOTED | FIX_PLAN §1.1: standard convention | |
| L-19 | BSTS backward_fill lookahead | FIXED | `0ac89c7` | |
| L-20 | BSTS centered gaussian_smooth lookahead | FIXED | `0ac89c7` (causal) | |
| L-21 | BSTS PCA sign convention (L-06 sibling) | FIXED-VIA L-06 pattern | `0ac89c7` (bundled per FIX_PLAN §1.2) | |
| L-22 | gmtime not thread-safe | FIXED | `0ac89c7` (gmtime_r) | |
| L-23 | std::stod silent truncation | FIXED | `0ac89c7` NaN-semantics family; code-verified `macro_data_loader.cpp:219-223` (consumed-length check) | |
| L-24 | load_single mostly-NaN rows on non-release days | FIXED | `0ac89c7` (90-day fill) | |
| L-25 | Market loader local-TZ date parse | DEFERRED-CONFIG-GATED | dropped in `0414833` (baseline-shifting on non-UTC hosts); in-lane phase 2 default flip (timegm) | Enh (config-gated) |
| L-26 | Loader emits silent 0.0 for missing prices | FIXED | `0ac89c7` family; code-verified `market_data_loader.cpp:26-39` (NaN contract) | |
| L-27 | Tests don't exercise lookahead/NaN/σ-collapse/stress | **OPEN-BUG (in-lane phase 2)** | the Phase-5 adversarial suite was never written; loader layer least covered | |
| L-28 | Division by zero if min_dwell_bars = 0 | FIXED | code-verified `macro_regime_pipeline.cpp:908` (`> 0` guard + comment); not individually named in a commit message | |
| L-29 | trained_ check before mutex acquisition | FIXED | code-verified `macro_regime_pipeline.cpp:976-979` (lock-first with rationale comment) | |
| L-30 | last_belief_ not reset on retrain | FIXED | `0ac89c7` | |
| L-31 | MacroBelief overlay fields never populated | FIXED | `0ac89c7` (fields removed per FIX_PLAN §1.3 decision; re-add with real overlay detectors) | Enh 5 |
| L-32 | Quadrant z-stats baked at training | FIXED | `2157c96` (recalibrate_quadrant_stats API) | |
| L-33 | Market pipeline lacks warmup analog | FIXED | `b66e279` (warmup counter; subsumes K-09) | |
| L-34 | Silent zero fill when GARCH vol series short | FIXED | code-verified `market_regime_pipeline_runner.cpp:129-156` (causal init, full-length series, explosive fallback); not individually named | |
| L-35 | Cross-asset corr silently zero-filled (<2 symbols) | FIXED | `d477bb2` (pooled redesign) + `9daf485` (contract test) | |

## Also in current lane scope (no ledger ID)

- **Gap 7** (macro override of sleeve TREND_LOWVOL): implement BOTH cap and blend,
  pick empirically. **Gap 9** finish (label decision beyond M-08's interim rename).
- **`test_macro_data_loader.cpp` stub**: 0 tests today; gets real coverage with L-27's suite.
- **Fresh baseline regeneration** (one controlled migration after all baseline-shifting
  flips: L-11, L-25, K-12-if-clean, K-10/Gap-8 geometry).

## Reconciliation (mandatory check)

- **Source-ledger counts**: ANALYSIS defines exactly **13 M** (M-01..M-13) and **17 K**
  (K-01..K-17); LIBRARY_AUDIT defines exactly **35 L** (L-01..L-35). This table has
  **65 rows** — every ID present, none missing, none invented.
- **Classification totals** (must sum to 65): FIXED **44** (M×8: M-01, M-03..M-07,
  M-08-interim, M-09 · K×12: K-01..K-08, K-11, K-14, K-16, K-17 ·
  L×24: L-01, L-03, L-05, L-06, L-07, L-08, L-09, L-10, L-12, L-13, L-19, L-20, L-22,
  L-23, L-24, L-26, L-28, L-29, L-30, L-31, L-32, L-33, L-34, L-35) ·
  FIXED-VIA **4** (M-02, K-09, L-02, L-21) · DEMOTED **5** (L-04, L-16, L-17, L-18,
  K-13) · TUNING-NOTE **2** (L-14, L-15) · DEFERRED-CONFIG-GATED **3** (L-11, L-25,
  K-12) · OPEN-BUG **3** (K-10, K-15, L-27) · OPEN-MEASURE **1** (M-10) ·
  OPEN-ENHANCEMENT **3** (M-11, M-12, M-13).
  44 + 4 + 5 + 2 + 3 + 3 + 1 + 3 = **65** ✓.
- **FIX_PLAN arithmetic** (for reference, its "items" are line entries that sometimes
  bundle several IDs, so they do not map 1:1 to the 65): 47 active → −4 NOT-A-BUG = 43 →
  −5 second-order = 38 → −1 (K-13) = 42 → 37 through phases (43 phase line entries).
  The 65-vs-47 delta = L-14..L-18 design notes plus IDs FIX_PLAN bundled or carried
  implicitly; every one of those is individually classified above.
- **Open-work cross-check vs decided lane scope**: open-bugs = {K-15, L-27(+ loader
  stub)} ✓; in-lane fixes = {K-10/Gap 8+10, Gap 9, Gap 7, L-11, L-25, K-12-pending-A/B,
  M-10-measure} ✓; everything else FIXED/DEMOTED/enhancement ✓. Matches
  EXECUTION_PLAN 6b final scope exactly.
