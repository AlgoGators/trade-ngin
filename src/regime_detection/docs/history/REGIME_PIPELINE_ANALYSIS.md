# Regime Detection Pipeline — Full Analysis & Fix Backlog

**Analysis date**: 2026-04-20
**Branch**: `regime-detection`
**Data analyzed**: Macro panel 2011-01-01 → 2026-04-08 (T=4765); Market data 2024-01-02 → 2024-12-31 (T=311 per sleeve)

## 0. Executive Summary

The regime-detection branch implements a two-stage pipeline: a **Macro pipeline** (4 models → one 6-state belief) and a **Market pipeline** (4 models per sleeve → 5-state belief per sleeve). This review walks each model against its spec, validates the math against the runner output, and identifies bugs, misspecifications, and economic misjudgments.

**Headline findings**:

1. **Macro MS-DFM locks 90.5% of sample into `EXPANSION_DISINFLATION`** due to a cross-state standardization step that amplifies tiny deviations of the dominant native state. Same mechanism repeats in BSTS's 6-ontology mapping, producing 0% `EXPANSION_DISINFLATION` from BSTS and 0% `RECESSION_*` from Quadrant.
2. **DFM factor 0 is non-discriminative trend** (loads 0.85 on GDP, CPI, retail_sales — all secular-growth series). It's already excluded from regime labeling but still inflates the 3D Gaussian density. Removing it from the Gaussian classifier is the cleanest local fix.
3. **The Quadrant model structurally cannot dominant-pick RECESSION** because the hardcoded table has `SLOWDOWN_* > RECESSION_*` in every row. This matches spec, so it's by design — but the Quadrant's growth input is cap-util-dominated (unscaled averaging of level and slope), which is a separate bug.
4. **BSTS's R0-R3 greedy label assignment forces cluster 2 → "R3 Reflation" with score −1.886** (anti-reflation). Cosmetic because the pipeline uses cluster posteriors, not R0-R3 labels — but the diagnostic output is misleading.
5. **Market pipeline trains on 2024 only** (T=311). Insufficient data for 3-state HMMs and 5-cluster GMMs, and 2024 had no stress events — so stress regimes are uncalibrated.
6. **Commodities HMM state 2 has μ=3.4%/day, σ=0.18%/day** — a classic EM degenerate state. No σ floor in the EM implementation.
7. **Market pipeline's cross-state standardization** replicates the macro lock-in mechanism for HMM (3→5) and MSAR (2→5) mappings.
8. **Known gaps vs newer market spec** (MBFS, ML confirmer, cross-asset overlay) are intentional and will be addressed separately.

**Current-bar ensemble confidence = 0.064** (top-minus-second). Symptom of model disagreement cascading from upstream lock-in issues. Fixing MS-DFM and BSTS target fingerprints is expected to bring confidence toward 0.15-0.25 without touching aggregation.

---

## 1. Spec Context

Two spec documents exist for the macro pipeline; they conflict on the mapping methodology:

| Doc | Status | Mapping approach for B2/B3/B4 |
|---|---|---|
| `deliverables/regime/SYNTHESIZED_MACRO_PIPELINE.md` | **Authoritative** | Each model maps independently via its own method (fingerprint/rule-based). Explicitly says "NOT via matrices learned from DFM labels." |
| `macro_regime_pipeline.txt` | **Superseded addendum** | Uses DFM percentile labels as ground truth to train MS-DFM/Quadrant/BSTS mapping matrices. |

The newer spec explicitly rejects the older addendum's single-point-of-failure design: SYNTHESIZED doc line 48 — *"Key difference from the pipeline text: models B2, B3, B4 map independently via their own methods — NOT via matrices learned from DFM labels."*

The implementation correctly follows the newer spec.

**Market pipeline** follows the older `Regime Engine Algo PDF` (sections A0-A7), not the newer `SYNTHESIZED_MARKET_PIPELINE.md`. This is intentional per team decision. Newer-spec-only gaps (MBFS, ML confirmer, cross-asset overlay) are documented in `deliverables/regime/MARKET_PIPELINE_GAP_ANALYSIS.md` and are out of scope for this review.

---

## 2. Macro Pipeline

### 2.1 Architecture

```
Macro panel (24 columns, 4765 dates)
     │
     ├─[B1] DFM → 3 factors → percentile labels (growth: 70/20/50, inflation: 50/50)
     │       → fit 3D Gaussian per of 6 regimes → map_dfm(f_t) → 6 probs          [w=0.25]
     │
     ├─[B2] MS-DFM (3 latent states) → fingerprint per state (5D) → softmax distance
     │       → 3×6 mapping matrix → map_msdfm(p_native) → 6 probs                  [w=0.40]
     │
     ├─[B3] Quadrant → BSTS composite growth/inflation scores → tanh sigmoid blend
     │       of 4 quadrant weights → 4×6 hardcoded rule table → 6 probs             [w=0.20]
     │
     ├─[B4] BSTS (Kalman + PCA + GMM 4-cluster) → fingerprint per cluster (5D)
     │       → softmax distance → 4×6 mapping matrix → 6 probs                       [w=0.10]
     │
     └─ Weighted aggregation → EWMA smoothing (λ=0.20, warmup 10 bars)
        → hysteresis + dwell (26 bars, penalty 0.3) → MacroBelief
```

Config: `include/trade_ngin/statistics/state_estimation/macro_regime_pipeline.hpp:71-182`
Impl: `src/statistics/state_estimation/macro_regime_pipeline.cpp`
Runner: `apps/macro/macro_regime_pipeline_runner.cpp`

### 2.2 B1 — DFM

**Spec compliance**:

| Spec requirement | Impl | Status |
|---|---|---|
| Multivariate Gaussian per regime | 3D, with `+0.5·I` cov floor (cpp:319) | [OK] |
| Percentile-based labeling | 70/20/50 for growth, 50/50 for inflation (vs spec 33/33/33 + 50/50) | [WARN] documented calibration |
| Sign-flip factors when dominant loadings are negative | `growth_factor_sign = -1`, `inflation_factor_sign = -1` | [OK] |
| Log-sum-exp normalization | cpp:347-351 | [OK] |

**Factor analysis** (from runner output, top 3 loadings per factor):

| Factor | Label | Top loaders | Nature |
|---|---|---|---|
| 0 | macro_level | cpi (0.854), retail_sales (0.853), gdp (0.850) | **Non-discriminative trend** — all growing secular series |
| 1 | real_activity | mfg_cap_util (−0.603), ind_prod (−0.520), unemployment (+0.400) | Stationary industrial cycle |
| 2 | commodity_inflation | wti_crude (−0.429), breakeven_5y (−0.292), mfg_cap_util (+0.278) | Commodity + inflation expectations |

Regime counts match the 70/20/50 × 50/50 partition:
- Expansion (30%): 7.3% EXP_DIS + 22.7% EXP_INF
- Slowdown (50%): 30.5% SLO_DIS + 19.5% SLO_INF
- Recession (20%): 12.1% REC_DEF + 7.8% REC_INF

Within-bucket inflation split is NOT 50/50 — confirms growth-inflation correlation:
- Expansion 77% inflationary (commodity super-cycle, strong demand)
- Slowdown 61% disinflation (demand cooling)
- Recession 60% deflation

**Economic sanity**:

- 2011-2014 labeled `EXPANSION_INFLATIONARY` — correct (commodity super-cycle tail, post-GFC reflation)
- 2018-2019 labeled `EXPANSION_DISINFLATION` — correct (TCJA boost, moderate oil, sub-target breakevens)
- 2020 labeled `RECESSION_INFLATIONARY` — debatable, driven by H2 2020 oil recovery dominating H1 deflationary shock. BSTS picks REC_DEF for same year (structural-break view). Flag for ensemble in §2.6.
- **2025-2026 labeled `RECESSION_DEFLATIONARY` — economically wrong but mechanically correct**:
  - Cap-util ~77% + IP flat + unemployment 4% → unflipped f1 high → flipped f1 bottom-20% → "recession"
  - Moderate oil + compressed breakevens → flipped f2 below median → "disinflation"
  - Root cause: factors 1/2 capture only industrial-cycle signals; services economy absent (absorbed into factor 0 as trend, which is not used for labeling)

**Factor 0 discussion** (deep-dive rationale for excluding it from the Gaussian):

Per-regime factor-0 means span [-0.65, +0.65] — spread of 1.29, vs factor 1 spread 3.67 and factor 2 spread 4.11. Factor 0 has ~1/3 the discriminative power of the other factors. With the cov floor of 0.5·I, factor 0's Mahalanobis contribution is largely suppressed anyway. The remaining signal is correlation with time-in-sample (recessions concentrated in 2020 and late-sample when price levels are elevated).

All 24 panel variables remain consumed across the 4 models (direct use in Quadrant, BSTS Kalman, fingerprint). Factor 0 is only about the **Gaussian classifier step**; all cyclical content of GDP/CPI/payrolls/retail-sales already leaks into factors 1 and 2 via smaller secondary loadings in the DFM's linear decomposition.

---

### 2.3 B2 — MS-DFM

**Spec compliance**:

| Spec requirement | Impl | Status |
|---|---|---|
| Fingerprint-based mapping | `train_msdfm_fingerprints()` at cpp:503-609 | [OK] — correctly follows newer spec |
| 5D fingerprint [growth, inflation, credit, yield, policy] | native_fingerprints = 5-dim | [OK] |
| Growth columns YoY-differenced, others z-scored | `prepare_fingerprint_data()` cpp:398-462 | [OK] — fixed previous time-trend lock-in |
| Cross-state standardization | cpp:538-561 — Stage-2 renorm after per-column z-scoring | [BUG] — root of current lock-in |

**The 90% EXP_DIS lock-in — mechanics**:

Native state distribution:
- State 0 (n=4307, 90.4%): "calm"
- State 1 (n=321, 6.7%): "modest stress"
- State 2 (n=137, 2.9%): "severe stress" (clusters in 2020, 2022)

State 0's raw fingerprint sits near zero on every dim (averaging 4307 z-scored observations ≈ global mean ≈ 0). After cross-state standardization across only J=3 natives (ns_std ≈ 0.06-0.09), the tiny 0.005-0.1 residual offsets amplify to z-scores of 0.7-1.4.

Math verification against runner output `native 0: [0.548, 0.076, 0.272, 0.041, 0.053, 0.010]`:

```
ns_mean    ≈ [-0.056,  0.099,  0.041, -0.061,  0.003]
ns_std     =  [ 0.089,  0.084,  0.063,  0.058,  0.008]
native_0_std ≈ [+0.68, -1.37, -0.71, +1.21, -0.46]

Distances to targets (τ=1):
  EXP_DIS [+1.5,-1.0,-1.0,+1.0,-0.5]: 0.97 → softmax 0.547
  SLO_DIS [ 0.0,-1.0, 0.0, 0.0, 0.0]: 1.67 → softmax 0.272
  EXP_INF [+1.5,+1.0,-0.5, 0.0,+0.5]: 2.95 → softmax 0.076
  REC_DEF [-1.5,-1.5,+1.5,+1.5,-1.5]: 3.29 → softmax 0.054
  SLO_INF [ 0.0,+1.0,+0.5,-0.5,+1.0]: 3.55 → softmax 0.041
  REC_INF [-1.5,+1.5,+1.5,-0.5,+1.5]: 4.96 → softmax 0.010
```

Exactly reproduces the runner output. Math is bug-free; the **design** is what's wrong.

**Root cause**: native 0's standardized fingerprint `[+0.68, -1.37, -0.71, +1.21, -0.46]` is best read as "not-stress" — slightly positive growth, below-average inflation, narrower credit, steeper yield curve (average across 15 years is normal/steep), slightly looser policy. This profile is closest to EXP_DIS simply because EXP_DIS is the "normal good times" target.

**Additional issues**:

- Fingerprints trained on **hard-decoded** argmax states (cpp:517), not soft probabilities. Training/runtime asymmetry (runtime uses soft probs in `map_msdfm` at cpp:616-618).
- **3-native-state ceiling**: MS-DFM separates "calm / modest-stress / severe-stress" (a volatility-regime taxonomy), not growth-inflation quadrants. Structurally cannot map 3 volatility states to 6 growth×inflation regimes.

---

### 2.4 B3 — Quadrant

**Spec compliance**:

| Spec requirement | Impl | Status |
|---|---|---|
| Rule-based 4×6 table | Hardcoded at cpp:133-137, matches spec line 110-115 exactly | [OK] |
| Smooth tanh-sigmoid boundary blending | cpp:642-657 | [OK] |
| Growth/inflation scores from BSTS composites | Runner:261-262 | [OK] |

**Table** (columns = 6 ontology states, rows = 4 quadrants):

```
              EXP_DIS  EXP_INF  SLO_DIS  SLO_INF  REC_DEF  REC_INF
GOLDILOCKS     0.80     0.15     0.05     0.00     0.00     0.00
REFLATION      0.05     0.85     0.00     0.10     0.00     0.00
DEFLATION      0.15     0.00     0.50     0.00     0.35     0.00
STAGFLATION    0.00     0.15     0.00     0.55     0.00     0.30
```

**Proof that Quadrant dominant pick cannot be RECESSION**:

Final = convex combination of 4 rows. REC_DEF receives weight only from row 2 (DEFLATION at 0.35), but SLO_DIS in the same row is 0.50 → P(REC_DEF) = 0.35·w_DEF < 0.50·w_DEF = P(SLO_DIS) for any w_DEF ≥ 0. Identically: REC_INF 0.30 vs SLO_INF 0.55 in row 3.

**This matches spec** — it's by design. Quadrant contributes probability mass to RECESSION (0.35 and 0.30 in the two rows) via the aggregate weighted sum, just doesn't vote recession as its own dominant pick. Other models (DFM, BSTS) carry the recession-calling load.

Evidence the ensemble redundancy works: 2020 diagnostic row shows `Quadrant dominant = SLOWDOWN_INFLATIONARY` but `Ensemble dominant = RECESSION_INFLATIONARY` — DFM+BSTS votes overcame Quadrant's SLO_INF vote.

**Real bug — growth_score composition** (`bsts_regime_detector.cpp:366-378`):

```cpp
gs = (mslope("industrial_production", t)   // ~0.01 scale
    + mlevel("manufacturing_capacity_util", t)   // ~78 scale
    + mslope("gdp", t)) / 3.0;                   // ~0.01 scale
```

The three components are averaged **without standardization**. Cap-util (~78) dominates; IP and GDP slopes contribute < 0.05% of the score. So "growth_score" is effectively just manufacturing capacity utilization.

After z-scoring with training medians/stds, this becomes "is cap-util above/below its historical median" — sensible industrial-cycle signal, but misses services economy. Combined with the DFM's same services-blind tendency, 2025 is reliably labeled industrial-slowdown-themed by both models.

**Secondary concern — inflation_score uses levels, not changes**: averaging CPI level + core PCE level + breakeven level. So `inflation_score` is "how high is inflation right now," not "is inflation rising/falling." Consequence: 2023-2024 (inflation falling from 9% → 3%) looks STAGFLATION to the Quadrant because level is still elevated.

---

### 2.5 B4 — BSTS / GMM

**Architecture**:

```
ETFs (8 incl SPY, EEM, TLT, HYG, GLD, UUP, USO, CPER)
Macro panel (12 series)
     │
     ▼
BSTS state-space (Kalman filter per series)
     │
     ▼
50-dim feature matrix (32 ETF + 12 macro + 6 composites)
     │
     ▼
PCA (12 components, ~70% variance)
     │
     ▼
GMM 4 clusters (K-means++ init, 10 restarts)
     │
     ├──→ R0-R3 greedy label assignment → bsts_out.regime_labels (diagnostic only)
     │
     └──→ Cluster posteriors T×4 → macro pipeline's fingerprint mapping
                                    → 4×6 softmax distance matrix
                                    → 6 ontology probs
```

**Cluster drift (economically correct)**:

| Period | Dominant cluster | Raw profile | Pipeline B4 output |
|---|---|---|---|
| 2011-2019 | c0 (modest gi, pos gs, low inf) | post-GFC slow growth | SLO_DIS (68.4% of sample) |
| 2020 | c1 (high stress, low gs, high ros) | COVID crisis | REC_DEF (4.6%) |
| 2022-2025 | c2 (slightly stagflationary) | inflation spike + rate shock | SLO_INF (25.3%) |
| 2026 | c2 → c3 transition | industrial weak + sticky inflation | SLO_INF drifting toward EXP_INF (1.7%) |

**Issue A — Greedy label assignment forces anti-matching labels**:

Cluster × signature scores (MACRO_SIGNATURES at cpp:41-46):

```
cluster 0 × R0 (Risk-On) = 3.44  → cluster 0 → R0
cluster 3 × R2 (Stagflat) = 3.90  → cluster 3 → R2
cluster 1 × R1 (Risk-Off) = 2.61  → cluster 1 → R1
cluster 2 × R3 (Reflation) = -1.89 → cluster 2 → R3  ← NEGATIVE SCORE
```

Greedy assignment at cpp:693-712 forces cluster 2 into R3 "Reflation" even though its score is -1.886 (anti-reflation). Cluster 2's actual profile (gs=-0.85, inf=+0.12, ros=-1.56) is classic slowdown-inflation, not reflation.

2022-2025 are 100% cluster 2 — labeled "Reflation" in the BSTS R0-R3 output, which is misleading. **Pipeline's macro regime output is not affected** (it uses cluster posteriors, not R0-R3 labels), so this is a diagnostic bug only.

**Issue B — BSTS never picks EXP_DIS (0.0% full sample)**:

Same mechanism as MS-DFM in §2.3. Target fingerprints are **reused from MS-DFM** (cpp:715 explicitly: `bsts_mapping_.target_fingerprints = msdfm_mapping_.target_fingerprints`). EXP_DIS target `[+1.5, -1.0, -1.0, +1.0, -0.5]` is outside the cross-state-standardized cluster hull (with only 4 clusters, std is especially noisy and standardized z-scores cap around ±1.2).

Result: SLO_DIS target (near origin, [0, -1, 0, 0, 0]) becomes the default winner for any cluster whose standardized fingerprint is near zero.

**Issue C — Structural-break gating bypassed**:

Config `bsts_always_contribute = true` at hpp:126. Spec says BSTS should output uniform when no break is detected (line 125 of SYNTHESIZED_MACRO_PIPELINE.md). Impl always uses fingerprint mapping. Documented calibration deviation to avoid wasting 10% of ensemble weight on uniform during 97.8% non-break periods.

---

### 2.6 Aggregation

**Weights** (hpp:74-77): MS-DFM 0.40, DFM 0.25, Quadrant 0.20, BSTS 0.10 — matches spec.

**Smoothing** (hpp:134):
- `base_lambda = 0.20` (spec 0.10, scaled 2× for daily vs monthly data — documented)
- `calm_lambda_scale = 1.0` (spec 0.5 — same scaling)
- Warmup 10 bars at λ=1.0 (pure raw), then switch to config λ
- Effective half-life on daily data: ~3 bars

**Hysteresis** (hpp:148-153):
- Enter defensive/risk-off/aggressive: 0.20 / 0.22 / 0.20 (spec 0.55 / 0.60 / 0.70)
- Relative-lead escape hatch at cpp:887: `lead > 0.05` triggers transition regardless of absolute thresholds
- Spec thresholds would make transitions impossible with 6 regimes at ~30% max probability

**Dwell** (hpp:159): 26 bars (spec 130 = 6 months; scaled for daily), penalty 0.3

**Current-bar decomposition** (2026-04-08, dominant=SLO_INF@0.309, confidence=0.064):

| Regime | MS-DFM×0.40 | DFM×0.25 | Quadrant×0.20 | BSTS×0.10 | Raw sum | Smoothed |
|---|---|---|---|---|---|---|
| EXP_DIS | **0.219** | ~0.013 | ~0.000 | ~0.015 | ~0.247 | 0.215 |
| EXP_INF | 0.030 | 0.050 | 0.030 | **0.030** | ~0.140 | 0.245 |
| SLO_DIS | 0.109 | ~0.013 | ~0.000 | ~0.015 | ~0.137 | 0.127 |
| SLO_INF | 0.016 | **0.128** | **0.104** | ~0.010 | **~0.258** | 0.309 |
| REC_DEF | 0.021 | 0.025 | ~0.000 | ~0.005 | ~0.051 | 0.026 |
| REC_INF | 0.004 | 0.022 | 0.060 | ~0.010 | ~0.096 | 0.078 |

Read: MS-DFM's 0.40 weight permanently anchors 0.219 on EXP_DIS via native state 0's EXP_DIS bias. DFM + Quadrant combined (0.45 weight) vote SLO_INF at 0.232. The raw ensemble is a 3-way tie (SLO_INF 0.258, EXP_DIS 0.247, EXP_INF 0.140). Smoothing over 58 bars widened SLO_INF's lead because it won most days, but EXP_DIS keeps getting pumped every bar by MS-DFM's lock-in.

**Confidence = top - second = 0.064**. Not aggregation's fault — it's baked into the raw ensemble from MS-DFM's structural bias.

**Failure degradation**: cpp:937-950 has redistribution logic for NaN / all-zero outputs. Does not catch "biased-valid" outputs (MS-DFM). Architecturally correct but can't help here.

### 2.7 Macro — Overall status

**Working as spec**:
- DFM structure, factor signs, multivariate Gaussian, log-sum-exp normalization
- MS-DFM fingerprint mechanism (right design — bad calibration)
- Quadrant hardcoded 4×6 table with tanh blend, matches spec exactly
- BSTS GMM clustering identifies 4 real macro eras correctly (post-GFC, COVID, inflation, stabilization)
- Aggregation weighted sum, warmup smoothing, relative-lead hysteresis
- Failure-degradation redistribution architecture

**Broken**:
- **Target fingerprint scale ±1.5 outside cross-state-standardized cluster hull** — affects MS-DFM and BSTS; causes 0% EXP_DIS lock-in (BSTS) and 90% EXP_DIS lock-in (MS-DFM)
- **Quadrant growth_score is cap-util-dominated** (unscaled averaging of components on different magnitude scales)
- **BSTS R0-R3 greedy label assignment** forces anti-matching labels (cluster 2 → R3 with score -1.886)

**Defensibly different from spec**:
- DFM factor 0 discarded from regime labeling (non-discriminative trend)
- DFM percentile 70/20/50 vs spec tercile (reduces over-use of "recession" bucket)
- `bsts_always_contribute = true` overrides spec's "uniform when no break"
- Smoothing/hysteresis/dwell all scaled for daily data (spec assumed monthly)

**Economically odd but explainable**:
- 2025-2026 DFM = RECESSION_DEFLATIONARY — industrial-side only, no services signal
- 2020 DFM = REC_INF (H2 oil recovery dominates) vs BSTS = REC_DEF (structural-break view)

---

## 3. Market Pipeline

Target spec: older `Regime Engine Algo PDF` sections A0-A7. Newer-spec gaps (MBFS, ML confirmer, cross-asset overlay) deferred.

### 3.1 Architecture

```
Per sleeve (equities / rates / fx / commodities):

Returns + volumes (sleeve-specific symbols)
     │
     ├─[A1] HMM 3-state → emission [μ_j, σ_j] → 2D fingerprint
     │       → softmax distance → 3×5 mapping → 5 probs                       [w=0.40]
     │
     ├─[A2] MSAR 2-state → [μ_j, σ_j, φ_j] → 3D fingerprint
     │       → softmax distance → 2×5 mapping → 5 probs                       [w=0.30]
     │
     ├─[A3] GARCH / EGARCH → σ_t + vol_percentile → 4-bin rule-based table
     │       → adjustments (spike/vov/asymmetry/liquidity) → 5 probs         [w=0.20]
     │
     ├─[A4] GMM 5-cluster on 5D features [r, σ̂, dd_speed, vol_shock, corr_spike]
     │       → feature-mean fingerprint per cluster → 5D softmax
     │       → 5×5 mapping → 5 probs                                          [w=0.10]
     │
     └─ Weighted aggregation → EWMA smoothing (λ=0.30 constant)
        → argmax on smoothed → MarketBelief
```

Config: `include/trade_ngin/statistics/state_estimation/market_regime_pipeline.hpp`
Impl: `src/statistics/state_estimation/market_regime_pipeline.cpp`
Runner: `apps/macro/market_regime_pipeline_runner.cpp`

### 3.2 Per-sleeve observations (2024 data, T=311)

**Equities (MES, MNQ, MYM)** — final `TREND_LOWVOL` conf 0.019

Economically correct: 2024 S&P bull year (~+25%, low vol). HMM state 1 (μ=+0.00087, σ=0.0063) maps to TREND_LOWVOL (0.69). State 0 (drawdown) maps to STRESS_PRICE (0.43). MSAR φ ≈ 0.07 / 0.008 — small AR, consistent with noisy equity returns. Final probabilities bunched (TREND_LOWVOL 0.32, MEANREV 0.30) because GARCH votes MEANREV_CHOPPY (vol in mid-bin) while HMM votes TREND_LOWVOL.

**Rates (ZN, ZF)** — final `TREND_LOWVOL` conf 0.005

All 3 HMM states have nearly identical σ (~0.0034) and near-zero μ. HMM cannot distinguish regimes in this data. Rendered regime label is a coin flip (0.298 vs 0.293). Display should flag low conviction.

**FX (6E, 6J, 6B, 6A)** — final `STRESS_PRICE` conf 0.115

The only sleeve with a non-TREND call. HMM state 0 (μ=-0.00089, σ=0.0049) maps STRESS_PRICE 0.51. Almost certainly picking up Sep-Dec 2024 USD surge (DXY 99→108). **But a strong trending DXY move is not "stress"** — the ontology mislabels sustained FX trends. MSAR φ=-0.14 (mean-reverting) is appropriate for FX.

**Commodities (CL, NG, GC, HG)** — final `TREND_LOWVOL` conf 0.028

HMM state 2: **μ=3.4%/day, σ=0.18%/day — degenerate EM solution**. Probably 1-3 outlier days got isolated. State 2's near-zero posterior means runtime impact is small, but any Mahalanobis computation involving this state will be unstable.

### 3.3 Issues within the older PDF spec

See issue list in §4 for cross-referenced locations and fixes. Brief summary of what's beyond the known MBFS/ML/overlay gaps:

1. **Cross-state standardization replicates macro lock-in** for HMM (3→5) and MSAR (2→5). MSAR worst-case: with J=2, standardized natives sit at exact opposites → only binary partition possible.
2. **No σ floor in HMM/MSAR EM** → degenerate states (commodities state 2).
3. **HMM conflates trend-direction with trend-regime** — downtrends labeled stress, not TREND.
4. **STRESS_PRICE vs STRESS_LIQUIDITY targets near-collinear** in HMM 2D space (distance 0.71 vs MEANREV→STRESS 1.80). HMM cannot differentiate them.
5. **MSAR transition matrix inconsistency**: intercepts/vars/AR from MarketMSAR, transition from pre-AR MarkovSwitching.
6. **GMM feature col 3 labeled "vol_shock" but computes volume ratio** — naming/targets mismatch.
7. **liquidity_proxy defaults to 1.0 when volumes missing** → STRESS_LIQUIDITY never triggers from liquidity signal path.
8. **Cross-asset correlation spike is within-sleeve only** — for equities = MES/MNQ/MYM mutual correlation (always ~0.95, near-useless as stress signal).
9. **O(T³) drawdown_speed** — hidden perf bomb if update loop is ever run over full history.
10. **Smoothing init contamination** — update loop runs only last 5 bars; `prev_smoothed` is uniform → reported confidence is half-uniform-noise.
11. **Target fingerprints identical across sleeves** — missed sleeve-customization feature.
12. **Stability metric computed but never printed**.
13. **EGARCH γ threshold arbitrary** (`< -0.01`).
14. **GARCH adjustments additive in probability space** (not log-odds) — should still work after normalize but brittle.
15. **Training on 2024 only (T=311)** — insufficient data for 3-state HMM + 5-cluster GMM; no stress events → uncalibrated stress regimes.
16. **Printf bugs**: rates MSAR state 1 missing σ, fx "MSAR 3" should be "MSAR 1".

---

## 4. Issue Backlog (Prioritized)

### P0 — Must fix (high impact, low cost)

| ID | Issue | Location | Fix summary | Impact |
|---|---|---|---|---|
| M-01 | MS-DFM 90% EXP_DIS lock-in via cross-state standardization | `macro_regime_pipeline.cpp:538-561` | **Drop cross-state /std (keep de-mean only), OR scale target fingerprints from ±1.5 to ±0.8** | Unlocks MS-DFM, fixes cascading ensemble confidence drop |
| M-02 | BSTS 0% EXP_DIS — same cross-state std mechanism | `macro_regime_pipeline.cpp:689-712` + targets at cpp:715 (reused from MS-DFM) | Same fix as M-01 (they share target fingerprints) | BSTS starts voting for expansion regimes |
| M-03 | Quadrant `growth_score` is cap-util-dominated | `bsts_regime_detector.cpp:366-370` | Z-score each component before averaging | Quadrant finally sees IP and GDP slope signals |
| M-04 | DFM factor 0 included in Gaussian but non-discriminative | `macro_regime_pipeline.cpp:232-354`, hpp:189-195 | Reduce Gaussians + `map_dfm` from 3D to 2D (factors 1,2 only); leave DFM decomposition at 3 factors for MS-DFM | Sharpens DFM classification; factor 0 noise no longer dilutes density |
| K-01 | Market HMM/MSAR σ collapse (commodities state 2) | `src/statistics/state_estimation/markov_switching.cpp` (M-step) | Add `σ_j² = max(σ_j², 0.01 × global_variance)` floor | Eliminates degenerate states |
| K-02 | Market training data only 2024 (T=311) | `apps/macro/market_regime_pipeline_runner.cpp:525-526` | Change default to 2020-2025 (T~1500) | Proper stress calibration (COVID, 2022 rate shock, SVB, yen unwind) |
| K-03 | GMM feature col 3 is volume ratio but labeled/targeted as vol_shock | `market_regime_pipeline_runner.cpp:164-169` + targets at `market_regime_pipeline.cpp:491-495` | Rename to `volume_ratio` and retune targets, OR replace col 3 with `(garch_vol[t] - garch_vol[t-20]) / garch_vol[t-20]` | GMM's 5D space becomes correctly interpreted |

### P1 — Important (calibration / economic soundness)

| ID | Issue | Location | Fix |
|---|---|---|---|
| M-05 | Quadrant inflation_score uses levels, not changes | `bsts_regime_detector.cpp:372-373` | Use YoY-change for CPI/PCE, keep breakeven as level |
| M-06 | BSTS R0-R3 greedy labeling forces anti-matching (cluster 2 → R3 @ -1.886) | `bsts_regime_detector.cpp:693-712` | Reject score ≤ 0, mark as "Unclassified" |
| M-07 | MS-DFM fingerprints trained on hard argmax, runtime uses soft probs | `macro_regime_pipeline.cpp:517` | Weight fingerprint aggregation by `smoothed_probs(t, j)` |
| M-08 | DFM labels imply recession for industrial slowdown + services decent | Conceptual | Either add services proxies (ISM-services) to factor 1, or widen recession bucket to bottom 10%, or rename bucket "INDUSTRIAL_WEAKNESS" |
| M-09 | Aggregation confidence metric only reports top-minus-second | `macro_regime_pipeline.cpp:903-910` + MacroBelief struct | Add `entropy_concentration` and `top_prob` fields alongside |
| M-10 | enter_riskoff only 2pp above enter_defensive | `macro_regime_pipeline.hpp:148-153` | Raise enter_riskoff to 0.30 (keep relative-lead escape hatch) |
| K-04 | HMM trend concept = signed drift, misclassifies downtrends as stress | `market_regime_pipeline.cpp:242-265` | Use `|μ|` as trend dim, or switch HMM fingerprint to include autocorrelation |
| K-05 | STRESS_PRICE vs STRESS_LIQUIDITY near-collinear in HMM 2D space | Target fingerprints cpp:260-264 | Add a 3rd dim (liquidity proxy) to HMM fingerprint once volumes are reliable |
| K-06 | MSAR transition matrix inconsistency | Runner:420-423 | Expose `get_transition_matrix()` on MarketMSAR or re-fit MS with AR-adjusted returns |
| K-07 | liquidity_proxy defaults to 1.0 with missing volumes → STRESS_LIQUIDITY muted | Runner:98-104, cpp:437-442 | Add `has_reliable_volume` per-sleeve flag; gate liquidity adjustments on it |
| K-08 | Cross-asset correlation spike is within-sleeve (useless for equities) | Runner:569-576 | Pool composite_returns across all sleeves into a global matrix, compute cross-sleeve rolling correlation |
| K-09 | Smoothing init contamination in 5-bar update loop | Runner:471 | Run update loop over full T, OR seed `prev_smoothed` from trailing 20-bar raw average |
| K-10 | Target fingerprints identical across sleeves | `market_regime_pipeline.cpp:260-264, 332-336, 491-495` | Add sleeve-specific target overrides in `SleeveConfig` |

### P2 — Polish (low impact)

| ID | Issue | Location | Fix |
|---|---|---|---|
| M-11 | DFM cov floor 0.5·I is tuned but unexplored sensitivity | `macro_regime_pipeline.cpp:319` | Expose as config; A/B test 0.2-0.7 |
| M-12 | Dwell penalty 0.3 may slightly mask legitimate transitions | `macro_regime_pipeline.hpp:160` | Lower to 0.15 |
| M-13 | Cold-start warmup might need extending for live deployment | `macro_regime_pipeline.cpp:801-805` | Extend warmup to 30-50 bars for live, or persist smoother state |
| K-11 | Stability metric computed but never printed | Runner `print_belief` | Print alongside confidence |
| K-12 | EGARCH γ significance threshold arbitrary | Runner:501 | Use standard-error-based significance test |
| K-13 | GARCH adjustments additive, not log-odds | `market_regime_pipeline.cpp:417-441` | Convert to log-odds multiplicative |
| K-14 | O(T³) drawdown_speed hidden cost | Runner:198-203 | Maintain running cumsum/peak incrementally |
| K-15 | Printf bugs (rates MSAR σ missing, fx MSAR state label typo) | Runner output format | Fix format strings |
| K-16 | `corr_spike` needs 252 bars for z-score but T=311 → 59 valid bars | Runner:255 | Reduce z-score window to min(252, T/2), or use raw correlation with different scoring |
| K-17 | No persistent state for live deployment | `SleeveTrainedState::prev_smoothed` | Add serialization hooks |

---

## 5. Recommended Sequencing

**Phase 1 — Macro unlocks (1-2 days)**:
1. M-04 (DFM → 2D Gaussians, factor 0 excluded)
2. M-01 + M-02 (scale target fingerprints to ±0.8, or drop cross-state std)
3. M-03 (fix growth_score component scaling)
4. Re-run the macro pipeline and verify:
   - MS-DFM dominant pick distribution is not 90% EXP_DIS
   - BSTS dominant pick distribution includes EXP_DIS > 0
   - Current-bar confidence rises from 0.064 toward 0.15+

**Phase 2 — Market data + EM robustness (1 day)**:
5. K-02 (retrain on 2020-2025)
6. K-01 (HMM/MSAR σ floor)
7. K-09 (smoothing warmup)
8. K-03 (GMM col 3 rename or replace)
9. Re-run market pipeline and verify:
   - No degenerate HMM states
   - Confidence above 0.05 for sleeves with clear regime (equities bull year)
   - Stress regimes have non-trivial probability during Mar 2020, Mar 2023

**Phase 3 — Economic refinement (2-3 days)**:
10. M-05 (inflation_score use changes)
11. M-08 (DFM services proxies OR relabel)
12. M-06 (BSTS R0-R3 labeling)
13. M-07 (MS-DFM soft-prob-weighted fingerprints)
14. K-04, K-06, K-07, K-08 (market economic fixes)

**Phase 4 — Polish + tooling (ongoing)**:
15. Remaining P1 items
16. All P2 items
17. Add live-deployment serialization (K-17, M-13)

---

## 6. Appendix

### 6.1 Macro panel columns (24)

```
GROWTH (6):      nonfarm_payrolls, unemployment_rate, manufacturing_capacity_util,
                 industrial_production, retail_sales, gdp
INFLATION (4):   cpi, core_cpi, core_pce, breakeven_5y
YIELD CURVE (4): treasury_2y, treasury_10y, yield_spread_10y_2y, fed_funds_rate
CREDIT (2):      ig_credit_spread, high_yield_spread
LIQUIDITY (3):   m2_money_supply, ted_spread, fed_balance_sheet
MARKET (5):      vix, dxy, tips_10y, wti_crude, gdp_nowcast
```

### 6.2 DFM per-regime Gaussian means (unflipped factors)

```
EXPANSION_DISINFLATION  n=350    mean=[ 0.093, -0.972,  1.314]
EXPANSION_INFLATIONARY  n=1080   mean=[-0.645, -1.282, -2.238]
SLOWDOWN_DISINFLATION   n=1453   mean=[-0.039, -0.030,  1.869]
SLOWDOWN_INFLATIONARY   n=929    mean=[ 0.110, -0.109, -1.374]
RECESSION_DEFLATIONARY  n=579    mean=[ 0.648,  1.683,  1.077]
RECESSION_INFLATIONARY  n=374    mean=[ 0.647,  2.391, -0.296]
```

Factor 1 and 2 are sign-flipped for labeling: `flipped = -unflipped`. Flipped factor 1 high = growth, flipped factor 2 high = inflation.

### 6.3 Macro target fingerprints (MS-DFM and BSTS share)

```
                      growth  inflation  credit  yield  policy
EXP_DIS (t0):         +1.5     -1.0      -1.0    +1.0   -0.5
EXP_INF (t1):         +1.5     +1.0      -0.5     0.0   +0.5
SLO_DIS (t2):          0.0     -1.0       0.0     0.0    0.0
SLO_INF (t3):          0.0     +1.0      +0.5    -0.5   +1.0
REC_DEF (t4):         -1.5     -1.5      +1.5    +1.5   -1.5
REC_INF (t5):         -1.5     +1.5      +1.5    -0.5   +1.5
```

### 6.4 Market target fingerprints

HMM (2D `[μ, σ]`):
```
TREND_LOWVOL:      [+1.5, -1.5]
TREND_HIGHVOL:     [+0.5, +1.0]
MEANREV_CHOPPY:    [ 0.0,  0.0]
STRESS_PRICE:      [-1.0, +1.5]
STRESS_LIQUIDITY:  [-0.5, +2.0]
```

MSAR (3D `[μ, σ, φ]`):
```
TREND_LOWVOL:      [+1.5, -1.5, +1.5]
TREND_HIGHVOL:     [+0.5, +1.0, +1.0]
MEANREV_CHOPPY:    [ 0.0,  0.0, -1.5]
STRESS_PRICE:      [-1.0, +1.5, -0.5]
STRESS_LIQUIDITY:  [-0.5, +2.0, -1.0]
```

GMM (5D `[r, σ̂, dd_speed, vol_shock, corr_spike]`):
```
TREND_LOWVOL:      [+0.5, -1.5,  0.0, -1.0, -0.5]
TREND_HIGHVOL:     [+0.3, +1.0,  0.0,  0.0, +0.5]
MEANREV_CHOPPY:    [ 0.0,  0.0,  0.0, -0.5,  0.0]
STRESS_PRICE:      [-1.0, +1.5, +1.5, +1.0, +1.5]
STRESS_LIQUIDITY:  [-0.5, +2.0, +1.0, +2.0, +1.5]
```

### 6.5 Quadrant hardcoded table (4×6)

```
              EXP_DIS  EXP_INF  SLO_DIS  SLO_INF  REC_DEF  REC_INF
GOLDILOCKS     0.80     0.15     0.05     0.00     0.00     0.00
REFLATION      0.05     0.85     0.00     0.10     0.00     0.00
DEFLATION      0.15     0.00     0.50     0.00     0.35     0.00
STAGFLATION    0.00     0.15     0.00     0.55     0.00     0.30
```

### 6.6 BSTS MACRO_SIGNATURES for R0-R3 scoring

Columns: `[gi_quad, growth, inflation, stress, ros]`
```
R0 Risk-On Growth:   [ 2.0,  1.0,  0.0, -1.0,  1.0]
R1 Risk-Off/Crash:   [-1.0, -1.0,  0.0,  2.0, -1.0]
R2 Stagflation:      [-2.0, -1.0,  1.5,  0.0, -1.0]
R3 Reflation:        [ 1.0,  1.0,  0.5, -1.0,  1.0]
```

### 6.7 Market sleeve configs (weights)

All four sleeves share the same weights in the current config:
- HMM: 0.40 (rates: 0.38)
- MSAR: 0.30
- GARCH: 0.20 (rates: 0.22)
- GMM: 0.10

Per-sleeve vol thresholds (`SleeveThresholds`):
```
Equities:    trend_lowvol < 12%   stress > 35%
Rates:       trend_lowvol < 8%    stress > 20%
FX:          trend_lowvol < 6%    stress > 15%
Commodities: trend_lowvol < 18%   stress > 40%
```

### 6.8 Key code pointers

```
Macro pipeline:
  Config: include/trade_ngin/statistics/state_estimation/macro_regime_pipeline.hpp
  Impl: src/statistics/state_estimation/macro_regime_pipeline.cpp
  Runner: apps/macro/macro_regime_pipeline_runner.cpp
  DFM: src/statistics/state_estimation/dynamic_factor_model.cpp
  MS-DFM: src/statistics/state_estimation/ms_dfm.cpp
  BSTS: src/statistics/state_estimation/bsts_regime_detector.cpp
  Panel loader: src/statistics/state_estimation/macro_data_loader.cpp

Market pipeline:
  Config: include/trade_ngin/statistics/state_estimation/market_regime_pipeline.hpp
  Impl: src/statistics/state_estimation/market_regime_pipeline.cpp
  Runner: apps/macro/market_regime_pipeline_runner.cpp
  HMM / MSAR: src/statistics/state_estimation/markov_switching.cpp
  GARCH / EGARCH: src/statistics/volatility/egarch.cpp
  GMM: src/statistics/clustering/gmm.cpp
  Market loader: src/statistics/state_estimation/market_data_loader.cpp

Specs:
  deliverables/regime/SYNTHESIZED_MACRO_PIPELINE.md     (authoritative macro)
  deliverables/regime/SYNTHESIZED_MARKET_PIPELINE.md    (future market target)
  deliverables/regime/MARKET_PIPELINE_GAP_ANALYSIS.md   (known market gaps)
  deliverables/regime/regime_detection_architecture.md  (master architecture)
  macro_regime_pipeline.txt                             (older addendum — superseded)
```

---

*End of analysis. Update this doc as fixes land; keep the backlog table as the source of truth for what's open.*
