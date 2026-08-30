# Synthesized Market Regime Pipeline

## The Idea in a Few Sentences

Per sleeve, four models (MBFS, HMM, MSAR, ML confirmer) each independently map their native outputs into probabilities over the same five market regime states. MBFS uses trained multivariate Gaussians on a 4D feature space (trend, vol, liquidity, correlation) — the same pattern DFM uses for macro. HMM and MSAR use fingerprint-based mappings because they output discrete latent states. The ML confirmer uses fingerprint mapping with an expanded feature set that includes liquidity metrics. Cross-asset overlay monitors adjust beliefs at the conflict resolution stage. No model is a backbone or single point of failure.

---

## Market Ontology (5 L1 States, per sleeve)

All models produce probabilities over these five states. The labels are shared across sleeves for portfolio coordination, but the statistical definition of each state is sleeve-specific.

| State | Trend | Volatility | Liquidity | Correlation | Typical Duration |
|-------|-------|------------|-----------|-------------|------------------|
| `TREND_LOWVOL` | Strong directional | Low (< sleeve-specific threshold) | Normal | Normal | Weeks to months |
| `TREND_HIGHVOL` | Moderate directional | High (> 75th pctl) | Thinning | Elevated | Days to weeks |
| `MEANREV_CHOPPY` | Weak / none | Moderate | Normal | Normal | Weeks to months |
| `STRESS_PRICE` | Negative | Spiking | Adequate | Increasing | Days to weeks |
| `STRESS_LIQUIDITY` | Variable | Extreme (> 95th pctl) | Collapsed | Breakdown | Days |

### Sleeve-Specific Thresholds

| Sleeve | TREND_LOWVOL vol < | STRESS vol > | Liquidity collapse = |
|--------|-------------------|-------------|---------------------|
| Equities | 12% | 35% | Spread > 3x normal + volume < 40% average |
| Rates | 8% | 20% | Depth < 50% normal around macro releases |
| FX | 6% | 15% | Spread > 4x normal + gap risk elevated |
| Commodities | 18% | 40% | Volume < 30% average + roll spread > 2x |

### Market Overlays (modulators, not L1 states)

| Overlay | Scope | Trigger |
|---------|-------|---------|
| `EVENT_SHOCK_TRANSIENT` | Per-sleeve or global | Earnings, OPEC, NFP, intervention |
| `CORRELATION_SPIKE` | Cross-asset | Cross-asset correlation > 2-sigma |
| `VOL_OF_VOL_HIGH` | Per-sleeve | Vol-of-vol > 90th percentile |
| `DISPERSION_HIGH` | Per-sleeve | Sector/stock dispersion elevated |
| `GAP_RISK_HIGH` | Per-sleeve | Overnight/weekend gap risk elevated |

### Market L2 Sub-States (optional detail)

| L1 State | L2 Sub-States |
|----------|---------------|
| TREND_LOWVOL | TREND_PERSISTENT, CARRY_FRIENDLY |
| TREND_HIGHVOL | BREAKOUT_VOLATILE, TREND_FRAGILE |
| MEANREV_CHOPPY | TIGHT_RANGE, WIDE_RANGE |
| STRESS_PRICE | DRAWDOWN_FAST, DRAWDOWN_GRINDING |
| STRESS_LIQUIDITY | FUNDING_STRESS, LIQUIDITY_GAP |

---

## Pipeline Architecture (per sleeve)

```
Market Data (returns, vol, spreads, volume, correlations)
     │
     ├────────────────────────────────────────────────────────────┐
     │                                                            │
     ▼                                                            ▼
Feature Extraction                                     Model Inference
     │                                                      │
     ▼                                                 ┌────┼────┐
MBFS: 4D features                                      │    │    │
[trend, vol, liq, corr]                               HMM  MSAR  ML
     │                                                 │    │    │
     ▼                                                 │    │    │
5 Trained Gaussians                                    │    │    │
     │                                                 ▼    ▼    ▼
     ▼                                          Fingerprint Mappings
5 probs (Bayes-normalized)                             │
     │                                                 ▼
     │                                            5 probs each
     │                                                 │
     └────────────────┬────────────────────────────────┘
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
             MarketBelief (this sleeve)
```

---

## Model-by-Model Specification

### M1: Market Behavior Feature Space (MBFS) — Trained Multivariate Gaussians

**Why this model exists**: HMM outputs 3 discrete states that must cover 5 ontology states. MSAR outputs 2 discrete states. Neither can cleanly distinguish all five L1 states, particularly the two STRESS sub-types. MBFS fills this gap by operating in a continuous 4D space that spans the same axes the ontology is defined on — exactly as DFM does for macro.

**Feature extraction** (computed from raw market data, per sleeve):

| Feature | Computation | What It Captures |
|---------|------------|------------------|
| `trend_strength` | Rolling autocorrelation (20-bar) + directional R-squared (rolling OLS of cumulative returns on time) | Trending vs mean-reverting behavior |
| `vol_level` | GARCH conditional volatility, expressed as percentile vs trailing 252-bar history | Current volatility regime |
| `liquidity_quality` | Composite: normalized bid-ask spread z-score + volume ratio (current/20-bar avg) + depth proxy if available | Market microstructure health |
| `correlation_stress` | Rolling cross-asset correlation z-score (vs 1-year history) | Contagion / diversification breakdown |

The output is a 4D vector: `f_t = [trend_strength, vol_level, liquidity_quality, correlation_stress]`

**Training**:

1. Compute 4D feature time series from historical data
2. Label each timestep using feature thresholds that define each L1 state:
   - `TREND_LOWVOL`: trend_strength > 60th pctl, vol_level < 30th pctl, liquidity_quality > 50th pctl
   - `TREND_HIGHVOL`: trend_strength > 40th pctl, vol_level > 70th pctl, liquidity_quality > 30th pctl
   - `MEANREV_CHOPPY`: trend_strength < 40th pctl, vol_level between 20th-70th pctl, liquidity_quality > 40th pctl
   - `STRESS_PRICE`: trend_strength negative, vol_level > 80th pctl, liquidity_quality > 25th pctl, correlation_stress > 70th pctl
   - `STRESS_LIQUIDITY`: vol_level > 90th pctl, liquidity_quality < 20th pctl, correlation_stress > 80th pctl
   - Percentile thresholds are sleeve-specific and configurable
3. For each of the 5 L1 states, collect all `f_t` values from that regime
4. Fit a 4D multivariate Gaussian per state: mean vector `mu_k` (4x1), covariance matrix `Sigma_k` (4x4)

**Why multivariate (not 4 independent thresholds)**: During STRESS_LIQUIDITY, vol and liquidity degrade together in a correlated way that's different from STRESS_PRICE. A 4D Gaussian captures that joint structure. Independent thresholds miss the covariance.

**Runtime**:

1. Compute current features: `f_t = [trend_strength, vol_level, liquidity_quality, correlation_stress]`
2. For each L1 state k, evaluate Gaussian density: `L_k = N(f_t | mu_k, Sigma_k)`
3. Bayes-normalize: `P(state_k | f_t) = L_k / sum(L_1 ... L_5)`
4. Output: 5 probabilities summing to 1

**Retraining**: Percentile thresholds recalibrate automatically on retrain (same property as DFM percentiles for macro). Gaussians are re-fit from the relabeled data.

**Sleeve-specific**: Each sleeve has its own MBFS instance with its own feature computation, thresholds, and Gaussians. TREND_LOWVOL in equities (vol < 12%) has a different Gaussian from TREND_LOWVOL in commodities (vol < 18%).

### M2: HMM — Fingerprint Mapping

**Why this method**: HMM outputs probabilities over 3 latent states (typically: low-vol, high-vol, extreme). These are discrete — fingerprint mapping is the natural tool.

**Training**:

1. Fit HMM on sleeve-specific return series (per-sleeve or per-cluster scope)
2. For each HMM state j (j = 0, 1, 2), compute empirical fingerprint from periods assigned to that state:
   - Mean drift (direction and magnitude)
   - Mean realized volatility
   - Mean autocorrelation sign and magnitude
   - Mean drawdown velocity
   - Mean spread/volume behavior
3. Define target fingerprints for each of the 5 L1 states (the ontology signatures, sleeve-specific)
4. Compute distance between each (HMM state, L1 state) pair
5. Softmax: `P(L1_k | HMM_j) = exp(-d(j,k) / tau) / sum(exp(-d(j,k') / tau))`
6. Walk-forward calibrate temperature `tau`

**Runtime**:

1. HMM outputs 3 state probabilities: `p_hmm = [p_0, p_1, p_2]`
2. Apply mapping: `p_ontology_k = sum_j p_hmm_j * P(L1_k | HMM_j)`
3. Output: 5 probabilities summing to 1

**Limitation addressed by ensemble**: HMM's 3 states cannot cleanly separate all 5 L1 states. Typically HMM state 0 maps mostly to TREND_LOWVOL + MEANREV_CHOPPY (overlapping), and HMM state 2 maps to both STRESS types (lumped). This is fine — MBFS handles the distinctions HMM cannot make, and the ensemble compensates.

### M3: MSAR — Fingerprint Mapping

**Why this method**: MSAR outputs probabilities over 2 latent states (momentum vs mean-reversion). Its AR coefficients carry trend/mean-reversion information that HMM doesn't have.

**Training**: Same fingerprint procedure as HMM, but with 2 native states. Key fingerprint dimensions:

- AR coefficient sign (positive = trending, negative = mean-reverting)
- AR coefficient magnitude (persistence strength)
- Regime-conditional volatility
- Regime-conditional drift

**Mapping**: MSAR's 2 states map to 5 L1 states. Typically:
- MSAR state 0 (positive AR, trending): spreads probability across TREND_LOWVOL, TREND_HIGHVOL, STRESS_PRICE (depending on vol/drift characteristics)
- MSAR state 1 (negative AR, mean-reverting): maps to MEANREV_CHOPPY primarily, some to STRESS states during high-vol mean-reversion

**Value in ensemble**: MSAR provides the strongest signal for TREND vs MEANREV distinction — the AR structure directly answers "is this market trending or mean-reverting?" This complements HMM (which sees vol structure better) and MBFS (which sees the full space but from features, not latent dynamics).

### M4: ML Confirmer — Fingerprint Mapping (with expanded features)

**Why this method**: ML models (RF/GBM) are nonlinear classifiers that can detect complex threshold interactions. Their primary role is stress/crash confirmation.

**Expanded feature set** (addresses STRESS_PRICE vs STRESS_LIQUIDITY gap):

| Feature Category | Features | Why Added |
|-----------------|----------|-----------|
| Returns | realized_vol, vol_of_vol, skew, tail_risk, drawdown | Original — captures vol structure |
| Correlation | correlation_breakdown, dispersion | Original — captures contagion |
| **Liquidity** (new) | bid_ask_spread_zscore, volume_ratio_20d, depth_proxy, intraday_vs_gap_vol_ratio | **Enables STRESS_PRICE vs STRESS_LIQUIDITY distinction** |
| **Microstructure** (new) | roll_spread_zscore (futures), trade_count_ratio | **Captures execution environment degradation** |

**Mapping**: Same fingerprint procedure as HMM/MSAR. ML outputs predicted regime probabilities; fingerprints map them to ontology states.

**Weight cap**: Hard-capped at 20% (can be reduced to 15% per sleeve). ML confirms stress but never overrides statistical models.

**Training labels**: When training the ML classifier, use consensus labels from agreement of MBFS + HMM + MSAR (same approach as macro ML training — no single-model ground truth).

### Future: K-Means / GMM — Fingerprint Mapping

When added, K-Means/GMM serves as exploratory validation:
- Cluster historical feature vectors
- Map cluster centers to ontology via fingerprint distances
- Low weight (0.05-0.10), confirmer role
- Primary value: initializing HMM states and validating that ontology states are empirically separable

---

## Aggregation

### Weights (per sleeve, configurable)

| Model | Default Weight | Rationale |
|-------|---------------|-----------|
| MBFS | 0.25 | Continuous feature space, covers all 5 states, best at STRESS sub-type distinction |
| HMM | 0.30 | Core latent state inference, strong vol regime detection |
| MSAR | 0.25 | Dynamics-aware, strongest trend vs mean-reversion signal |
| ML confirmer | 0.20 | Nonlinear stress confirmation, hard-capped at 25% max |

### Sleeve-Specific Weight Adjustments

| Sleeve | Adjustment | Rationale |
|--------|-----------|-----------|
| Equities | Standard weights | Balanced model applicability |
| Rates | MBFS 0.30, HMM 0.25, MSAR 0.25, ML 0.20 | MBFS liquidity dimension is critical for rates (thinning around macro releases) |
| FX | MBFS 0.25, HMM 0.30, MSAR 0.30, ML 0.15 | MSAR trend detection is strong for carry regimes |
| Commodities | MBFS 0.30, HMM 0.25, MSAR 0.25, ML 0.20 | MBFS captures supply-shock / liquidity dynamics better |

### Formula

For each L1 state o at time t:

```
p_raw(o,t) = sum_m  w_m * p_m(o,t)
```

### Persistence Smoothing

```
p_smooth(o,t) = lambda_t * p_raw(o,t) + (1 - lambda_t) * p_smooth(o,t-1)
```

- Base lambda for market: 0.3 (faster than macro — market regimes move faster)
- Adaptive: lambda increases to 0.6-0.8 during vol spikes (faster response to stress)
- Adaptive: lambda decreases to 0.15 during extended calm (more smoothing)

### Hysteresis

Asymmetric entry/exit thresholds prevent thrashing:

| Transition | Enter if P > | Exit if P < |
|------------|--------------|-------------|
| Enter STRESS (either type) | 0.55 | 0.35 |
| Enter TREND_LOWVOL | 0.60 | 0.40 |
| Enter MEANREV_CHOPPY | 0.50 | 0.35 |

### Minimum Duration

Market regime must persist for at least 20 bars before transitioning. During dwell period, current regime receives transition penalty bonus (0.3) that decays as dwell expires.

Exception: transition TO any STRESS state bypasses minimum duration (stress can't wait).

---

## Cross-Asset Overlay Model

### Why It Exists

Each sleeve's models run on local data only. Cross-asset signals (equity-bond correlation flip, credit contagion, FX funding stress) are invisible to local models but are among the strongest regime change indicators.

### Architecture

The cross-asset overlay model is NOT a regime model. It is a set of monitors that adjust belief distributions at the conflict resolution stage — after per-sleeve aggregation, before the final MarketBelief is produced.

```
Sleeve A MarketBelief (raw) ──┐
Sleeve B MarketBelief (raw) ──┤
Sleeve C MarketBelief (raw) ──┼──> Conflict Resolver ──> Final MarketBelief (per sleeve)
Sleeve D MarketBelief (raw) ──┤            ▲
MacroBelief ──────────────────┘            │
                                Cross-Asset Overlays
                                           │
                              ┌────────────┼────────────┐
                              │            │            │
                       EquityBond    CreditEquity   FXCommodity
                       CorrMonitor   Contagion      Divergence
```

### Monitors

**Equity-Bond Correlation Monitor**:
- Computes rolling equity-bond return correlation (60-bar window)
- Normal: negative correlation (stocks down, bonds up)
- Stress signal: correlation flips positive AND vol elevated
- Output: stress_score (0 to 1)
- Action when score > 0.7: boost STRESS probability across ALL sleeves by +0.15

**Credit-Equity Contagion Detector**:
- Tracks credit spread change z-score jointly with equity vol z-score
- Normal: credit spreads stable
- Stress signal: credit spreads widening > 2-sigma AND equity vol rising > 1.5-sigma simultaneously
- Output: contagion_score (0 to 1)
- Action when score > 0.6: reduce TREND confidence by -0.20, increase STRESS confidence by +0.20 across all sleeves

**FX-Commodity Divergence Tracker**:
- Monitors USD index vs commodity basket correlation
- Normal: inverse relationship (USD up, commodities down)
- Stress signal: both rising or both falling simultaneously (regime break)
- Output: divergence_score (0 to 1)
- Action when score > 0.5: flag as macro transition early warning (feeds into Transition Intelligence layer)

### Implementation Notes

- Overlay adjustments are additive to the aggregated probabilities, then renormalized
- Overlays can only INCREASE stress probability or DECREASE trend confidence — they cannot reduce stress signals
- Each overlay is independently computed and can trigger independently
- Overlay activations are logged with full provenance for auditability

---

## Output: MarketBelief (per sleeve)

```cpp
struct MarketBelief {
    std::string sleeve_id;  // "equities", "fx", "rates", "commodities"

    std::map<MarketRegimeL1, double> market_probs;  // 5 probabilities summing to 1
    MarketRegimeL1 most_likely;
    double confidence;

    // Active overlays
    std::set<MarketOverlay> active_overlays;

    // Cross-asset overlay scores
    double equity_bond_corr_score;
    double credit_equity_contagion_score;
    double fx_commodity_divergence_score;

    // Provenance
    std::map<std::string, std::map<MarketRegimeL1, double>> model_contributions;
    std::chrono::system_clock::time_point timestamp;
    int regime_age_bars;
};
```

---

## How MarketBelief Connects to MacroBelief

```
                MacroBelief (global)
                     │
                     │  Risk ceiling (governor)
                     │
        ┌────────────┼────────────┬────────────┐
        ▼            ▼            ▼            ▼
   FX Sleeve    Rates Sleeve  Commod Sleeve  Equity Sleeve
   MarketBelief MarketBelief  MarketBelief   MarketBelief
        │            │            │            │
        │  Gas pedal │  Gas pedal │  Gas pedal │  Gas pedal
        ▼            ▼            ▼            ▼
        ┌────────────┴────────────┴────────────┘
        │
        ▼
   Conflict Resolver
   (macro caps market; stress overrides opportunity)
        │
        ▼
   RegimeBelief (unified)
        │
        ▼
   Policy Layer (L7) → Allocation Engine (L8) → Strategies (L9)
```

### Hierarchical Control

```
effective_risk = min(
    macro_risk_ceiling,                    // Governor: from MacroBelief
    sleeve_base_risk * sleeve_multiplier   // Gas pedal: from MarketBelief
)
```

Macro cannot be overridden by market. If MacroBelief says RECESSION with high confidence, a sleeve seeing TREND_LOWVOL locally still operates under the recession risk ceiling.

---

## Failure Degradation

| Failure | Impact | Remaining Signal |
|---------|--------|------------------|
| MBFS breaks | Lose 25% weight | HMM (0.30) + MSAR (0.25) + ML (0.20) = 75%. Lose STRESS sub-type distinction — both stress types merge (acceptable short-term) |
| HMM breaks | Lose 30% weight | MBFS (0.25) + MSAR (0.25) + ML (0.20) = 70%. Lose latent state dynamics — still viable via MBFS feature space |
| MSAR breaks | Lose 25% weight | MBFS (0.25) + HMM (0.30) + ML (0.20) = 75%. Lose AR-based trend/mean-rev detection — HMM partially compensates |
| ML breaks | Lose 20% weight | MBFS + HMM + MSAR = 80%. Lose nonlinear stress confirmation — minimal impact |
| Cross-asset overlays break | No weight loss | Local models still work. Miss cross-asset contagion signals — risk of delayed stress detection |
| All models break (catastrophic) | Full signal loss | Fall back to MacroBelief only (macro governor still sets conservative risk ceiling). Trigger immediate alert. |

No single model failure causes cascading errors in other models' mappings.

---

## Scalability

**Adding models**: Implement `map_to_ontology()` with the appropriate method for the output type. Register with per-sleeve aggregator. Rebalance weights. No existing models need retraining.

**L2 sub-states**: Define richer fingerprints for L2. MBFS can potentially be extended to 5-6D to distinguish L2 states (e.g., add a "range width" feature to separate TIGHT_RANGE from WIDE_RANGE under MEANREV_CHOPPY). Models that can't see L2 dimensions spread probability across sub-states within the correct L1 bucket.

**New sleeves**: Instantiate new MBFS + HMM + MSAR + ML per sleeve. Configure sleeve-specific thresholds and Gaussian parameters. Share the same ontology and aggregation framework.

**New overlays**: Add new cross-asset monitors. They plug into the conflict resolver with their own threshold and adjustment rules. Independent by construction.

---

## What Needs to Be Built (Model Inventory)

### New Models (do not exist yet)

| Model | What It Is | Effort |
|-------|-----------|--------|
| **MBFS** | Feature extraction (4D) + 5 multivariate Gaussians per sleeve | Medium — feature engineering is the main work; Gaussian fitting is straightforward |
| **Cross-asset equity-bond correlation monitor** | Rolling correlation tracker + stress scoring | Low — simple statistics |
| **Cross-asset credit-equity contagion detector** | Joint z-score tracker | Low — simple statistics |
| **Cross-asset FX-commodity divergence tracker** | Correlation regime monitor | Low — simple statistics |

### Existing Models (need modifications)

| Model | Modification | Effort |
|-------|-------------|--------|
| **ML confirmer** | Add liquidity features (spread z-score, volume ratio, depth proxy, gap vol ratio) to feature set | Low — feature addition only |
| **HMM** | Add fingerprint mapping layer (currently may output raw states without ontology mapping) | Low-Medium — compute empirical fingerprints, calibrate temperature |
| **MSAR** | Add fingerprint mapping layer (same as HMM) | Low-Medium |

### No Changes Needed

| Component | Why |
|-----------|-----|
| Ontology (5 L1 + overlays + L2) | Well-designed, no gaps |
| Aggregation framework | Weighted blend + persistence + hysteresis — sound and reusable |
| Per-sleeve architecture | Correct separation of local market behavior |
| Conflict resolver | Macro-caps-market hierarchy is right |
| Policy layer (L7) and downstream | Consumes RegimeBelief regardless of how it was produced |
