# Regime Detection — Known Gaps

For the engineer about to add the next fix. This doc inventories specific
**behavioral gaps in the current pipeline** with fixture dates, expected
vs current output, and acceptance criteria for closing each gap.

This is backward-looking from a "what's broken" perspective. For the
forward architectural roadmap (MBFS, ML confirmer, cross-asset overlay,
conflict resolver, etc.), see `ENHANCEMENTS.md`. Each gap below points
at the enhancement that would close it.

---

## 1. Stress detection coverage

The market regime ontology has 5 states:

```
TREND_LOWVOL, TREND_HIGHVOL, MEANREV_CHOPPY, STRESS_PRICE, STRESS_LIQUIDITY
```

In economic terms there are at least **6 distinct stress signatures**
the pipeline should be able to identify. The table below maps each to
the current state of coverage:

| Stress type | Phenomenology | Coverage |
|---|---|---|
| **Acute crash** | σ explodes 3-5×, drawdown collapses days | ✅ Covered |
| **Persistent macro stress** | σ elevated, persistent direction (e.g., Fed hikes) | ✅ Covered (ret60 dim) |
| **Slow grinding bear** | σ moderate, persistent neg drift over weeks/months | ⚠️ **Partial** — see Gap 1 |
| **Liquidity dysfunction** | σ moderate, volume collapses, spreads widen | ⚠️ **Token coverage** — see Gap 2 |
| **Correlation breakdown** | σ moderate per-asset, cross-asset corr flips | ❌ Open — see Gap 5 |
| **Funding stress** | σ low, credit spreads widen, equity vol low | ❌ Open — see Gap 6 |

---

## 2. Behavioral gaps (current pipeline)

### Gap 1 — Slow grinding bear doesn't dominate STRESS_PRICE

**Phenomenology.** 2022 H2 S&P 500 fell ~25% over Q3-Q4 with daily σ
~25% annualized (elevated, not extreme). No single-day crash; just
persistent negative drift.

**Trigger fixtures.**
- 2022-09-26 (S&P 3655 — bear bottom approach)
- 2022-10-12 (S&P 3491 — actual cycle low)
- 2022-12-22 (S&P 3822 — pre-Christmas tax-loss selling)

**Expected.** Equities sleeve dominant regime should be STRESS_PRICE
on these dates, or at least majority STRESS_PRICE during 2022-09 → 2022-12.

**Current.**
```
2022-09-26 equities: MEANREV=0.287, STRESS_PRICE=0.186, TREND_HIGHVOL=0.206
2022-10-12 equities: MEANREV similar, STRESS_PRICE ~0.20
Annual 2022: STRESS_PRICE 5.8%, MEANREV 50.0%, TREND_LOWVOL 44.2%
```

STRESS probability is meaningful (doubled vs pre-fix) but doesn't cross
the dominance threshold. MEANREV wins because σ wasn't extreme enough
to clear the STRESS_PRICE σ target (1.0 z-score) in cross-state
standardization.

**Why open.** Three fix paths, each with risk — see `ENHANCEMENTS.md`
§ "Slow-bear stress state split". The cleanest is splitting
STRESS_PRICE → STRESS_ACUTE + STRESS_PERSISTENT in the ontology.

**Acceptance criterion.**
- Equities 2022 H2 (Aug–Dec) ≥40% STRESS_PRICE (or ≥40% combined
  STRESS_PERSISTENT + STRESS_ACUTE under the ontology split).
- 2024 bull period ≤5% STRESS_PRICE (no false positives).

**Suggested test.**
```cpp
TEST(MarketRegimeEconomic, SlowBearDominatesStress) {
    // Run pipeline against 2022 H2 timeline.
    // Assert dominant regime is STRESS_PRICE (or *_PERSISTENT) on >50%
    // of bars between 2022-09-01 and 2022-12-31 for equities sleeve.
}
```

**Closes via.** `ENHANCEMENTS.md` § "Slow-bear stress state split".

---

### Gap 2 — Liquidity dysfunction only token coverage

**Phenomenology.** Treasury market March 16-23 2020 — yields whipsawed
but volume *collapsed*; market makers couldn't absorb flow. Fed had to
intervene with unlimited QE specifically to restore liquidity, not
because prices were too low.

**Trigger fixtures.**
- 2020-03-12 (Fed announces $1.5T repo injection)
- 2020-03-15 (FOMC emergency cut to 0%)
- 2020-03-23 (Fed announces unlimited QE specifically for Treasuries)

**Expected.** Rates sleeve dominant regime should be STRESS_LIQUIDITY
(not STRESS_PRICE) on at least Mar 16-23 2020.

**Current.**
```
Annual 2020 rates: STRESS_LIQUIDITY 0.8% (~12 bars total, scattered)
2020-03 rates: STRESS_PRICE every bar
```

STRESS_LIQUIDITY fires 0.8% but isn't concentrated in the dysfunction
window. The HMM aggregates panic crashes (high volume) and dysfunction
(low volume) into the same state, so the per-state liquidity mean is
muted.

**Why open.** HMM-on-returns alone can't separate panic-crash from
dysfunction. Both have high σ. The discriminator is *volume direction*
— panic = volume up, dysfunction = volume down. The current
liquidity-as-3rd-fingerprint-dim averages across both phases.

**Acceptance criterion.**
- Rates STRESS_LIQUIDITY > 50% during 2020-03-16 to 2020-03-23 inclusive.
- At least one block of 5+ consecutive STRESS_LIQUIDITY bars somewhere
  in the panel.

**Closes via.** `ENHANCEMENTS.md` § "MBFS" (explicit `liquidity_quality`
dim from raw spread/depth/volume data).

---

### Gap 3 — Stress detection lags news by 1-2 weeks

**Phenomenology.** SVB news broke 2023-03-08 to 2023-03-13. Yields
collapsed, regional bank stocks crashed. Rates STRESS_PRICE first fires
2023-03-17 — 9 trading days after the news event.

**Why it lags.**
- HMM smoothing across the full panel
- ret60 window is 60 bars — slow to register a recent shock
- Forward-filtered probabilities react to data only at time t

**Acceptance criterion.**
- Regime call within 3 trading days of the trigger (intraday move > 3σ
  in any sleeve).

**Likely fix paths.**
- Shorter ret60 window for stress detection (e.g., 20-bar parallel
  attractor)
- Faster GARCH or EGARCH-driven STRESS proxy that doesn't require
  regime smoothing
- Event detection layer that bypasses the regime smoother on news-day
  shocks

**Closes via.** No single enhancement — possibly a fast-path layer
between GARCH and aggregation. Lower priority than Gaps 1, 2, 5, 6, 7.

---

### Gap 4 — FX stress detection structurally weak

**Phenomenology.** USD spike during March 2020 was a stress event (DXY
+5% in a week). FX sleeve currently classifies as TREND_HIGHVOL
throughout — directionally correct (USD trended), but misses the
stress nature.

**Why structurally weak.**
- FX futures have unreliable volume data → liquidity dim doesn't fire
- FX rarely exhibits the sustained directional drift pattern of slow
  bears (carry trades unwind suddenly)
- Default STRESS targets calibrated for sleeves with volume signal

**Acceptance criterion.**
- 2020-03-09 to 2020-03-20 (USD acute spike): FX sleeve dominant
  regime should be STRESS_PRICE OR STRESS_LIQUIDITY (not just
  TREND_HIGHVOL).
- Same for 2022-09-26 (UK gilt crisis week — GBP cratered).

**Likely fix paths.**
- Gap-risk feature: ratio of overnight gap vol to intraday vol (proxy
  for FX-specific stress)
- Cross-asset trigger: when DXY moves > 2σ in 5 days, override
  individual FX sleeve regime call
- MBFS with FX-specific liquidity proxy (futures depth, bid-ask if
  available)

**Closes via.** `ENHANCEMENTS.md` § "Cross-Asset Overlay Monitor"
(DXY trigger), `ENHANCEMENTS.md` § "MBFS" (FX-specific liquidity proxy).

---

### Gap 5 — Cross-asset correlation breakdown

**Phenomenology.** Equity-bond correlation flipped from negative
(normal) to positive (both falling) in 2022 — classical regime change
indicator. The pipeline currently has pooled cross-asset `corr_spike`
as a GMM feature, but no first-class regime state for "correlation
regime change."

**Acceptance criterion.**
- When equity-bond rolling correlation flips from < −0.3 to > +0.3,
  the conflict resolver should boost STRESS probability across all
  sleeves by ≥0.15.

**Closes via.** `ENHANCEMENTS.md` § "Cross-Asset Overlay Monitor".

---

### Gap 6 — Funding stress (credit spreads widening pre-crisis)

**Phenomenology.** 2007 H1 had credit spreads widening but equity vol
low. This is a leading indicator of crisis but the current pipeline
has no input feature for it.

**Acceptance criterion.**
- Synthetic test: feed historical credit spread + equity vol pair
  where credit > 2σ AND equity vol < 1σ. Pipeline should output
  non-trivial STRESS probability (>0.10) even though equity vol is
  calm.

**Closes via.** `ENHANCEMENTS.md` § "MBFS" (`correlation_stress` dim
captures funding stress).

---

### Gap 7 — Macro signal does not override sleeve TREND_LOWVOL

**Phenomenology.** Currently a sleeve calling TREND_LOWVOL during a
macro `INDUSTRIAL_WEAKNESS_*` regime gets full risk allocation — the
macro signal doesn't propagate. The conflict resolver between
MacroBelief and per-sleeve MarketBelief is not implemented.

**Acceptance criterion.**
- Synthetic input where MacroBelief.most_likely =
  INDUSTRIAL_WEAKNESS_DEFLATIONARY @ 0.8 confidence AND market sleeves
  call TREND_LOWVOL.
- Final combined RegimeBelief should reflect macro ceiling — sleeve
  risk multipliers capped at < 0.5.

**Closes via.** `ENHANCEMENTS.md` § "Conflict Resolver".

---

### Gap 8 — Per-sleeve target fingerprints

**Phenomenology.** Currently target fingerprints are identical across
sleeves (equities, rates, FX, commodities all use the same default 2D /
3D / 4D targets). Rates' typical σ is ~4× lower than equities', so the
same z-scored targets don't land where they should for both.

**Acceptance criterion.**
- Rates TREND_LOWVOL σ target is lower than equities' (rates have ~4×
  lower typical σ).
- Per-sleeve override exposed in `SleeveConfig`.

**Closes via.** `ENHANCEMENTS.md` § "Per-sleeve target fingerprints".

---

### Gap 9 — Macro INDUSTRIAL_WEAKNESS labels are interim

**Phenomenology.** The DFM is services-blind — factor 1
("real_activity") loads on `manufacturing_capacity_util`,
`industrial_production`, `unemployment_rate`. There are no services
indicators in the macro panel. So the model's "weakness" classification
fires whenever industrial slack appears, even when services hold up
(e.g., 2025-2026).

**Current state.** Enum values were renamed `RECESSION_*` →
`INDUSTRIAL_WEAKNESS_*` in the macro pipeline. **No behavior change** —
same probabilities, just honest labels reflecting what the model
actually identifies.

**Why interim.** The rename costs nothing in modeling; tightening the
percentile threshold would change the regime distribution NOW and
require re-tuning later when services data lands.

**Acceptance criterion (after services data lands).**
- 2020 Q1-Q2 (COVID — broad-based contraction including services):
  regime call is RECESSION_* (or both RECESSION_* and
  INDUSTRIAL_WEAKNESS_* with RECESSION dominant).
- 2025-2026 (industrial slowing, services strong): regime call stays
  INDUSTRIAL_WEAKNESS_* (or SLOWDOWN_*).

**Acceptance criterion (current state).**
- Downstream consumers reading `MacroBelief.most_likely` must handle
  `INDUSTRIAL_WEAKNESS_*` enum values — these are the names of slots 4
  and 5 in `MacroRegimeL1`.
- Policy layer reading "RECESSION" by string must update to read
  "INDUSTRIAL_WEAKNESS" until the services-aware refit lands.

**Closes via.** `ENHANCEMENTS.md` § "Services-aware Macro DFM".

---

### Gap 10 — Commodities TREND_LOWVOL never fires

**Phenomenology.** 2024 saw GLD up 27%, oil grinding higher across
multiple periods. The commodities sleeve currently classifies these as
MEANREV_CHOPPY (94% of all bars across 2020-2025), with **0%
TREND_LOWVOL coverage** despite genuine trending periods.

**Root cause.**
1. Commodities HMM fits 3 states. State 0 is an extreme outlier
   (|μ|=0.9%/d, σ=10%/d, COVID-style crash). States 1 (μ=+0.06%,
   σ=1.66%) and 2 (μ=−0.0001%, σ=2.82%) are the "calm" states.
2. State 1 IS the bull-trend state; it should map to TREND_LOWVOL.
3. Even with TREND_LOWVOL target σ adjusted, state 2 gets the dominant
   posterior mass at most timesteps (because state 2's |μ| is closer
   to zero, and most days are flat or near-flat). State 2 still maps
   to MEANREV.
4. So bars are classified MEANREV even during periods where state 1
   *should* be active.

**Why target tuning alone doesn't fix it.** Targets only matter for
the mapping_matrix (state → ontology). The state assignment itself
comes from the HMM emission/posterior. If the HMM doesn't put enough
mass on state 1 during commodity rallies, no target adjustment will
help.

**Acceptance criterion.**
- Commodities TREND_LOWVOL > 10% during 2024 (gold rally year).
- Commodities TREND_LOWVOL > 5% during 2020 H2 (post-crash recovery).

**Likely fix paths.**
- Increase HMM state count to 4 or 5, so trending sub-states separate
  from chop sub-states.
- MBFS replacement: MBFS computes trend_strength as a continuous
  feature directly from rolling autocorrelation, no HMM state
  assignment needed.
- Per-sleeve HMM init that biases toward separating bull-trend from
  chop-trend (commodities-specific).

**Closes via.** `ENHANCEMENTS.md` § "MBFS" (the cleanest fix), or HMM
state expansion as a tactical alternative.

---

## 3. Reference fixture suite

Use the timeline CSV diagnostic
(`TIMELINE_CSV=path market_regime_pipeline_runner ...`) to validate
against these fixture dates after every regime-affecting change. Each
row is a binary criterion. A change is "good enough" only when it
doesn't break a ✅ row AND moves at least one ❌ row to ✅ or partial.

| Date | Sleeve | Expected | Current |
|---|---|---|---|
| 2020-03-13 | equities | STRESS_PRICE dominant | ✅ STRESS_PRICE 0.27 |
| 2020-03-13 | commodities | STRESS_PRICE | ✅ |
| 2020-03-13 | rates | STRESS_PRICE | ✅ |
| 2020-03-23 | rates | STRESS_LIQUIDITY (Treasury dysfunction) | ❌ STRESS_PRICE (Gap 2) |
| 2020-08-15 | equities | TREND_LOWVOL (recovery) | ✅ |
| 2022-03-15 | rates | TREND_HIGHVOL or STRESS (first hike) | partial — STRESS_PRICE coverage |
| 2022-06-15 | equities | STRESS_PRICE majority | ❌ MEANREV (Gap 1) |
| 2022-09-26 | fx | STRESS_PRICE (UK gilt) | ❌ TREND_HIGHVOL (Gap 4) |
| 2022-09-26 | rates | STRESS (gilt contagion) | partial |
| 2022-10-12 | equities | STRESS_PRICE majority | ❌ MEANREV (Gap 1) |
| 2023-03-13 | rates | STRESS_PRICE (SVB) | partial — fires Mar 17, 4 days late (Gap 3) |
| 2024-07-15 | equities | TREND_LOWVOL | ✅ |
| 2025-04-08 | equities | TREND_LOWVOL | ✅ (last bar) |

Update the "Current" column after every regime-affecting fix. The
goal is to drive every row to ✅.

---

## 4. What this doc does NOT cover

- Architectural enhancements (MBFS, ML confirmer, cross-asset overlay,
  conflict resolver, macro overlay detectors) — see
  `ENHANCEMENTS.md`.
- Internal numerical bugs / library audit — those landed across the
  Phase 0–4 commit series; the substance lives in commit messages.
- Per-sleeve weight tuning (calibration, not gap).
- Live deployment concerns (separate ops doc).
