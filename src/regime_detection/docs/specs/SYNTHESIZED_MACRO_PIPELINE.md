# Synthesized Macro Regime Pipeline

## The Idea in a Few Sentences

Four macro models (DFM, MS-DFM, Growth-Inflation Quadrant, BSTS) each independently map their native outputs into probabilities over the same six macro regime states. Each model uses the mapping method that best fits its output type — DFM uses trained multivariate Gaussians (because it outputs continuous factors), while the others use fingerprint-based or rule-based mappings (because they output discrete states or probabilities). No model defines "ground truth" for any other model. The four resulting 6-probability vectors are combined via a weighted blend with persistence smoothing, hysteresis, and conflict resolution into a single `MacroBelief` shared across all portfolio sleeves.

---

## Macro Ontology (6 States)

All models ultimately produce probabilities over these six regimes, defined by the intersection of growth (3 buckets) and inflation (2 buckets):

| State | Growth | Inflation |
|-------|--------|-----------|
| `EXPANSION_DISINFLATION` | Strong | Falling/low |
| `EXPANSION_INFLATIONARY` | Strong | Rising/high |
| `SLOWDOWN_DISINFLATION` | Weakening | Falling/low |
| `SLOWDOWN_INFLATIONARY` | Weakening | Rising/high |
| `RECESSION_DEFLATIONARY` | Contraction | Collapsing |
| `RECESSION_INFLATIONARY` | Contraction | Sticky/rising |

---

## Model Dependency Chain

```
Macro panel data (PostgreSQL / CSV)
     │
     ├──────────────────────────────────────────────────────────┐
     │                                                          │
     ▼                                                          ▼
[B1] DFM ──> 3 factors ──> Trained Gaussians ──> 6 probs    [B2] MS-DFM ──> 3 native probs ──> Fingerprint map ──> 6 probs
                                                    │                                                     │
                                                    │        [B3] Quadrant ──> 4 quadrant probs ──> Rule-based map ──> 6 probs
                                                    │                                                     │
                                                    │        [B4] BSTS/GMM ──> break probs ──> Fingerprint map ──> 6 probs
                                                    │                                                     │
                                                    ▼                                                     ▼
                                                              Weighted Aggregation
                                                                      │
                                                                      ▼
                                                             Persistence Smoothing
                                                                      │
                                                                      ▼
                                                                MacroBelief
```

Key difference from the pipeline text: models B2, B3, B4 map independently via their own methods — NOT via matrices learned from DFM labels.

---

## Model-by-Model Specification

### B1: DFM — Trained Multivariate Gaussians

**Why this method**: DFM outputs 3 continuous factors `f_t = [growth, inflation, liquidity]`. These factors live in a continuous space where each regime occupies a distinct region. Multivariate Gaussians capture the joint distribution (growth and inflation co-move differently per regime), which is strictly better than treating each factor independently or using generic distance metrics.

**Training**:

1. Run DFM on historical macro panel to get factor time series `f_t`
2. Label each timestep using percentile thresholds on factors:
   - Growth factor: upper tercile = Expansion, middle = Slowdown, lower = Recession
   - Inflation factor: upper half = Inflationary, lower half = Disinflationary
   - Combined: 3 x 2 = 6 regime labels
   - Percentile splits are configurable (terciles, quartiles, asymmetric)
3. For each of the 6 regimes, collect all `f_t` values from that regime
4. Fit a 3D multivariate Gaussian per regime: mean vector `mu_k` (3x1), covariance matrix `Sigma_k` (3x3)

**Runtime**:

1. New DFM factors arrive: `f_t = [growth, inflation, liquidity]`
2. For each regime k, evaluate the Gaussian density: `L_k = N(f_t | mu_k, Sigma_k)`
3. Convert to probabilities via Bayes rule: `P(regime_k | f_t) = L_k / sum(L_1 ... L_6)`
4. Output: 6 probabilities summing to 1

**Why percentiles (not fixed thresholds)**: DFM factors are not on a fixed scale; they shift with retraining. Fixed thresholds could label 80% of history as "expansion." Percentiles guarantee balanced regime coverage and recalibrate automatically.

**Scope constraint**: These labels train DFM's own Gaussians ONLY. They are NOT used to train any other model's mapping. This is what eliminates the single-point-of-failure risk.

### B2: MS-DFM — Fingerprint Mapping

**Why this method**: MS-DFM outputs probabilities over 3 native regimes (its own latent states, not the ontology). These native states need to be mapped to the 6 ontology states. Fingerprint mapping lets MS-DFM define its own relationship to the ontology based on what macroeconomic conditions empirically look like during each native state.

**Training**:

1. Run MS-DFM on historical data to get native state assignments
2. For each native state j, compute an empirical fingerprint from the periods assigned to that state:
   - Mean growth momentum
   - Mean inflation level
   - Credit spread behavior
   - Yield curve shape
   - Policy stance indicators
3. Define target fingerprints for each of the 6 ontology states (what each regime "should look like" in these metrics)
4. Compute distances between each (native state, ontology state) pair
5. Apply softmax: `P(ontology_k | native_j) = exp(-d(j,k) / tau) / sum(exp(-d(j,k') / tau))`
6. Walk-forward calibrate temperature `tau`

**Runtime**:

1. MS-DFM outputs 3 native state probabilities: `p_native = [p_0, p_1, p_2]`
2. Apply mapping: `p_ontology_k = sum_j p_native_j * P(ontology_k | native_j)`
3. Output: 6 probabilities summing to 1

### B3: Growth-Inflation Quadrant — Rule-Based Mapping

**Why this method**: The quadrant model already operates on growth and inflation scores — the same axes the macro ontology is built on. No statistical mapping is needed; the relationship is deterministic and interpretable.

**Mapping rules** (using continuous scores, not hard quadrant boundaries):

| Quadrant | Ontology Distribution |
|----------|----------------------|
| GOLDILOCKS (high growth, low inflation) | 0.80 EXP_DIS + 0.15 EXP_INF + 0.05 SLO_DIS |
| REFLATION (high growth, high inflation) | 0.85 EXP_INF + 0.10 SLO_INF + 0.05 EXP_DIS |
| STAGFLATION (low growth, high inflation) | 0.55 SLO_INF + 0.30 REC_INF + 0.15 EXP_INF |
| DEFLATION (low growth, low inflation) | 0.50 SLO_DIS + 0.35 REC_DEF + 0.15 EXP_DIS |

For continuous scores near boundaries, blend adjacent quadrant distributions weighted by distance from threshold.

### B4: BSTS — Fingerprint Mapping (Crisis/Break Confirmer)

**Why this method**: BSTS detects structural breaks and policy shifts, not steady-state regimes. Its value is in flagging transitions and crises, not in classifying calm periods.

**Mapping behavior**:

- **No structural break detected**: Output near-uniform distribution `[0.17, 0.17, 0.17, 0.17, 0.16, 0.16]`. This effectively defers to other models — BSTS contributes no information during stable periods.
- **Structural break detected**: Fingerprint mapping based on break characteristics:
  - Break direction (growth collapsing vs. inflation spiking)
  - Break severity (magnitude of trend shift)
  - Co-occurrence with credit/policy events
  - Maps to stress-weighted distribution (e.g., severe downward break + credit widening → heavy weight on RECESSION_DEFLATIONARY)

### Future: XGBoost / RF — Consensus-Label Classification

When ML models are added, they are trained against **consensus labels** derived from the agreement of existing models — not from any single model's labels. This prevents diversity collapse.

**Consensus label generation**: At each training timestep, if 2+ of {DFM Gaussians, MS-DFM fingerprints, Quadrant rules} agree on the most-likely ontology state, that becomes a training label. Disagreement periods are excluded or down-weighted.

---

## Aggregation

### Weights

| Model | Weight | Rationale |
|-------|--------|-----------|
| MS-DFM | 0.40 | Primary regime classifier with explicit state identification |
| DFM Gaussians | 0.25 | Continuous factor backbone, smooth signal |
| Quadrant | 0.20 | Interpretable anchor, sanity check |
| BSTS | 0.10 | Structural break confirmation |
| ML (future) | 0.05 | Confidence adjustment only, hard-capped at 10% |

### Formula

For each ontology bucket o at time t:

```
p_raw(o,t) = sum_m  w_m * p_m(o,t)
```

where `p_m(o,t)` is model m's mapped probability for ontology state o.

### Persistence Smoothing

```
p_smooth(o,t) = lambda_t * p_raw(o,t) + (1 - lambda_t) * p_smooth(o,t-1)
```

- `lambda_t` adapts: lower during calm (more smoothing), higher during confirmed shocks (faster response)
- Base lambda for macro: 0.1 (much smoother than market — macro moves slowly)

### Hysteresis

Asymmetric entry/exit thresholds prevent thrashing:

| Transition | Enter if P > | Exit if P < |
|------------|--------------|-------------|
| Enter defensive | 0.55 | 0.40 |
| Enter risk-off | 0.60 | 0.45 |
| Enter aggressive | 0.70 | 0.50 |

### Minimum Duration

Macro regime must persist for at least 6 months before the system considers transitioning. During this dwell period, the current regime probability receives a bonus (transition penalty = 0.5) that decays as the dwell period expires.

---

## Output: MacroBelief

```cpp
struct MacroBelief {
    std::map<MacroRegimeL1, double> macro_probs;  // 6 probabilities summing to 1
    MacroRegimeL1 most_likely;
    double confidence;

    // Overlays (from dedicated detectors, not from main regime models)
    bool policy_restrictive;
    bool credit_tightening;
    bool inflation_sticky;
    bool structural_break_risk;

    // Provenance
    std::map<std::string, std::map<MacroRegimeL1, double>> model_contributions;
    std::chrono::system_clock::time_point timestamp;
    int regime_age_bars;
};
```

This MacroBelief is shared across all portfolio sleeves. It sets the **risk ceiling** (macro governor). Each sleeve's local market regime sets the **tactical aggressiveness** within that ceiling.

---

## Failure Degradation

| Failure | Impact | Remaining Signal |
|---------|--------|------------------|
| DFM breaks | Lose 25% weight | MS-DFM (0.40) + Quadrant (0.20) + BSTS (0.10) = 70% still operational |
| MS-DFM breaks | Lose 40% weight | DFM (0.25) + Quadrant (0.20) + BSTS (0.10) = 55% — reduced but viable |
| Both DFM + MS-DFM break | Lose 65% weight | Quadrant (0.20) + BSTS (0.10) = 30% — minimal signal, trigger low-confidence alert, tighten risk limits automatically |
| Quadrant breaks | Lose 20% weight | DFM + MS-DFM = 65% — barely noticed |
| BSTS breaks | Lose 10% weight | 90% intact — negligible |

No single model failure causes cascading errors in other models' mappings.

---

## Scalability

**Adding new models**: Implement `map_to_ontology()` using whichever method fits the output type. Register with aggregator. Rebalance weights. No existing models need retraining.

**L2 sub-states**: Define richer fingerprints for L2. Models that see the relevant dimensions map sharply (e.g., a credit model can distinguish PRODUCTIVITY_DRIVEN from CREDIT_FUELED). Models that cannot (like DFM with 3 factors) spread probability evenly across L2 sub-states within the correct L1 bucket — correct behavior expressing "I know the L1 state but not the L2 detail."

**Overlays**: Produced by dedicated detectors (policy stance tracker, credit conditions model), not by the main 4 regime models. Independent by construction.
