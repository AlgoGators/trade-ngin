> **SUPERSEDED (2026-08-30)** by the tracked `../KNOWN_GAPS.md` (same 10 gaps,
> consolidated labels, fixture table). Kept for history.

# Regime Pipeline — Known Gaps & Acceptance Criteria

**Doc date**: 2026-04-29
**Pipeline state**: Phase 3 partial — K-04 v2 + K-05 (liquidity 3rd dim) + K-05+ (drawdown 4th dim) committed.
**Companion docs**:
- `docs/REGIME_PIPELINE_ANALYSIS.md` — original 30-item M-/K- backlog
- `docs/REGIME_PIPELINE_LIBRARY_AUDIT.md` — L-# library audit
- `docs/REGIME_PIPELINE_FIX_PLAN.md` — phased plan
- `docs/REGIME_PIPELINE_FIX_LOG.md` — running log of what landed
- `deliverables/regime/MARKET_PIPELINE_GAP_ANALYSIS.md` — newer-spec MBFS / ML / overlay specs

## 0. Purpose of this doc

Two readers:

1. **Operators using the pipeline today** — what NOT to trust the regime call for. Where regime probability is suggestive but not authoritative. Use this for risk-control decisions before consuming `MarketBelief` / `MacroBelief`.
2. **Post-reorg engineers building MBFS / cross-asset overlays / conflict resolver** — what the user-facing behavior MUST look like after each enhancement lands. Use the *acceptance criteria* sections below as regression-test targets, not just spec correctness.

Each gap below has a **concrete trigger date** + **expected output** + **current output** so a future fix can validate against the same fixture.

---

## 1. Stress detection coverage (the core taxonomy)

The regime ontology has 5 market states:
- TREND_LOWVOL, TREND_HIGHVOL, MEANREV_CHOPPY, STRESS_PRICE, STRESS_LIQUIDITY

In economic terms there are at least **6 distinct stress signatures** the pipeline should be able to identify:

| Stress type | Phenomenology | Coverage |
|---|---|---|
| **Acute crash** | σ explodes 3-5×, drawdown collapses days | ✅ Closed — K-04 v2 + K-05+ |
| **Persistent macro stress** | σ elevated, persistent direction (e.g., Fed hikes) | ✅ Closed — K-05+ ret60 dim |
| **Slow grinding bear** | σ moderate, persistent neg drift over weeks/months | ⚠️ **Partial** — see Gap 1 below |
| **Liquidity dysfunction** | σ moderate, volume collapses, spreads widen | ⚠️ **Token coverage** — see Gap 2 |
| **Correlation breakdown** | σ moderate per-asset, cross-asset corr flips | ❌ Open — see Gap 3 (deferred) |
| **Funding stress** | σ low, credit spreads widen, equity vol low | ❌ Open — see Gap 4 (deferred) |

---

## 2. Active residual gaps (close in Phase 4 or before ship)

### Gap 1 — Slow grinding bear doesn't dominate STRESS_PRICE

**Phenomenology**: 2022 H2 S&P 500 fell ~25% over Q3-Q4 with daily σ ~25% annualized (elevated, not extreme). No single-day crash; just persistent negative drift.

**Trigger fixture (validation date set)**:
- 2022-09-26 (S&P 3655 — bear bottom approach)
- 2022-10-12 (S&P 3491 — actual cycle low)
- 2022-12-22 (S&P 3822 — pre-Christmas tax loss selling)

**Expected output**: Equities sleeve dominant regime should be STRESS_PRICE on these dates (or at least majority STRESS_PRICE during 2022-09 through 2022-12).

**Current output (K-05+)**:
```
2022-09-26 equities: MEANREV=0.287, STRESS_PRICE=0.186, TREND_HIGHVOL=0.206
2022-10-12 equities: MEANREV similar, STRESS_PRICE ~0.20
Annual 2022: STRESS_PRICE 5.8%, MEANREV 50.0%, TREND_LOWVOL 44.2%
```
STRESS probability is meaningful (doubled from K-04 v2) but doesn't cross the dominant threshold. MEANREV wins because the σ wasn't extreme enough to clear the STRESS_PRICE σ target (1.0 z-score) in cross-state standardization.

**Why it's open**: Three ways to fix, each with risk:
- **Lower STRESS_PRICE σ target** (currently 1.0): would make 2022 H2 STRESS but risk false positives during normal high-vol periods
- **Steeper ret60 attractor** (currently −1.2): same risk
- **Architectural: split STRESS_PRICE → STRESS_ACUTE + STRESS_PERSISTENT** in the ontology. Each gets its own attractor — STRESS_ACUTE high σ + neg ret60, STRESS_PERSISTENT moderate σ + very neg ret60. Cleanest fix.

**Acceptance criterion for fix**:
- Equities 2022 H2 (Aug-Dec) should be ≥40% STRESS_PRICE (or ≥40% combined STRESS_PERSISTENT + STRESS_ACUTE)
- 2024 bull period should remain ≤5% STRESS_PRICE (no false positives)

**Suggested test addition**:
```cpp
TEST(RegimeEconomic, SlowBearDominatesStress_Gap1) {
    // Run pipeline against 2022 H2 timeline
    // Assert dominant regime is STRESS_PRICE (or *_PERSISTENT) on >50% of bars
    // between 2022-09-01 and 2022-12-31 for equities sleeve
}
```

---

### Gap 2 — Liquidity dysfunction only token coverage

**Phenomenology**: Treasury market March 16-23 2020 — yields whipsawed but volume *collapsed*; market makers couldn't absorb flow. Fed had to intervene with unlimited QE specifically to restore liquidity, not because prices were too low.

**Trigger fixture**:
- 2020-03-12 (Fed announces $1.5T repo injection)
- 2020-03-15 (FOMC emergency cut to 0%)
- 2020-03-23 (Fed announces unlimited QE specifically for Treasuries)

**Expected output**: Rates sleeve dominant regime should be STRESS_LIQUIDITY (not STRESS_PRICE) on at least Mar 16-23 2020.

**Current output (K-05+)**:
```
Annual 2020 rates: STRESS_LIQUIDITY 0.8% (~12 bars total)
2020-03 rates: STRESS_PRICE every bar (we want STRESS_LIQUIDITY for the dysfunction window)
```
STRESS_LIQUIDITY fires 0.8% but it's not concentrated in the dysfunction window. The HMM aggregates panic crashes (high volume) and dysfunction (low volume) into the same state, so the per-state liquidity mean is muted.

**Why it's open**: HMM-on-returns alone can't separate panic-crash from dysfunction. Both have high σ. The discriminator is *volume direction* — panic = volume up, dysfunction = volume down. K-05's per-state liquidity mean averages across both phases.

**Acceptance criterion for fix**:
- Rates STRESS_LIQUIDITY > 50% during 2020-03-16 to 2020-03-23 inclusive
- At least one block of 5+ consecutive STRESS_LIQUIDITY bars somewhere in the panel

**Likely fix path** (post-reorg):
- MBFS (Market Behavior Feature Space) with explicit `liquidity_quality` dim from raw spread/depth/volume
- Or an HMM with more states (e.g., 5) so panic and dysfunction can separate
- Or a separate liquidity dysfunction detector layered on top of HMM regime call

---

### Gap 3 — Stress detection lags news by 1-2 weeks

**Phenomenology**: SVB news broke 2023-03-08 to 2023-03-13. Yields collapsed, regional bank stocks crashed. K-05+ rates STRESS_PRICE first fires on 2023-03-17 — 9 trading days after the news event.

**Why it lags**:
- HMM smoothing across the full panel
- ret60 window is 60 bars — slow to register a recent shock
- Forward-filtered probabilities react to data only at time t

**Acceptance criterion for fix**:
- Regime call within 3 trading days of the trigger (intraday move > 3σ in any sleeve)

**Likely fix paths**:
- Shorter ret60 window for stress detection (e.g., 20-bar parallel attractor)
- Faster GARCH or EGARCH-driven STRESS proxy that doesn't require regime smoothing
- Event detection layer that bypasses regime smoother on news-day shocks

---

### Gap 4 — FX stress detection structurally weak

**Phenomenology**: USD spike during March 2020 was a stress event (DXY +5% in a week). FX sleeve currently classifies as TREND_HIGHVOL throughout — directionally correct (USD trended), but misses the stress nature.

**Why it's structurally weak**:
- FX futures have unreliable volume data → K-05 liquidity dim doesn't fire
- FX never exhibits the sustained directional drift pattern of slow bears (carry trades unwind suddenly)
- Spec's STRESS targets calibrated for sleeves with volume signal

**Acceptance criterion for fix**:
- During 2020-03-09 to 2020-03-20 (USD acute spike), FX sleeve dominant regime should be STRESS_PRICE OR STRESS_LIQUIDITY (not just TREND_HIGHVOL)
- Same for 2022-09-26 (UK gilt crisis week — GBP cratered)

**Likely fix paths**:
- **Gap risk feature**: ratio of overnight gap vol to intraday vol (proxy for FX-specific stress)
- **Cross-asset trigger**: when DXY moves > 2σ in 5 days, override individual FX sleeve regime call
- MBFS with FX-specific liquidity proxy (futures depth, bid-ask if available)

---

## 3. Deferred gaps (require post-reorg structural work)

These cannot be addressed within the existing HMM + MSAR + GARCH + GMM stack. They require new models specified in `deliverables/regime/MARKET_PIPELINE_GAP_ANALYSIS.md`.

### Gap 5 — Cross-asset correlation breakdown
Equity-bond correlation flipped from negative (normal) to positive (both falling) in 2022 — classical regime change indicator. The pipeline currently has K-08 pooled cross-asset corr_spike *as a GMM feature*, but no first-class regime state for "correlation regime change."

**Spec ref**: `MARKET_PIPELINE_GAP_ANALYSIS.md` Gap 3 (cross-asset overlay model). New monitors:
- Equity-bond correlation flip detector
- Credit-equity contagion detector
- FX-commodity divergence tracker

**Acceptance**: when equity-bond rolling correlation flips from < −0.3 to > +0.3, the conflict resolver should boost STRESS probability across all sleeves by ≥0.15.

---

### Gap 6 — Funding stress (credit spreads widening pre-crisis)
2007 H1 had credit spreads widening but equity vol low. This is a leading indicator of crisis but the current pipeline has no input feature for it.

**Spec ref**: MBFS `correlation_stress` dim (which actually captures funding stress); also overlay monitors.

**Acceptance**: synthetic test — feed historical credit spread + equity vol pair where credit > 2σ AND equity vol < 1σ. Pipeline should output non-trivial STRESS probability (>0.10) even though equity vol is calm.

---

### Gap 7 — Macro CRISIS does not override sleeve TREND_LOWVOL
The conflict resolver between MacroBelief and MarketBelief is not implemented. Currently a sleeve calling TREND_LOWVOL during a macro CRISIS regime gets full risk allocation — the macro signal doesn't propagate.

**Spec ref**: `regime_aware_portfolio_engine.md` § Conflict Resolver.

**Acceptance**: synthetic input where MacroBelief.most_likely = RECESSION_DEFLATIONARY @ 0.8 confidence AND market sleeves call TREND_LOWVOL. Final RegimeBelief should reflect macro ceiling — sleeve risk multipliers capped at < 0.5.

---

### Gap 8 — Per-sleeve target fingerprints
Currently target fingerprints are identical across sleeves (equities/rates/FX/commodities all use the same K-04 v2 targets). The audit doc Appendix 6.4 specifies sleeve-specific targets that haven't been wired in.

**Spec ref**: `REGIME_PIPELINE_ANALYSIS.md` K-10.

**Acceptance**: rates TREND_LOWVOL σ target is lower than equities (rates have 4× lower typical σ). Per-sleeve override exposed in `SleeveConfig`.

**Status**: scheduled for Phase 3 P3-8, NOT yet done.

---

### Gap 9 — Macro RECESSION label semantics (interim relabel only)

The DFM is services-blind — factor 1 ("real_activity") loads on `manufacturing_capacity_util`, `industrial_production`, `unemployment_rate`. There are no services indicators in the macro panel. So the model's "recession" classification fires whenever industrial slack appears, even when services hold up (e.g., 2025-2026).

**Phase 3 interim fix (M-08, 2026-04-29)**: enum values renamed `RECESSION_*` → `INDUSTRIAL_WEAKNESS_*`. **No behavior change** — same probabilities, just honest labels reflecting what the model actually identifies.

**Why option C (rename) over option B (tighten threshold)**: Tightening `growth_lower_pctile` from 0.20 to 0.10 would change the regime distribution NOW and require re-tuning when services data lands later (option A). The rename is a labeling decision that costs nothing in modeling; full fix (services data) replaces the rename when ready.

**Final fix (option A, post-reorg)**:
- Add ISM-services / PCE-services / services CPI as columns in `macro_data` schema
- Re-fit DFM — factor structure will shift (factor 1 may now capture both industrial AND services slack, OR a new factor for services emerges)
- After A lands, decide:
  - (a) Rename back: `INDUSTRIAL_WEAKNESS_*` → `RECESSION_*` (services data makes "recession" legitimate)
  - (b) Keep both as separate states: `INDUSTRIAL_WEAKNESS_*` for services-resilient slowdowns + `RECESSION_*` for broad-based contractions (richer ontology)

**Acceptance for option A**:
- 2020 Q1-Q2 (COVID — broad-based contraction including services): regime call is RECESSION_* (or both RECESSION_* and INDUSTRIAL_WEAKNESS_* with RECESSION dominant)
- 2025-2026 (industrial slowing, services strong): regime call stays INDUSTRIAL_WEAKNESS_* (or SLOWDOWN_*)

**Acceptance for interim (current state)**:
- Downstream consumers reading `MacroBelief.most_likely` must handle `INDUSTRIAL_WEAKNESS_*` enum values — these are the new names of slots 4 and 5 in `MacroRegimeL1`.
- Policy layer reading "RECESSION" by string must update to read "INDUSTRIAL_WEAKNESS" until option A.
- The four other states (EXPANSION_*, SLOWDOWN_*) are unchanged.

**Status**: option C done (committed in L-32 / M-08 commit). Option A planned for post-reorg / spec extensions phase.

---

### Gap 10 — Commodities TREND_LOWVOL never fires (HMM state-mass distribution)

**Phenomenology**: 2024 saw GLD up 27%, oil grinding higher across multiple periods. The commodities sleeve currently classifies these as MEANREV_CHOPPY (94% of all bars across 2020-2025), with **0% TREND_LOWVOL coverage** despite genuine trending periods.

**Root cause** (after K-10 lite analysis):
1. Commodities HMM fits 3 states. State 0 is an extreme outlier (|μ|=0.9%/d, σ=10%/d, COVID-style crash). States 1 (μ=+0.06%, σ=1.66%) and 2 (μ=-0.0001%, σ=2.82%) are the "calm" states.
2. State 1 IS the bull-trend state; it should map to TREND_LOWVOL.
3. K-10 lite already adjusted TREND_LOWVOL target σ from −1.5 to −1.0 for commodities, which makes state 1 closer to TREND_LOWVOL than to MEANREV in z-space.
4. **BUT** state 2 gets the dominant posterior mass at most timesteps (because state 2's |μ| is closer to zero, and most days are flat or near-flat). State 2 still maps to MEANREV.
5. So bars are classified MEANREV even during periods where state 1 *should* be active.

**Why K-10 lite didn't fix it**: targets only matter for the mapping_matrix (state → ontology). The state assignment itself comes from the HMM emission/posterior. If the HMM doesn't put enough mass on state 1 during gold rallies, no target adjustment will help.

**Acceptance criterion for fix**:
- Commodities TREND_LOWVOL > 10% during 2024 (gold rally year)
- Commodities TREND_LOWVOL > 5% during 2020 H2 (post-crash recovery)

**Likely fix paths**:
- (a) Increase HMM state count to 4 or 5, so trending sub-states separate from chop sub-states
- (b) MBFS replacement (post-reorg) — MBFS computes trend_strength as a continuous feature directly from rolling autocorrelation, no HMM state assignment needed
- (c) Per-sleeve HMM init that biases toward separating bull-trend from chop-trend (commodities-specific)

**Status**: structural — needs MBFS or expanded HMM states. Documented; deferred.

---

## 4. Test fixture: known-period regression suite

Use the timeline CSV diagnostic (`TIMELINE_CSV=path market_regime_pipeline_runner ...`) to validate against these reference dates after every regime-affecting fix:

| Date | Sleeve | Expected | Current K-05+ |
|---|---|---|---|
| 2020-03-13 | equities | STRESS_PRICE dominant | ✅ STRESS_PRICE 0.27 |
| 2020-03-13 | commodities | STRESS_PRICE | ✅ |
| 2020-03-13 | rates | STRESS_PRICE | ✅ |
| 2020-03-23 | rates | STRESS_LIQUIDITY (Treasury dysfunction) | ❌ STRESS_PRICE (Gap 2) |
| 2020-08-15 | equities | TREND_LOWVOL (recovery) | ✅ |
| 2022-03-15 | rates | TREND_HIGHVOL or STRESS (first hike) | partial — STRESS_PRICE coverage post-fix |
| 2022-06-15 | equities | STRESS_PRICE majority | ❌ MEANREV (Gap 1) |
| 2022-09-26 | fx | STRESS_PRICE (UK gilt) | ❌ TREND_HIGHVOL (Gap 4) |
| 2022-09-26 | rates | STRESS (gilt contagion) | partial |
| 2022-10-12 | equities | STRESS_PRICE majority | ❌ MEANREV (Gap 1) |
| 2023-03-13 | rates | STRESS_PRICE (SVB) | partial — fires Mar 17, 4 days late (Gap 3) |
| 2024-07-15 | equities | TREND_LOWVOL | ✅ |
| 2025-04-08 | equities | TREND_LOWVOL | ✅ (last bar) |

**Use this table as the regression suite for any subsequent fix.** Each row is a binary criterion. A fix is "good enough" only when it doesn't break a ✅ row AND moves at least one ❌ row to ✅ or partial.

---

## 5. Phase-by-phase coverage targets

| Phase | Target | Status |
|---|---|---|
| Phase 0 substrate | No regime regression on the substrate | ✅ |
| Phase 1 macro correctness | M-01 lock-in broken; macro confidence reflects honest ambiguity | ✅ |
| Phase 2 market data + EM | EM robustness, no σ collapse, 2020-2025 retrain | ✅ |
| Phase 3 economic refinement | Acute + persistent stress detection; partial slow bear | ✅ (partial — gaps 1-4 documented) |
| Phase 4 polish | Address Gaps 1-4 if possible; otherwise prepare for spec extensions | ⏳ |
| Phase 5 tests | Adversarial suite catches all documented gaps | ⏳ |
| Reorg | Move files, no behavior change | ⏳ |
| Spec extensions | Close Gaps 5-7 via MBFS, cross-asset overlays, conflict resolver, ML confirmer | ⏳ post-reorg |

---

## 6. What this doc does NOT cover

- Internal numerical bugs (those are in `REGIME_PIPELINE_ANALYSIS.md` and `REGIME_PIPELINE_LIBRARY_AUDIT.md`)
- Per-sleeve weight tuning (calibration, not gap)
- Live deployment concerns (separate ops doc)

---

*End of known gaps doc. Update after every regime-affecting fix; mark gap rows as ✅/⚠️/❌ in §4 to track progress.*
