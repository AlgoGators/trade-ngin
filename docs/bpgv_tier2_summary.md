# BPGV Tier 2 — Retrospective

**Status**: **Tier 2 did not improve the strategy**. Two backtests were run; both
underperformed remediation. Configs have been reverted to the remediation
settings. Tier 2 code infrastructure (feature-flagged) stays in place for
future diagnosis.

**Current recommended config = [remediation run `BPGV_ROTATION_20260420_235451_768`]**:
CAGR 3.93 %, Sharpe 0.705, Max DD 12.80 %, 88.5 % of days holding equity.

---

## Three-way results table

| Metric | Remediation (best) | Tier 2 v1 (W1+W2+W3+W4) | Tier 2 v2 (W1+W2+W3, no W4) |
|---|---:|---:|---:|
| Run ID | `..._235451_768` | `..._034407_231` | `..._135434_204` |
| Total return (15y) | **+78.25 %** | −253.74 % | +31.76 % |
| CAGR | **3.93 %** | ≪ 0 (blew up) | 1.85 % |
| Sharpe | **0.705** | −0.250 | 0.286 |
| Max DD | **12.80 %** | 244.37 % | 17.76 % |
| Volatility | 3.01 % | 183.92 % | 3.43 % |
| Total trades | 3,100 | 1,282 | 2,499 |
| Crash overrides | 3 | 8 | 4 |
| Final equity ETFs held | **SMH + TLT + GLD** | none (short book) | **none — GLD + TLT only** |
| Key bull-year: 2019 | +3.66 % | — | +5.16 % (small lift) |
| Key bull-year: 2025 | +10.19 % | — | +5.75 % (WORSE) |

---

## What was tried

Per [docs/bpgv_tier2_plan.md](bpgv_tier2_plan.md):

- **W1 — `skip_warmup`**: bypass the coordinator's 514-day warmup since the
  strategy pre-loads its own history in `initialize()`.
- **W2 — `graded_weighting: false`**: switch the breakout filter from
  ATR-graded [0, 1] scaler to binary cut (above-SMA = full, 3 consecutive
  closes below = zero).
- **W3 — `tsm_gate_enabled: false`**: drop the 12-month absolute TSM gate,
  keep the vol-scaled cross-sectional ranker (which has a statistically
  significant +0.76 %/mo rank-1 − rank-N spread).
- **W4 — `rebalance.mode: "monthly_only"`**: short-circuit the daily
  tolerance-band check; trade only on monthly rebalance days + crash
  override entry/exit.

## What happened

### Tier 2 v1 (all four changes) — catastrophic

Strategy took large short positions on day 1 and compounded losses:
- SMH position ended day 1 at +837 in `backtest.final_positions` (correct
  target) but the execution log emitted SELL 837 for SMH — sign flipped
  somewhere between target materialization and the executor's
  `is_establishment_exec` path at
  [portfolio_manager.cpp:444-467](src/portfolio/portfolio_manager.cpp:444).
- Once short, SMH's ~3,000 % rally over 15 years crushed the book.
- Final: −253 % return, 244 % max DD.

Root cause of the sign flip not fully diagnosed. Most likely interaction
of the `monthly_only` echo-current-qty path + `skip_warmup` + the
portfolio manager's establishment logic. Marked in memory
[feedback_monthly_only_mode.md](~/.claude/projects/-Users-gannonstoner-trade-ngin/memory/feedback_monthly_only_mode.md)
so it doesn't get re-enabled accidentally.

### Tier 2 v2 (W1+W2+W3, no W4) — underperformed

- CAGR 1.85 % vs remediation's 3.93 % (−210 bps).
- Sharpe 0.286 vs 0.705 (way worse).
- Final book: GLD 54 + TLT 148, **zero equity**.
- 2011 partial-year (newly enabled by W1): −5.72 %.

Why the filter relaxations didn't help as the attribution doc predicted:

1. **W2 binary breakout cut more names than W2 graded** in choppy markets.
   Graded gave partial weight (e.g. 0.3–0.7) on names hovering near SMA;
   binary either gave full weight (close > SMA) or zero (after 3
   consecutive closes below). The "close to SMA" regime is common; binary
   zeroed more of these than graded did.
2. **W3 TSM gate off** removed the absolute filter but the vol-scaled xsec
   ranker's spread wasn't enough to carry equity exposure when W2 had
   already zeroed many names.
3. **W1 skip_warmup** captured 2011 — a year where our regime call was
   wrong (we got risk-on signals for the 2011 debt-ceiling drawdown). The
   attribution doc's estimate of +84 bps/yr assumed 60/40-style
   participation during warmup, not live trading with possibly-bad
   early-period signal.

The alpha-attribution doc's +80 + +125 + +100 = ~305 bps estimated lift
was wrong. The filters are not *independent* contributors; they interact
with both the execution path and the strategy's own normalize step.

## Code state after revert

All Tier 2 code was fully reverted (per user request) to match the
remediation state exactly. The codebase now contains only Tier 1 + Tier 1
remediation. Removed artifacts:

| Artifact | State |
|---|---|
| `BacktestCoordinatorConfig::skip_warmup` flag + coordinator logic | Removed |
| `RebalanceConfig::mode` field + monthly_only code path | Removed |
| `BPGVRotationStrategy::targets_pending_` mutable flag | Removed |
| Tier 2 unit tests (`DefaultModeIsToleranceBand`, `MonthlyOnlyModeParses`, `FullTier2RebalanceBlockParses`, `TSMGateCanBeDisabled`, `GradedWeightingCanBeDisabled`) | Removed |
| Tier 2 config changes in both portfolio.json files | Reverted to remediation values |

**Remaining in the tree** (all Tier 1 + remediation, unchanged):
- cash_symbols bucket (BIL/DBMF)
- TSM tolerance 0.05
- graded_weighting = true with index_gate = false
- tolerance_band rebalancing with 250 bps / 50 % bands
- warmup_start_date pre-load in `BPGVRotationStrategy::initialize()`

Test suite: **28 / 28 pass** (5 Tier 2 cases removed; 23 Tier 1 + remediation tests preserved).

## Lessons / what we'd do differently

1. **Test one change at a time with a backtest in between**. The plan said
   this; the user preference said batch to save time. Had we run W1 alone,
   we would have seen the 2011 −5.72 % and paused before stacking W2/W3.
2. **Verify the executor path before relying on get_target_positions
   semantics**. The establishment-execution path at
   `portfolio_manager.cpp:444-467` treats `new_qty` directly as signed
   trade size on the first post-warmup day. `skip_warmup` means the first
   post-warmup day is day 1 — fine for well-formed positive targets, but
   the monthly_only echo-current-qty path (current_qty = 0 on day 1)
   interacted badly.
3. **Filter changes don't linearly add**. The alpha-attribution doc gave
   independent bps estimates per filter; in practice, binary breakout and
   TSM-off interact multiplicatively (reducing equity via a different
   mechanism than graded + TSM-on would).
4. **Overfit avoidance is hard when the signal itself is weak**. The
   attribution doc also found regime IC is +0.19 at 12m (likely
   contrarian). Relaxing filters without fixing the signal puts us on an
   over-equity curve that reflects the weak regime score.

## Next steps — revised Tier 2 agenda

See updated [docs/bpgv_tier2_plan.md](bpgv_tier2_plan.md) (this doc's §7
becomes obsolete; the plan remains the long-term direction). Revised
near-term priorities:

1. **Diagnose the `is_establishment_exec` sign anomaly** —
   `src/portfolio/portfolio_manager.cpp:444-467`. Trace with a minimal
   test that puts a BPGV strategy into a synthetic backtest on a single
   day with known target quantities, verify BUY vs SELL sign matches.
2. **Back off W1 `skip_warmup` until the establishment path is understood**.
   The 10-month coordinator warmup is a known cost but not catastrophic.
3. **Reconsider W2/W3 as a single combined change**, not stacked — e.g.
   leave TSM gate on but lower `atr_k` from 2.0 → 1.0 (milder relaxation
   than turning the graded scaler off entirely).
4. **Tackle the regime-IC sign issue** (attribution §8.1). At 12m horizon
   the regime_score has +0.19 correlation with forward SPY — wrong
   direction. This is upstream of any filter change.

## Production configuration (current on `feature/bpgv-strategy`)

- **All Tier 1 remediation settings** (cash_symbols bucket, TSM tolerance 0.05,
  index_gate_enabled=false, wide bands 250 bps/50 %).
- **`skip_warmup: false`**, **`graded_weighting: true`**, **`tsm_gate_enabled: true`**,
  **`rebalance.mode: "tolerance_band"`** — all Tier 2 flags off.
- Expected next full-backtest numbers ≈ remediation baseline
  (CAGR 3.93 %, Sharpe 0.705, Max DD 12.80 %).
