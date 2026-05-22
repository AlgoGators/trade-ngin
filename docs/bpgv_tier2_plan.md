# BPGV Tier 2 Plan — Close the Alpha Leaks Without Overfitting

## Context

Three runs now sit in the DB:

| Run | CAGR | Sharpe | Max DD |
|---|---:|---:|---:|
| Baseline (`..._212846_627`) | 5.59 % | 0.504 | 16.48 % |
| Tier 1 (`..._235451_768`) | 2.03 % | 0.629 | 5.14 % |
| **Remediation (`..._011441_289`)** | **3.93 %** | **0.705** | **12.80 %** |

The remediation run shipped six mechanical fixes that cleanly solved downside
control (SPY whipsaw eliminated, crash overrides 106 → 3, max DD within
target). But the diagnostic in
[docs/bpgv_alpha_attribution.md](bpgv_alpha_attribution.md) shows the strategy
is still catastrophically **under-invested**:

- **Target equity weight 77.6 %, realized 13.3 %** — filter stack cuts 64 pp
  off every trading day, even in risk-on regimes.
- **SPY held only 36 % of days**, IWM only 15 %.
- Bull years leak 15–25 pp vs SPY (2013, 2017, 2019, 2023, 2024).
- 10-month coordinator warmup wastes year one (60/40 would have made +13.4 %).

Tier 2 is about closing these leaks while keeping the Tier 1 downside wins
and not overfitting to the 2011–2026 window.

---

## Guiding principles (overfitting controls)

This is a strategy with ~180 monthly observations and 20+ tunable parameters —
Bailey/López de Prado deflated-Sharpe territory. Every workstream below is
designed to add ≤ 1 degree of freedom per A/B test. Hard rules:

1. **No parameter grid searches.** Each proposed change is a *single binary
   flip* or a single move to a literature-documented value (e.g.
   `atr_k 2.0 → 1.0`, Faber 2007). No "try values in {0.5, 1.0, 1.5, 2.0}".
2. **No re-tuning what already works.** Homebuilder tilt, vol-scaled momentum
   tilt, SPY-zeroed crash basket, tolerance-band rebalancing — these earn
   their keep per the attribution doc. Don't touch them.
3. **One change per backtest.** Stack only after each lands and is documented.
   Attribution otherwise blurs.
4. **Keep both the Tier 1 baseline and the Tier 1 remediation run as
   reference lines** for every Tier 2 A/B — not just "vs baseline".
5. **No new macro signals in Tier 2.** NFCI / HY OAS / Sahm / 10Y-3M remain
   Tier 3 per the research doc. Adding signals before fixing the exposure
   gap would be building on sand.
6. **Every Tier 2 commit must regenerate [docs/bpgv_tier1_summary.md](bpgv_tier1_summary.md)
   tables** with a new column. No orphan runs.

---

## Workstreams, sequenced by overfit risk (low → high)

### W1 — Trace and fix the coordinator-layer warmup (Priority 1)

**Zero overfit risk** — this is a mechanical bug that costs 84 bps/yr once,
forever. The strategy-level warmup pre-load (Tier 1 Fix 3) works — filter
histories are full on day 1 — but first execution is still 2012-04-30, ten
months after the 2011-04-21 backtest start.

**What to do**:
1. Explore agent (read-only) to trace `is_warmup` flag through
   `BacktestCoordinator` and `PortfolioManager`. Unused parameter at
   [backtest_coordinator.cpp:370](src/backtest/backtest_coordinator.cpp:370)
   was flagged by the compiler during Tier 1 implementation — start there.
2. Find the layer that gates execution on "insufficient portfolio history"
   and determine whether to (a) respect `warmup_start_date` from the
   strategy config, (b) remove the gate entirely for backtests, or (c) add
   a config toggle.
3. Implement the fix, unit-test with a trivial strategy that rebalances on
   day 1. Verify first execution is ≤ 5 trading days after `start_date`.

**Expected lift**: +80 bps/yr CAGR (one-time ~13 % pickup, amortized).
**Effort**: 1–2 days of exploration + a targeted fix. No parameter tuning.
**Overfit risk**: zero — mechanical.
**Validation**: single backtest, compare first-execution date and 2011–2012
performance to remediation run.

### W2 — Unstack one more filter: drop the graded ATR scaler (Priority 2)

**Low overfit risk** — this is *removing* a parameter, not adding one.

**Rationale from the attribution doc §3**: the 200d SMA × ATR-graded × [0, 1]
multiplier is the single largest contributor to the 64 pp exposure gap.
Even names trading *above* their 200d SMA get multiplied by score < 1
whenever they're less than 2 ATR above SMA — which is most of the time.

**What to do**:
- Set `breakout.graded_weighting: false` in config.
- The existing code already supports binary mode (`mode: "binary_legacy"`) —
  no code change needed, just config.
- 3-bar confirmation remains — that's what Faber (2007) validated at the
  *binary* level. Index gate stays off (already disabled in remediation).

**Expected lift**: +100–150 bps/yr CAGR (from attribution §3 and
§5 — SPY / IWM held days should roughly double).
**Effort**: config change + one backtest.
**Overfit risk**: low — we're removing a scaler, not adding one. Binary mode
is literature-standard (Faber 2007, Moskowitz-Ooi-Pedersen 2012).
**Validation**: single backtest, check realized equity weight in §3 buckets
rises from 13 % toward ~40 %.

### W3 — Drop the TSM absolute gate entirely (Priority 3)

**Low-to-medium overfit risk** — also removing a parameter. Keeps the
vol-scaled cross-sectional momentum ranker from Tier 1 Change 5.

**Rationale**: the attribution shows the TSM gate is the *second* contributor
to §3's exposure gap. The momentum tilt itself (Tier 1 Change 5's xsec
ranker) has t = 2.0 spread, so we don't need the absolute gate on top —
the relative ranker already captures the signal.

**What to do**:
- Set `momentum.tsm_gate_enabled: false` in config.
- The xsec vol-scaled ranker continues to fire.
- W3 sequences *after* W2 to avoid bundling two relaxations in one run —
  we want per-fix attribution.

**Expected lift**: +75–125 bps/yr CAGR.
**Effort**: config change + one backtest.
**Overfit risk**: low — the gate is a literature-documented add-on to xsec
momentum; removing it doesn't introduce any free parameter.
**Validation**: single backtest after W2 lands, check SPY/IWM held-day
percentages rise further, 2017/2019/2023/2024 shortfall closes.

### W4 — Rebalance discipline: monthly-only mode (Priority 4)

**Zero-to-low overfit risk**.

**Rationale**: trade-days/yr after remediation is ~200 (target 30–60). The
residual trades come from the daily tolerance-band check producing 1–3
share trims on days when monthly weights haven't changed. Locking
rebalancing strictly to the monthly day + crash-override entry/exit should
hit the 30–60 range without affecting PnL (Jaconetti-Kinniry-Zilbering
2010 showed frequency barely affects risk-adjusted returns).

**What to do**:
- Add `rebalance.mode: "monthly_only"` option (alongside current
  "tolerance_band"). Default remains "tolerance_band" for back-compat.
- Active config sets `rebalance.mode: "monthly_only"`.
- In `get_target_positions()`, if mode is "monthly_only", short-circuit
  the daily drift check — always return the last-rebalance target.

**Expected lift**: +30–50 bps/yr CAGR (from cost savings ~$3–5k less over
15 years).
**Effort**: small code change + config + 2 unit tests + 1 backtest.
**Overfit risk**: zero — it's a frequency choice, not a parameter tune.
**Validation**: trade-days/yr falls to 12 × 1.5 = ~18 (12 monthly + 6
override-adjacent); cost drag < $3 k total.

### W5 — Investigate the perverse regime-score IC (Priority 5)

**HIGH overfit risk if done wrong. Do this last, do it carefully.**

**The finding**: Spearman IC of `regime_score` vs forward SPY return is
+0.19 at 12 m (p = 0.013, n = 166). That's significant evidence the
signal is either (a) inverted at long horizons, (b) broken, or (c)
correctly a "buy the stress" contrarian at long horizons.

**What to do — research, not implement**:
1. Build a research notebook (not a backtest) that plots `regime_score`
   against forward 1/3/6/12 m SPY returns, with split-sample analysis
   (2001–2013 vs 2014–2026) to check if IC is stable across subsamples.
2. If IC > 0 is stable across subsamples → the signal is contrarian.
3. If IC collapses in the out-of-sample half → it's a coincidence and no
   action.
4. If stable, the Tier 3 direction is *not* "flip the signal" (that
   doubles overfit risk) but "add a second, shorter-horizon signal" to
   decouple short-term defense from long-term opportunity.

**Expected lift**: unknowable without the investigation. Could be 0 or
could be 100+ bps/yr.
**Effort**: 1 day of notebook work — no code changes, no backtest.
**Overfit risk**: HIGH if we start tuning the score composition. The
research output should be a plain-English finding, not a parameter change.
**Validation**: the sub-sample IC table itself is the deliverable. Decisions
about whether to change the score go into a *separate* plan.

---

## Sequencing and gates

Run workstreams in order W1 → W4 → W2 → W3 → W5. Rationale for this
(non-obvious) ordering:

1. **W1 first**: mechanical, zero overfit risk, biggest confidence.
   It also reveals the true first-trade date for every subsequent run,
   which makes bull-year attribution cleaner for W2/W3.
2. **W4 before W2**: locking rebalancing to monthly means fewer "noise"
   trades from intra-month filter swings. When we then relax the ATR
   scaler (W2), the trade-count blow-up is contained.
3. **W2 before W3**: attribution doc says ATR scaler is the bigger of
   the two remaining filter drags. Fix the bigger one first, measure,
   then decide if W3 is still worth it.
4. **W3**: if W2 alone closed most of the exposure gap (realized equity
   > 50 %), W3 may be unnecessary — save the overfit budget.
5. **W5 last**: research only. Informs Tier 3 direction.

**Hard gates between workstreams**:
- After W1, if first-execution is still > 1 month after backtest start,
  **stop and escalate**. W1 didn't land.
- After W2, if realized equity weight in risk-on regimes is still < 30 %,
  **stop**. We mis-diagnosed §3 of the attribution doc.
- After W3, if Sharpe drops below 0.60 while CAGR rises, re-examine —
  we may be giving up too much downside protection.

---

## Success criteria

Per-workstream targets are stated above. Tier 2 end-state target:

| Metric | Remediation (baseline for Tier 2) | Tier 2 target |
|---|---:|---:|
| CAGR | 3.93 % | **≥ 6.5 %** |
| Sharpe (Rf = 0) | 0.99 | ≥ 0.85 (can spend some Sharpe for return) |
| Max DD | 12.80 % | ≤ 15 % |
| Realized equity weight | 13.3 % | ≥ 40 % avg, ≥ 55 % in risk-on regimes |
| Trade-days/yr | ~200 | 30–60 |
| Crash overrides | 3 | 3–10 (don't regress) |
| First-execution delay | 375 days | ≤ 10 days |

If CAGR lands at ~6 % but realized equity is still only ~30 %, the
attribution has shifted — investigate before doing anything else.

If CAGR lands at ~6 % with realized equity at 50 %+, we've correctly
diagnosed the exposure gap and Tier 2 is done. Tier 3 (validation stack,
out-of-sample extension, new macro signals) begins.

---

## Explicit out-of-scope for Tier 2

These are tempting traps:

- **Tuning `atr_k` in a range**. Don't. W2 flips it off entirely; picking
  a new numeric value is a search.
- **Adding back any of the six Tier 1 mechanical fixes**. They work as a
  set.
- **Building a new macro signal composite** (NFCI / HY OAS / Sahm / 10Y-3M).
  Tier 3.
- **2001–2011 walk-forward extension with proxies**. Tier 3 — requires
  careful vintage work (VUSTX → TLT, GOLDAMGBD228NLBM → GLD, ^SOX → SMH).
- **Changing sizing from fixed-capital to live-NAV**. Guardrail from
  the original Tier 1 prompt (no live-NAV sizing).
- **Deflated-Sharpe / Harvey-Liu multi-testing adjustments**. Tier 3
  validation. Doing these *before* fixing the exposure gap would be
  validating the wrong thing.

---

## Deliverables

Per workstream:
1. A single commit on `feature/bpgv-strategy` (or a `feature/bpgv-tier2`
   sub-branch) with a descriptive subject line.
2. One 15-year backtest → new `run_id` in `backtest.results`.
3. One new column in [bpgv_tier1_summary.md](bpgv_tier1_summary.md) tables
   (no new file per workstream — single rolling document).

End of Tier 2:
4. One rolled-up summary addendum: **[docs/bpgv_tier2_summary.md]** with
   final before/after across all 5 workstreams and the full lineage of
   run_ids.

---

## Reused functions / utilities

No new machinery is required for W1–W4. Every change is either config or
a minimal extension of existing code paths:
- [apply_breakout_filter](src/strategy/bpgv_rotation.cpp) already supports
  `graded_weighting` flag (W2 is pure config).
- [apply_momentum_tilt](src/strategy/bpgv_rotation.cpp) already supports
  `tsm_gate_enabled` flag (W3 is pure config).
- [get_target_positions](src/strategy/bpgv_rotation.cpp) needs a mode
  switch for W4 — ~20 lines of code.
- W1 is in the coordinator/portfolio layer, not the strategy.
- W5 is research only — pandas notebook against existing CSVs.

## References to keep honest

- [docs/bpgv_alpha_attribution.md](bpgv_alpha_attribution.md) — the
  leak-by-leak diagnosis this plan addresses.
- [docs/bpgv_tier1_summary.md](bpgv_tier1_summary.md) §4.3 + §7 — the
  residual-gap hypothesis that the attribution confirmed.
- [docs/bpgv_improvement_research.md](bpgv_improvement_research.md) — the
  broader research agenda (kept for Tier 3 use).
- `~/Downloads/compass_artifact_*.md` — the original research doc with
  validation-stack guidance for Tier 3.
