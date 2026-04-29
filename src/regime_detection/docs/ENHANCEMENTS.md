# Regime Detection — Enhancements Roadmap

For an engineer planning the next phase of work. This doc lists the
architectural pieces that **remain to be built** to finish the regime
detection pipeline. Each entry has: what to build, why it matters, what
behavioral gaps it closes (cross-referenced to `KNOWN_GAPS.md`),
and current blockers.

This is a forward-looking roadmap. For the inventory of what's already
broken in the current pipeline (with fixture dates and acceptance
criteria), see `KNOWN_GAPS.md`. For the canonical design specs, see
`deliverables/regime/` (path is in the parent project worktree, not
this branch yet).

---

## Big-block enhancements (priority order)

### 1. MBFS — Market Behavior Feature Space

**What.** A continuous 4D feature space over `[trend_strength, vol,
liquidity_quality, correlation_stress]` mapped via *trained
multivariate Gaussians per regime* — the same pattern DFM uses for the
macro pipeline. The MBFS Gaussians sit alongside HMM and MSAR in the
market aggregation; they are not a backbone or single point of failure.

**Why.** The current market pipeline relies on HMM-on-returns to
discover stress states. HMM cannot separate two phenomenologically
distinct stress types that both have high σ:
- **Panic crash** (volume spikes, prices fall fast)
- **Liquidity dysfunction** (volume *collapses*, market makers can't
  absorb flow)

K-05's per-state liquidity mean averages across both phases of a single
HMM state, so neither separates cleanly. MBFS adds an explicit
`liquidity_quality` dimension from raw spread / depth / volume signals,
so the two stress types occupy different points in the feature space and
get distinct Gaussian assignments.

**Closes.** `KNOWN_GAPS.md` Gap 2 (liquidity dysfunction), Gap 6 (funding
stress via `correlation_stress` dim), Gap 10 (commodities
TREND_LOWVOL never fires).

**Spec.** `deliverables/regime/SYNTHESIZED_MARKET_PIPELINE.md`,
`deliverables/regime/MARKET_PIPELINE_GAP_ANALYSIS.md` § "MBFS".

**Status.** Designed; not implemented.

**Blockers.**
- Spread / depth / volume data ingest. Current `market_data_loader`
  only emits OHLCV; MBFS needs at minimum order-book depth proxy and
  intraday volume profile.
- Training-data labeling decision: which historical periods are
  ground-truth STRESS_LIQUIDITY (Treasury Mar 2020 is one; need a few
  more for the Gaussian fit).

---

### 2. ML Confirmer (spec section A6)

**What.** A learned classifier (gradient boosting or shallow neural
net, framework TBD) on the consensus features from the four
existing models (A1 HMM, A2 MSAR, A3 GARCH, A4 GMM) producing a
confidence-weighted "is the rule-based regime call right?" signal. Adds
as a fifth contributor to the market aggregation with weight `w_ml`.

**Why.** `w_ml` is currently 0 — the ML slot is unimplemented. The
rule-based aggregation has well-known residual ambiguity in the
MEANREV_CHOPPY ↔ TREND_LOWVOL boundary (the two attractors are close
in z-space, especially during low-vol regimes with mild persistence). A
confirmer that's trained on labeled historical periods can break this
ambiguity without manual threshold tuning.

**Closes.** Residual MEANREV/TREND ambiguity; provides a learned
confidence signal that can be A/B'd against the rule-based call before
defaulting it on.

**Spec.** `deliverables/regime/regime_detection_architecture.md` §
"Layer 2 — ML Confirmer".

**Status.** Not implemented. No design beyond "fit a classifier on the
consensus features".

**Blockers.**
- Training-data labeling. Manual per-bar labels for ~5 years of 4
  sleeves is expensive; bootstrapping from rule-based labels reintroduces
  the rule-based bias.
- Framework decision (XGBoost C++ binding vs ONNX vs in-house).
- Versioning + rollback path for the trained model artifact.

---

### 3. Cross-Asset Overlay Monitor

**What.** A separate process (or pipeline component) that watches for
divergences between sleeves and emits a high-priority overlay belief.
Three concrete monitors:
- **Equity-bond correlation flip** — when 60-day rolling correlation
  flips from < −0.3 (normal) to > +0.3 (stress), boost STRESS
  probability across all sleeves
- **Credit-equity contagion** — when credit spreads widen > 2σ AND
  equity vol is normal, flag funding-stress overlay
- **FX-commodity divergence** — when DXY moves > 2σ in 5 days and oil
  moves opposite, override individual FX sleeve regime call

**Why.** K-08 already pools cross-asset `corr_spike` and feeds it to
the GMM. But correlation breakdown is treated as one feature among
five, not as a first-class regime signal. The 2022 equity-bond
correlation flip from negative to positive was a textbook regime change
that the current pipeline detects only weakly (the GMM cluster shifts
slightly, but the per-sleeve regime calls don't).

**Closes.** `KNOWN_GAPS.md` Gap 5 (correlation breakdown), Gap 4 (FX
stress detection via DXY trigger).

**Spec.** `deliverables/regime/MARKET_PIPELINE_GAP_ANALYSIS.md` Gap 3.

**Status.** Not implemented. K-08 pooled corr exists but is a feature,
not an overlay.

**Blockers.**
- Decision on overlay-vs-state: should correlation regime be a separate
  ontology state, an overlay flag on existing states, or a multiplier
  on STRESS probability? Spec leans toward overlay; needs sign-off.

---

### 4. Conflict Resolver (MacroBelief × MarketBelief)

**What.** The reconciler between the macro pipeline's single shared
`MacroBelief` and the four per-sleeve `MarketBelief` outputs. Today
they run in parallel with no formal coupling. The resolver should:
- Cap per-sleeve risk multipliers when macro is in
  `INDUSTRIAL_WEAKNESS_*` or future `RECESSION_*`
- Override sleeve TREND_LOWVOL claims during macro CRISIS
- Resolve disagreement between sleeves (equities TREND_LOWVOL vs
  commodities STRESS_PRICE) into a single portfolio-level signal

**Why.** Currently a sleeve calling TREND_LOWVOL during a macro
slowdown gets full risk allocation — the macro signal doesn't propagate.
This is the most operationally impactful gap because it's the difference
between "regime detection works" and "regime-aware portfolio sizing
works".

**Closes.** `KNOWN_GAPS.md` Gap 7 (Macro CRISIS does not override
sleeve TREND_LOWVOL).

**Spec.** `deliverables/regime/regime_aware_portfolio_engine.md` §
"Conflict Resolver".

**Status.** Spec exists at the design level; no implementation.

**Blockers.**
- Theory call: does macro override act as a hard cap on sleeve risk
  multipliers, or as a probability blend? Different downstream
  implications for the portfolio sizing layer.
- Test fixture creation: synthetic input where macro and market
  disagree, with expected resolved output.

---

### 5. Macro Overlay Detectors

**What.** Re-introduce three boolean overlays on `MacroBelief` that
were removed when no detectors existed to populate them:
- **`policy_restrictive`** — true when monetary policy is in
  restrictive territory (Fed funds > neutral, real rates positive,
  yield curve indicators)
- **`credit_tightening`** — true when credit spreads are widening
  beyond historical baseline
- **`inflation_sticky`** — true when realized inflation prints exceed
  expectations for N consecutive periods

**Why.** Removed in commit 95e4944's predecessor (L-31) because they
were silently `false` and no callers populated them. The spec defines
them as outputs of dedicated detectors that should exist alongside the
four core macro models. Their absence means downstream consumers can't
distinguish "neutral expansion" from "restrictive expansion that's
about to break" — economically critical.

**Closes.** Macro contextualization gaps that the four-model
aggregation alone can't capture.

**Spec.** `deliverables/regime/regime_aware_portfolio_engine.md` §
"Macro Overlays".

**Status.** Detectors not built. Struct fields removed.

**Blockers.**
- Each detector has its own sub-design (how restrictive is restrictive,
  what credit benchmark, how to define "exceeds expectations").
- Re-adding the boolean fields to `MacroBelief` is trivial; building the
  detectors that populate them honestly is the work.

---

### 6. Services-aware Macro DFM

**What.** Add ISM-services PMI, PCE-services, and services CPI to the
`macro_data` panel; refit DFM with the expanded feature set; decide
factor structure post-fit (factor 1 may now span industrial + services
slack, OR a new factor for services may emerge).

**Why.** The current DFM is industrial-only — factor 1
("real_activity") loads on `manufacturing_capacity_util`,
`industrial_production`, `unemployment_rate`. The macro panel has no
services indicators, so the model's "weakness" classification fires
whenever industrial slack appears, even when services hold up (e.g.,
2025-2026). The interim relabel `RECESSION_*` → `INDUSTRIAL_WEAKNESS_*`
is honest about what the model identifies, but it's a labeling fix, not
a modeling fix.

**Closes.** `KNOWN_GAPS.md` Gap 9 (RECESSION semantics).

**Status.** Requires data ingest. After fit, decide:
- (a) Rename back: `INDUSTRIAL_WEAKNESS_*` → `RECESSION_*` (services
  data makes "recession" legitimate)
- (b) Keep both as separate states for richer ontology

**Blockers.** Data engineering — adding columns to `macro_data` schema,
backfilling history.

---

### 7. Per-sleeve target fingerprints

**What.** Expose `target_fingerprints` in `SleeveConfig` so each sleeve
can override the default HMM/MSAR ontology targets. Rates' typical σ is
~4× lower than equities', so the same z-scored target geometry doesn't
land where it should for both.

**Why.** Currently target fingerprints are identical across sleeves
(equities/rates/FX/commodities all use the same 2D/3D/4D targets). The
audit identified this as a calibration enhancement — the substrate
already adapts via z-scoring within sleeve, but per-sleeve targets
would let the ontology encode sleeve-specific economic *ratios* (e.g.,
rates' MEANREV is more persistent than equities' MEANREV).

**Closes.** `KNOWN_GAPS.md` Gap 8 (per-sleeve target fingerprints).

**Status.** Designed; deferred from Phase 3 because the marginal value
was unclear given z-score adaptation. Worth a focused experiment with
an A/B regression run before committing.

**Blockers.** None technical; calibration work.

---

### 8. Slow-bear stress state split

**What.** Decide between three options to make slow-bear periods (e.g.,
2022 H2 equities) classify as STRESS rather than MEANREV:
- (a) Lower STRESS_PRICE σ target from 1.0 z to ~0.5 z (single-knob
  fix; risks false positives on normal high-vol periods)
- (b) Steeper ret60 attractor on STRESS_PRICE (currently −1.2; risks
  same)
- (c) Architectural: split STRESS_PRICE → `STRESS_ACUTE` +
  `STRESS_PERSISTENT` in the ontology. Each gets its own attractor —
  STRESS_ACUTE high σ + neg ret60, STRESS_PERSISTENT moderate σ + very
  neg ret60. Cleanest fix, biggest blast radius.

**Why.** 2022 H2 S&P fell ~25% over Q3-Q4 with elevated-but-not-extreme
σ. STRESS_PRICE probability nearly doubled vs pre-K-05+ but still
doesn't cross dominance. Option (c) is the right long-term answer; (a)
and (b) are tactical.

**Closes.** `KNOWN_GAPS.md` Gap 1 (slow grinding bear).

**Status.** Tradeoffs documented; decision pending.

**Blockers.** Decision on whether to extend the 5-state ontology.
Option (c) requires backfilling the new attractors and a full
regression run.

---

## Smaller enhancements

### Config-gated GMM determinism per restart

**What.** Add `seed_per_restart` config knob to the GMM EM driver so
each restart's RNG is independently seedable. Default off — preserves
current `(X, K, seed)` determinism property (verified by existing
tests).

**Why.** Reproducibility under data refresh: when the input panel
shifts by one bar (because a new bar was added), the current rng
threading through restarts perturbs convergence and can move the regime
call. A per-restart seed isolates this.

**Status.** Deferred from Phase 4 because turning it on changes the
init pattern across restarts → can move converged solutions → needs
A/B validation before flipping default.

**Action when picked up.** Land the config knob; run a per-fit
perturbation A/B (same input, both seed modes); accept default-on only
if regime-call diff is below an explicit threshold (e.g., < 0.5% of
bars change call).

---

### Config-gated EGARCH asymmetry threshold mode

**What.** Add `egarch_asymmetry_threshold_mode = "fixed" | "tstat"`. In
"fixed" mode (current behavior), asymmetry flag fires when γ < −0.01.
In "tstat" mode, fires when γ is statistically significant (t-stat >
2) against its standard error.

**Why.** The hardcoded −0.01 is empirically calibrated, not
statistical. A t-stat-based test is more principled but flags different
bars during borderline-asymmetric periods, which shifts GARCH
contributions and can shift the regime call.

**Status.** Deferred from Phase 4. Never default `"tstat"` without
historical-period A/B (COVID, SVB, 2022 hikes).

**Action when picked up.** Land the config knob; run economic
validation against the fixture dates in `KNOWN_GAPS.md` § "Test
fixture: known-period regression suite"; flip default only if the
fixture rows hold or improve.

---

### UTC date parsing — upstream Timestamp passing

**What.** Stop parsing date strings inside `market_data_loader` and
`macro_data_loader`. Have the runners parse `start_date` / `end_date`
to UTC `Timestamp` values once and pass them through.

**Why.** The current `mktime()` path interprets dates as local TZ; on a
non-UTC dev host this shifts the bar boundary by the TZ offset and
silently changes the panel. Switching the loader to `timegm()` was
attempted in Phase 4 and reverted — the TZ-shift was caught by timeline
regression and pulled in 4 extra pre-COVID bars.

The proper fix lives upstream in the runner, not in the loader. See the
NOTE comment in `src/regime_detection/market/market_data_loader.cpp`'s
`load_symbol` for the rationale.

**Status.** Deferred — needs runner refactor to construct UTC
Timestamps before calling the loader.

**Action when picked up.** Replace runner argv parsing with explicit
UTC construction; drop the string-parsing lambda from the loader; run
timeline regression on the dev host AND a UTC host to confirm no shift.

---

## Roadmap summary

| Priority | Enhancement | Closes gaps | Effort | Blocker |
|---|---|---|---|---|
| 1 | MBFS | 2, 6, 10 | Large | Spread/depth data ingest |
| 2 | Conflict Resolver | 7 | Medium | Theory call (cap vs blend) |
| 3 | Cross-Asset Overlay | 5, 4 | Medium | Overlay-vs-state decision |
| 4 | ML Confirmer | residual ambiguity | Large | Labeling + framework choice |
| 5 | Macro Overlay Detectors | macro context | Medium | Per-detector sub-design |
| 6 | Services-aware DFM | 9 | Small (after data) | Data ingest |
| 7 | Per-sleeve target fingerprints | 8 | Small | Calibration A/B |
| 8 | Slow-bear stress split | 1 | Medium | Ontology decision |
| — | Config-gated GMM rng | reproducibility | Small | Default-flip A/B |
| — | Config-gated EGARCH t-stat | asymmetry rigor | Small | Default-flip A/B |
| — | UTC dates upstream | TZ portability | Small | Runner refactor |

The first four enhancements (MBFS, Conflict Resolver, Cross-Asset
Overlay, ML Confirmer) together close most of the operationally
important gaps. They're listed in priority of *behavioral impact*, not
implementation effort.

## What this doc does NOT cover

- The 10 specific behavioral gaps with fixture dates and acceptance
  criteria — see `KNOWN_GAPS.md`.
- Internal numerical bugs / library audit — those landed across the
  Phase 0–4 commit series; the substance lives in commit messages.
- Live deployment ops (scheduled refits, model versioning, alerting on
  regime transitions) — separate ops doc, unblocked by the live-state
  serialization API but requires infrastructure beyond this module.
