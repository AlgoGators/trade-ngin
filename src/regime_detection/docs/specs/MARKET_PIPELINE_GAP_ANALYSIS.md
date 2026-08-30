# Market Regime Pipeline — Gap Analysis

## Context

The macro pipeline got a thorough rethink: DFM maps via trained Gaussians, other models map independently via fingerprints/rules, no single point of failure, clean scalability path. Does the market pipeline need a similar upgrade, or is the current fingerprint-only design sufficient?

**Short answer**: The market pipeline has three real gaps that should be addressed. None require a full redesign, but one of them — the lack of a continuous feature space model — would benefit from the same "use the right mapping method per output type" principle that improved the macro side.

---

## Gap 1: No Continuous Feature Space Model (the big one)

### The Problem

The macro side has DFM: a model that extracts continuous factors (growth, inflation, liquidity) that naturally span the space the ontology is defined on. This enabled the Gaussian mapping — the best mapping for continuous outputs.

The market side has no equivalent. The models are:

| Model | Output Type | Dimensionality |
|-------|-------------|----------------|
| HMM | 3 discrete latent states | 3 probabilities |
| MSAR | 2 discrete latent states | 2 probabilities |
| GARCH | 1 continuous value (vol) | 1 dimension |
| ML | Classification probabilities | Variable |

None of these span the full "market behavior space." But the 5 market L1 states are actually defined along 3-4 continuous dimensions:

| Dimension | What It Captures | Which L1 States It Distinguishes |
|-----------|-----------------|----------------------------------|
| **Trend strength** | Autocorrelation, directional R-squared, momentum persistence | TREND_* vs MEANREV_CHOPPY |
| **Volatility level** | Realized vol, vol percentile, vol clustering | LOWVOL vs HIGHVOL vs STRESS |
| **Liquidity quality** | Bid-ask spreads, volume, depth, market impact | STRESS_PRICE vs STRESS_LIQUIDITY |
| **Correlation stress** | Cross-asset correlation, dispersion, contagion | Normal states vs stress overlays |

These are the same axes the fingerprint signatures are defined on. The difference is that fingerprints are static target vectors, while a continuous feature space model would estimate where we are in this space dynamically.

### The Fix: Market Behavior Feature Space (MBFS)

Add a model that explicitly constructs the market behavior feature space — analogous to what DFM does for macro, but from observable market metrics rather than a factor model:

```
Market data (returns, vol, spreads, volume, correlations)
     │
     ▼
Feature Extraction
     │
     ├── trend_strength: rolling autocorrelation + directional R²
     ├── vol_level: GARCH output percentile vs history
     ├── liquidity_quality: normalized spread + volume z-score + depth proxy
     └── correlation_stress: rolling cross-asset correlation z-score
     │
     ▼
f_t = [trend_strength, vol_level, liquidity_quality, correlation_stress]
     │
     ▼
5 Multivariate Gaussians (one per market L1 state)
     │
     ▼
5 probabilities (Bayes-normalized)
```

**Training**: Label historical periods using the feature thresholds that define each L1 state (the same thresholds already in the ontology table). Fit one multivariate Gaussian per L1 state in 4D feature space.

**Runtime**: Compute features, evaluate 5 Gaussians, Bayes-normalize.

**What this adds over fingerprints**: Fingerprints are static signatures compared via Euclidean distance. Gaussians capture the covariance structure — during STRESS_LIQUIDITY, vol and spreads are correlated differently than during STRESS_PRICE. A 4D Gaussian captures that joint structure; a distance metric does not.

**What this does NOT replace**: HMM, MSAR, and ML still run independently with their fingerprint mappings. MBFS is an additional model in the ensemble, not a backbone. It participates in aggregation with a weight (suggested: 0.25-0.30), and other models keep their weights.

**Revised market aggregation weights**:

| Model | Current Weight | With MBFS |
|-------|---------------|-----------|
| HMM | 0.40 | 0.30 |
| MSAR | 0.40 | 0.25 |
| MBFS | — | 0.25 |
| ML confirmer | 0.20 | 0.20 |

---

## Gap 2: STRESS_PRICE vs STRESS_LIQUIDITY Distinction

### The Problem

The ontology distinguishes two stress states:

- `STRESS_PRICE`: Drawdowns, gaps, negative drift. But spreads are still reasonable, volume is adequate. This is a selloff, not a crisis.
- `STRESS_LIQUIDITY`: Spread explosion, volume collapse, depth evaporation. This is a market breakdown.

The distinction matters enormously for execution. In STRESS_PRICE, you can still trade. In STRESS_LIQUIDITY, order slicing and participation constraints become critical.

But the current market models are poorly equipped to distinguish them:

- **HMM (3 states)**: Typically learns low-vol, high-vol, crash. It lumps both stress types into one "crash" state because it only sees returns, not microstructure.
- **MSAR (2 states)**: Momentum vs mean-reversion. Doesn't see liquidity at all.
- **GARCH**: Sees vol spiking in both cases. Cannot distinguish the cause.
- **ML confirmer**: Could distinguish them IF it has liquidity features, but the current feature list (`realized_vol, vol_of_vol, skew, tail_risk, drawdown, correlation_breakdown`) lacks explicit liquidity inputs.

### The Fix

Two changes:

**A) Add liquidity features to the ML confirmer**:

Add to its feature set:
- Bid-ask spread z-score (vs rolling average)
- Volume ratio (current vs 20-day average)
- Market depth proxy (if available)
- Intraday volatility vs overnight gap ratio (liquidity stress shows more in gaps)

This lets the ML confirmer flag STRESS_LIQUIDITY specifically, which is one of the main reasons to have ML in the ensemble.

**B) MBFS (from Gap 1) solves this directly**: The `liquidity_quality` dimension in the 4D feature space explicitly separates the two stress types. STRESS_PRICE has `[negative trend, high vol, normal liquidity, elevated correlation]`. STRESS_LIQUIDITY has `[variable trend, extreme vol, collapsed liquidity, breakdown correlation]`. The Gaussians for these two states will have clearly different centers in the liquidity dimension.

---

## Gap 3: Cross-Asset Information Blindness

### The Problem

Market regime models (HMM, MSAR) run per-sleeve on single-asset data. But several of the most powerful regime signals are cross-asset:

| Signal | What It Indicates | Which Sleeve Sees It |
|--------|------------------|---------------------|
| Equity-bond correlation flip | Risk regime change | Neither (it's between sleeves) |
| USD strength + EM stress | Funding stress | FX sees USD; others miss EM |
| Commodity supply shock + rates response | Inflation regime shift | Commodities and rates separately |
| VIX term structure inversion | Imminent stress | Equities only |
| Credit spread widening + equity vol spike | Contagion | Neither directly |

Each sleeve detects its own local symptoms but misses the cross-asset pattern. The overlays (CORRELATION_SPIKE, etc.) partially address this, but they are binary flags, not probabilistic regime inputs.

### The Fix: Cross-Asset Overlay Model

This is not a new regime model — it is an **overlay enrichment** that feeds into the conflict resolver between macro and market aggregation:

```
Per-sleeve market beliefs ──┐
                            ├──> Conflict Resolver ──> RegimeBelief
MacroBelief ────────────────┘
                            ▲
                            │
              Cross-Asset Overlay Model
                            │
         ┌──────────────────┼──────────────────┐
         │                  │                  │
  Equity-Bond         Credit-Equity      FX-Commodity
  Correlation         Contagion          Divergence
  Monitor             Detector           Tracker
```

**Mechanics**: Each cross-asset monitor outputs a stress score (0-1). When a score exceeds a threshold, it modifies the conflict resolver's behavior:

- **Equity-bond correlation flip** (score > 0.7): Boost STRESS probability across ALL sleeves by 0.15, regardless of what local models say. This captures contagion that local models miss.
- **Credit-equity contagion** (score > 0.6): Override market TREND states to reduce confidence by 0.2, increase STRESS confidence by 0.2.
- **FX-commodity divergence** (score > 0.5): Flag as macro regime transition early warning (feed into the Transition Intelligence layer).

These are NOT new L1 states. They modify existing belief distributions at the conflict resolution stage.

---

## Summary: What Needs to Change for Market

| Gap | Severity | Fix | Effort |
|-----|----------|-----|--------|
| No continuous feature space model | **High** — market models map 2-3 discrete states to 5 ontology states with limited coverage | Add Market Behavior Feature Space (MBFS) with Gaussian mapping | Medium (feature engineering + Gaussian fitting, same pattern as DFM macro) |
| STRESS_PRICE vs STRESS_LIQUIDITY indistinguishable | **High** — execution strategy depends on this distinction | Add liquidity features to ML confirmer + MBFS liquidity dimension | Low-Medium |
| Cross-asset blindness | **Medium** — overlays partially cover this, but as binary flags not probabilistic inputs | Add cross-asset overlay monitors feeding into conflict resolver | Medium |

### What Does NOT Need to Change

- The fingerprint mapping approach for HMM/MSAR/ML is fine — these models output discrete states, and fingerprints are the right tool for discrete-to-ontology mapping.
- The aggregation framework (weighted blend + persistence + hysteresis) is sound and doesn't need rework.
- The per-sleeve architecture (local market models with global macro governor) is correct.
- The ontology itself (5 L1 states + overlays + L2 sub-states) is well-designed.

---

## Revised Market Pipeline (with fixes)

```
Market Data (returns, vol, spreads, volume, correlations)
     │
     ├──────────────────────────────────────────────────────────┐
     │                                                          │
     ▼                                                          ▼
Feature Extraction                                   Model Inference
     │                                                    │
     ▼                                               ┌────┼────┐
MBFS: 4D features                                    │    │    │
[trend, vol, liq, corr]                             HMM  MSAR  ML
     │                                               │    │    │
     ▼                                               │    │    │ (with liquidity
5 Gaussians                                          │    │    │  features added)
     │                                               │    │    │
     ▼                                               ▼    ▼    ▼
5 probs (Bayes)                              Fingerprint Mappings
     │                                               │
     │                                               ▼
     │                                          5 probs each
     │                                               │
     └───────────────┬───────────────────────────────┘
                     ▼
            Weighted Aggregation
            (MBFS 0.25, HMM 0.30, MSAR 0.25, ML 0.20)
                     │
                     ▼
            Persistence + Hysteresis
                     │
                     ▼
          Cross-Asset Overlay Adjustment
                     │
                     ▼
            MarketBelief (per sleeve)
```

This parallels the macro pipeline's structure: one continuous-space model (MBFS / DFM) using Gaussians, plus independent discrete models using fingerprints, all aggregated with no single point of failure.
