# Regime Detection

Per-sleeve market regime detection and macro regime detection, fused into
operator-facing `MarketBelief` (one per sleeve) and `MacroBelief` (one
shared) outputs.

## At a glance

- **Macro pipeline**: 4 models (DFM, MS-DFM, Growth-Inflation Quadrant,
  BSTS) → 6 macro regime states → `MacroBelief`.
- **Market pipeline**: 4 models per sleeve (HMM, MSAR, GARCH, GMM) → 5
  market regime states → `MarketBelief` per sleeve (equities, rates, FX,
  commodities).
- **Aggregation**: per-model 5/6-prob vectors → weighted blend → EWMA
  smoothing → hysteresis + minimum dwell → final belief.
- **Outputs include uncertainty**: `confidence`, `entropy`, `top_prob`,
  `regime_age_bars`, plus per-model contribution vectors for diagnostics.

## Design principles

The pipeline follows three rules that recur across both macro and market
sides. Knowing them up-front makes the rest of this doc easier to read.

**1. No model is a backbone.** Each contributing model maps its native
output independently into the same regime ontology. If any single model
fails, the others still produce a valid belief at reduced confidence.
There is no "the DFM said so, therefore..." anywhere — DFM probabilities
go in alongside MS-DFM, Quadrant, and BSTS, weighted by `w_dfm`. This
matters because individual models are wrong in known, asymmetric ways
(DFM is services-blind; HMM can't separate panic-crash from
liquidity-dysfunction; BSTS misclassifies during structural-break
windows). Aggregating limits the damage from any one being wrong.

**2. Use the right mapping method per output type.** Models that emit
*continuous factors* get **trained multivariate Gaussians per regime**
(this is what the macro DFM does — fit a Gaussian to the historical
growth/inflation factor distribution within each regime, then evaluate
each new bar's factor vector under all 6 Gaussians). Models that emit
*discrete latent states* get **fingerprint mapping** (compute a feature
vector per native state, z-score across states within the model,
soft-distance-map to ontology target fingerprints). Models that emit
*single scalar features* (GARCH σ_t, BSTS structural break flag) get
**rule-based binning or feature mapping**. Mixing methods — e.g.,
forcing HMM to use trained Gaussians, or DFM to use fingerprints — is
worse than letting each model use what suits its output type.

**3. Operator visibility is part of the contract.** Every belief carries
`confidence`, `entropy`, `top_prob`, `regime_age_bars`, and per-model
contribution vectors. The pipeline reports *how sure it is and which
models agree*, not just the final regime label. Downstream consumers
should gate decisions on these uncertainty fields, not just
`most_likely`. See "Diagnostic workflow" below.

## File layout

```
src/regime_detection/
  macro/
    bsts_regime_detector.cpp      Bayesian structural time series; R0-R3 cluster assignment
    dynamic_factor_model.cpp      DFM Kalman+EM on the 24-series macro panel
    ms_dfm.cpp                    Markov-Switching DFM; regime-conditioned dynamics
    macro_data_loader.cpp         Postgres macro_data ingest, NaN-aware
    macro_regime_pipeline.cpp     Aggregator: DFM + MS-DFM + Quadrant + BSTS → MacroBelief
  market/
    market_regime_pipeline.cpp    Aggregator: HMM + MSAR + GARCH + GMM → MarketBelief per sleeve
    market_data_loader.cpp        Postgres market_data ingest, multi-symbol panel alignment
    autoregressive.cpp            Hamilton-filter AR(1) used by MarketMSAR
    autoregressive_debug.cpp      Diagnostic AR variant (not shipped in production path)
  bsts_regime_detection_multiasset.cpp   Standalone BSTS runner (one-off analysis tool)

include/trade_ngin/regime_detection/
  macro/   matching public headers for the macro pipeline
  market/  matching public headers for the market pipeline (including header-only msar.hpp)

apps/regime_detection/
  macro_dfm_runner.cpp                Standalone DFM training + diagnostics
  macro_msdfm_runner.cpp              Standalone MS-DFM training + diagnostics
  macro_regime_pipeline_runner.cpp    Full macro pipeline run, prints last belief
  market_regime_pipeline_runner.cpp   Per-sleeve market pipeline run; with TIMELINE_CSV env var emits per-bar regime CSV
```

General-purpose statistics (`KalmanFilter`, `HMM`, `MarkovSwitching`,
`ExtendedKalmanFilter`, `GMM`) stay in `src/statistics/` — they are
consumed by regime detection but are not regime-specific.

## Macro pipeline

### Inputs
- 24-series macro panel from `macro_data` schema. Six column groups:
  growth (6), inflation (5), credit (3), yield_curve (3), policy (3),
  plus 4 "other" not in fingerprints.
- BSTS `structural_break` flag from external structural-break detector.

### The four models (each maps to the same 6 macro regime states)

| Model | Native output | Mapping method |
|---|---|---|
| **DFM** | 3 continuous factors over time | Trained multivariate Gaussians per regime, fit on growth/inflation percentile partitions |
| **MS-DFM** | 3-state regime-conditioned VAR posterior | Soft-prob-weighted fingerprint mapping (each native state's posterior-weighted feature vector is z-scored, then softmax-distance-mapped to ontology states) |
| **Quadrant** | (growth_score, inflation_score) ∈ ℝ² | Hardcoded quadrant table: GOLDILOCKS / REFLATION / DEFLATION / STAGFLATION → 6-regime blend, with boundary blending |
| **BSTS** | 4 cluster assignments (R0..R3) on PCA-reduced indicators | Greedy fingerprint assignment to nearest regime; `-1` (Unclassified) sentinel for clusters with no positive match |

### Aggregation

```
p_raw   = w_dfm × p_dfm + w_msdfm × p_msdfm + w_quad × p_quad + w_bsts × p_bsts
p_smooth = λ × p_raw + (1−λ) × p_smooth_{t-1}     (EWMA, with shock-adaptive λ)
dominant = apply_hysteresis(p_smooth)              (asymmetric enter/exit thresholds + min_dwell_bars)
```

Default weights: DFM 0.25, MS-DFM 0.40, Quadrant 0.20, BSTS 0.10.
Default smoothing: `base_lambda = 0.20`, `calm_lambda_scale = 1.0`,
`shock_lambda_scale = 3.0`, `shock_threshold = 0.10`.

The shock-adaptive λ is the part most worth understanding: when
`||p_raw - p_smooth_{t-1}||_∞ > shock_threshold`, λ jumps to
`base_lambda × shock_lambda_scale = 0.60` (3× the calm value), so the
pipeline reacts within ~2 bars to genuine regime shifts. In calm
periods λ stays at `base_lambda × calm_lambda_scale = 0.20`, which
filters daily noise. The original spec used λ ≈ 0.10 for monthly
macro data; raised to 0.20 here because the pipeline runs on daily
prints and 0.10 made transitions effectively impossible.

Hysteresis enforces asymmetric enter/exit thresholds — a regime needs
≥ `enter_*_thresh` posterior to become dominant, but the *current*
regime needs only ≥ `exit_*_thresh` to stay dominant. Combined with
`min_dwell_bars = 26` (~1 month of daily bars), this prevents
single-bar thrashing while still allowing meaningful regime changes
within ~2-3 weeks of a true shift.

### Macro regime ontology (6 states)

```
EXPANSION_DISINFLATION
EXPANSION_INFLATIONARY
SLOWDOWN_DISINFLATION
SLOWDOWN_INFLATIONARY
INDUSTRIAL_WEAKNESS_DEFLATIONARY
INDUSTRIAL_WEAKNESS_INFLATIONARY
```

`INDUSTRIAL_WEAKNESS_*` is an interim relabel of the spec's `RECESSION_*`.
The DFM is services-blind (factor 1 loads on `manufacturing_capacity_util`,
`industrial_production`, `unemployment_rate`), so calling this "recession"
overstated what the model identifies. See `docs/KNOWN_GAPS.md` Gap 9 for
the full rationale and the path back to `RECESSION_*` once services
indicators land.

### MacroBelief contract

```cpp
struct MacroBelief {
    std::map<MacroRegimeL1, double> macro_probs;    // 6 probs summing to 1
    MacroRegimeL1 most_likely;
    double confidence;                              // dominant prob - second-place prob
    bool   structural_break_risk;                   // BSTS-driven flag
    std::map<std::string, std::map<MacroRegimeL1, double>> model_contributions;
    std::chrono::system_clock::time_point timestamp;
    int    regime_age_bars;
    double entropy;                                 // normalised Shannon entropy ∈ [0,1]
    double top_prob;                                // posterior of most_likely state
};
```

`policy_restrictive`, `credit_tightening`, `inflation_sticky` overlays
were removed because no detectors populate them yet. See
`docs/ENHANCEMENTS.md` § "Macro Overlay Detectors" for the path to
re-introduce them.

## Market pipeline

### Inputs (per sleeve)
- Log-return series from `market_data` schema, multi-symbol panel
  aligned by date.
- Per-bar volume series (NaN where unavailable; downstream gates on
  `isfinite()`).
- Cross-asset pooled correlation z-score (`corr_spike`), computed once
  across all sleeves and broadcast to each.
- 60-bar trailing return (`ret60`) for slow-stress detection.

### The four models (each maps to the same 5 market regime states)

| Model | Native output | Mapping method |
|---|---|---|
| **HMM** (3 states) | filtered + smoothed posteriors over 3 latent states | Fingerprint mapping on 4D `[\|μ\|, σ, liquidity, ret60]` (drops to 2D when liquidity/ret60 unavailable) |
| **MSAR** (2 states) | filtered posteriors with AR(1) emission `N(μ_j + φ_j·r_{t-1}, σ²_j)` | Fingerprint mapping on 3D `[μ, σ, φ]` |
| **GARCH/EGARCH** | conditional vol σ_t + asymmetry flag | Bin σ_t into 4 quantile bands → ontology probs; EGARCH leverage flag adjusts STRESS share |
| **GMM** (5 clusters) | cluster posteriors on 5D feature space `[r, σ̂, dd_speed, volume_ratio, corr_spike]` | Cluster fingerprint at training time → softmax distance to ontology targets |

### Aggregation per sleeve

```
p_raw    = w_hmm × p_hmm + w_msar × p_msar + w_garch × p_garch + w_gmm × p_gmm
p_smooth = λ × p_raw + (1−λ) × p_smooth_{t-1}   (warmup: λ=1 for first N updates)
dominant = argmax(p_smooth)
```

Default weights: HMM 0.40, MSAR 0.30, GARCH 0.20, GMM 0.10.

### Market regime ontology (5 states)

```
TREND_LOWVOL       Strong directional, low vol
TREND_HIGHVOL      Moderate directional, high vol
MEANREV_CHOPPY     Weak/no trend, choppy
STRESS_PRICE       Drawdown, spiking vol, adequate liquidity
STRESS_LIQUIDITY   Extreme vol, collapsed liquidity
```

### MarketBelief contract

```cpp
struct MarketBelief {
    SleeveId sleeve_id;
    std::map<MarketRegimeL1, double> market_probs;  // 5 probs summing to 1
    MarketRegimeL1 most_likely;
    double confidence;
    std::map<std::string, std::map<MarketRegimeL1, double>> model_contributions;
    std::chrono::system_clock::time_point timestamp;
    int    regime_age_bars;
    double stability;                               // dwell progress ∈ [0,1]
};
```

### Live state checkpointing

`MarketRegimePipeline::get_live_state(sleeve)` and
`restore_live_state(sleeve, snapshot)` snapshot the per-sleeve EWMA
recurrence state (`prev_smoothed`, `last_belief`, `update_count`) so a
live process can checkpoint between bars and resume cleanly across
restarts. Trained mappings are not serialised through this path — those
come from a separate `train()` call.

The split between trained-mapping state and runtime EWMA state is
deliberate. Trained mappings (HMM emission means/covs, MSAR transition
matrix, GMM cluster means, fingerprint matrices) are large and only
change at training time. The runtime state is small (5 probs +
`MarketBelief` + counter) and changes every bar. A live process
re-trains weekly or monthly, then checkpoints runtime state every bar
— so a process restart only needs to reload the snapshot, not retrain.

## How fingerprint mapping works

Three of the four market models (HMM, MSAR, GMM) and two of the four
macro models (MS-DFM, BSTS) all use the same fingerprint-mapping
pattern. Worth understanding once.

**Step 1 — native fingerprints.** For each native state `j` of a
model, compute a feature vector. For HMM: `[|μ_j|, σ_j, liq_j,
ret60_j]` (the 4D fingerprint described in the market section above).
For MSAR: `[μ_j, σ_j, φ_j]`. For GMM: the cluster mean in the 5D GMM
feature space. The feature dimension `D` matches the dimension of the
*ontology targets* this model maps into.

**Step 2 — z-score across native states.** For each feature dimension
`d`, compute mean and std across the model's `J` native states. Replace
each native fingerprint with its z-scored version. This is what makes
the mapping adaptive across sleeves — equities' σ ~10× rates' σ in
absolute terms, but after z-scoring across the sleeve's own 3 native
states, both end up with comparable z values.

**Step 3 — softmax distance to ontology targets.** For each ontology
state `k` (TREND_LOWVOL, TREND_HIGHVOL, ...), compute squared
Euclidean distance from the standardised native fingerprint to the
ontology target vector `t_k`. Convert to probabilities via softmax with
temperature τ:

```
log p(j → k) = −||fp_j − t_k||² / τ
p(j → k)     = softmax over k
```

The result is a `J × K` mapping matrix `M[j][k]` = probability that
native state `j` corresponds to ontology state `k`. Lower τ makes
assignments sharper; default τ = 1.0 gives moderate smoothing.

**Step 4 — runtime mapping.** At each bar, the model produces a
`J`-vector of native posteriors `p_native`. The ontology probability
is `p_ontology = M^T · p_native`. This is what feeds into aggregation.

Targets are hardcoded per model. The market HMM's 2D targets (when
liquidity unavailable):

```
TREND_LOWVOL    : (0.0, -1.5)    any |μ|, low σ
TREND_HIGHVOL   : (0.5,  0.0)    moderate |μ|, mid σ
MEANREV_CHOPPY  : (-0.5, 0.0)    low |μ|, mid σ
STRESS_PRICE    : (0.5,  1.5)    moderate |μ|, high σ
STRESS_LIQUIDITY: (0.0,  2.5)    any |μ|, EXTREME σ
```

Stress is defined by σ; |μ| is secondary. Bull-market high-vol periods
land closer to TREND_HIGHVOL (mid σ) than to STRESS_PRICE (high σ),
even when |μ| is comparable. This was the central calibration insight
of the K-04 v2 retune (see commit history for the path that got here).

## Configuration reference

All knobs live in `MarketRegimePipelineConfig` /
`MacroRegimePipelineConfig`. The ones operators are most likely to
touch:

### Macro pipeline (`MacroRegimePipelineConfig`)

| Knob | Default | What it does |
|---|---|---|
| `w_dfm`, `w_msdfm`, `w_quadrant`, `w_bsts` | 0.25, 0.40, 0.20, 0.10 | Per-model aggregation weights (sum to 1.0) |
| `growth_upper_pctile`, `growth_lower_pctile` | 0.70, 0.20 | DFM Gaussian training partitions: top 30% = expansion, bottom 20% = weakness, middle 50% = slowdown |
| `inflation_pctile` | 0.50 | DFM Gaussian inflation split (median) |
| `growth_factor_idx`, `inflation_factor_idx` | 1, 2 | Which DFM factor is growth, which is inflation. **Verify by inspecting DFM loadings before changing.** |
| `growth_factor_sign`, `inflation_factor_sign` | -1.0, -1.0 | Sign flips. DFM factors are identified up to sign; if the dominant loading is negative, set to -1 so high values mean expansion / high inflation as the labeling expects. |
| `base_lambda` | 0.20 | EWMA smoothing in calm periods |
| `shock_lambda_scale` | 3.0 | Multiplier on λ during shocks (`base_lambda × shock_lambda_scale = 0.60`) |
| `shock_threshold` | 0.10 | Max-abs deviation of `p_raw` from `p_smooth_{t-1}` that triggers shock mode |
| `min_dwell_bars` | 26 | Bars a regime must hold before transition allowed |
| `enter_*_thresh`, `exit_*_thresh` | 0.20, 0.15 | Hysteresis bands per regime category |
| `bsts_always_contribute` | true | If false, BSTS contributes uniform when no break detected (wastes its weight); if true, BSTS always contributes its fingerprint mapping |

### Market pipeline (`MarketRegimePipelineConfig`)

Per-sleeve weights and thresholds live in `SleeveConfig` (one per
sleeve). Defaults are sleeve-specific:

| Sleeve | w_hmm | w_msar | w_garch | w_gmm | trend_lowvol_vol_upper | stress_vol_lower |
|---|---|---|---|---|---|---|
| equities | 0.40 | 0.30 | 0.20 | 0.10 | 0.12 | 0.35 |
| rates | 0.38 | 0.30 | 0.22 | 0.10 | 0.08 | 0.20 |
| fx | 0.40 | 0.30 | 0.20 | 0.10 | 0.06 | 0.15 |
| commodities | 0.40 | 0.30 | 0.20 | 0.10 | 0.18 | 0.40 |

Pipeline-wide knobs:

| Knob | Default | What it does |
|---|---|---|
| `hmm_n_states` | 3 | HMM latent state count per sleeve |
| `msar_n_states`, `msar_ar_lag` | 2, 1 | MSAR config (2 regimes, AR(1) emission) |
| `gmm_n_clusters`, `gmm_restarts`, `gmm_max_iterations` | 5, 10, 300 | GMM config |
| `fingerprint_tau` | 1.0 | Softmax temperature for fingerprint mapping (lower = sharper) |
| `lambda` | 0.30 | EWMA smoothing constant (single λ; no shock adaptation on market side) |
| `realized_vol_window`, `vol_of_vol_window`, `liquidity_window` | 20, 60, 20 | Feature lookback windows |
| `garch_vol_history` | 252 | Trailing bars for σ_t percentile binning |

## Diagnostic workflow

### TIMELINE_CSV — full per-bar regime trace

The market runner emits a per-bar regime CSV when `TIMELINE_CSV` is
set in the environment. Format:

```
date,sleeve,regime,confidence,p_TREND_LOWVOL,p_TREND_HIGHVOL,p_MEANREV_CHOPPY,p_STRESS_PRICE,p_STRESS_LIQUIDITY
2020-03-13,equities,STRESS_PRICE,0.2655,0.0413,0.1598,0.1484,0.4580,0.1925
...
```

This is the bit-identity gate used by maintenance work — every change
that should preserve the regime call must produce 0 `diff` lines vs
the post-Phase-3 baseline at
`apps/strategies/results/regime_fix_baselines/market_timeline_K05plus.csv`.
Use it for cross-version validation, not just regression testing — the
per-bar probabilities tell you *which* model contributions shifted
when something changes.

### `model_contributions` — who voted for what

Every `MarketBelief` and `MacroBelief` carries a
`model_contributions` map: model name → `{regime → prob}`. This is how
you debug "why did the pipeline call X?" Print it from the runner via
`print_belief()` or read it programmatically.

When confidence is low (< 0.20) and `entropy` is high (> 0.7), the
disagreement is between models, not within them. Look at
`model_contributions` to see which model is voting against the
consensus and why.

### Reference fixtures

`docs/KNOWN_GAPS.md` § "Reference fixture suite" lists known dates
where the pipeline should call specific regimes (e.g., 2020-03-13
equities = STRESS_PRICE; 2024-07-15 equities = TREND_LOWVOL). Run a
TIMELINE_CSV trace and grep these dates after any change. The
fixtures are the regression target — a change is "good" only if it
preserves the ✅ rows and moves at least one ❌ row toward ✅.

## Failure modes — what to watch for

### NaN propagation

The pipeline treats NaN as "no observation available," not "value is
zero." Loaders emit NaN when:
- `compute_log_returns` sees a non-positive price → return is NaN
- `align_panels` finds a date where one symbol has no bar → composite
  cell is NaN (not 0)
- `liquidity_proxy` has no volume series for the sleeve → NaN
- Cross-asset `corr_spike` for a single-symbol sleeve → NaN

Downstream gates these via `isfinite()` and skips the affected
contribution. **NaN never silently becomes zero.** If you see a regime
call that doesn't match expectations and feel like the pipeline
"defaulted" to something, check NaN handling in the input — the
contract is fail-loud-not-silent.

### MS-DFM lock-in

MS-DFM EM can settle into a degenerate solution where one regime
captures ~90% of bars and the others are barely populated. This used
to happen pre-fix because cross-state /std normalisation amplified
small differences into runaway separation. The fix dropped that
normalisation and switched MS-DFM fingerprints to soft-prob-weighted
aggregates instead of hard argmax. If you re-tune the macro DFM and
see MS-DFM dominance > 80% on any single regime, that's the lock-in
pattern returning — investigate before shipping.

### GMM determinism caveat

GMM EM is deterministic given `(X, K, seed)` — same inputs always
produce the same output. But changing the input panel by even one bar
(extending the training window) can shift the converged solution
non-trivially through restart-init perturbation. This is real and
documented; see `docs/ENHANCEMENTS.md` § "Config-gated GMM
determinism per restart" for the proposed config knob to make this
explicit.

### Date parser timezone

`MarketDataLoader::load_symbol` parses `start_date` / `end_date` via
`std::mktime()`, which interprets the broken-down time in the host's
local timezone. On a non-UTC host this shifts the bar boundary by the
TZ offset. The empirically validated baseline was generated with
`mktime()`, so changing it perturbs the panel. The proper fix lives
upstream in the runner — see `docs/ENHANCEMENTS.md` § "UTC date
parsing — upstream Timestamp passing" for the deferred refactor.

### Stress lag

Stress regimes fire 1-2 weeks late on news-driven events (e.g., SVB
2023-03-08 → first STRESS_PRICE on 2023-03-17). This is structural —
HMM smoothing + 60-bar ret60 window is slow by construction. See
`docs/KNOWN_GAPS.md` Gap 3 for the fixture and proposed fix paths
(fast-path GARCH-driven STRESS proxy; news-day event detection
layer).

## Running the binaries

All four runners auto-load DB connection from `config/defaults.json` if
no argv / `DATABASE_URL` is supplied (mirroring the `bt_portfolio` /
`live_portfolio` pattern). Explicit args still override.

```bash
# From repo root, after a build
./build/bin/Release/macro_regime_pipeline_runner       # uses config/defaults.json
./build/bin/Release/market_regime_pipeline_runner       # same

# With explicit args
./build/bin/Release/market_regime_pipeline_runner \
    "host=... port=5432 user=... password=... dbname=..." \
    2020-01-01 2025-12-31

# Per-bar regime CSV for cross-version validation
TIMELINE_CSV=/tmp/timeline.csv ./build/bin/Release/market_regime_pipeline_runner

# Standalone DFM and MS-DFM analysis runners
./build/bin/Release/macro_dfm_runner
./build/bin/Release/macro_msdfm_runner

# Standalone BSTS multi-asset runner
./build/bin/Release/bsts_regime_detector
```

## Testing

Tests live alongside the rest of the test suite under
`tests/statistics/` (topic-named: `test_dynamic_factor_model.cpp`,
`test_ms_dfm.cpp`, `test_bsts_regime_detector.cpp`,
`test_macro_regime_pipeline.cpp`, `test_market_regime_pipeline.cpp`,
`test_market_data_loader.cpp`, `test_macro_data_loader.cpp`,
`test_msar.cpp`, `test_gmm.cpp`).

```bash
cmake --build build -j 8 --target trade_ngin_tests
./build/bin/Release/trade_ngin_tests
```

The timeline-CSV diagnostic above is the bit-identity gate used by
maintenance work — every change that should preserve the regime call
must produce 0 diff lines vs the post-Phase-3 baseline at
`apps/strategies/results/regime_fix_baselines/market_timeline_K05plus.csv`.

## Where to read next

- **`docs/ENHANCEMENTS.md`** — the architectural enhancements that
  remain to finish the pipeline (MBFS, ML confirmer, cross-asset
  overlay, conflict resolver, macro overlay detectors).
- **`docs/KNOWN_GAPS.md`** — specific behavioral gaps in the current
  pipeline, with fixture dates, expected vs current output, and
  acceptance criteria for fixes.
