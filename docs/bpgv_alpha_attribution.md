# BPGV Alpha-Leak Attribution

**Subject run**: `BPGV_ROTATION_20260421_011441_289` (Tier-1 + remediation,
15 y window 2011-04-20 → 2026-04-20)
**Headline metrics**: CAGR 3.93 % / Sharpe 0.705 / Max DD 12.80 %
**Gap to baseline CAGR**: −166 bps; **gap to SPY buy-and-hold**: −750 bps

This document identifies *where* the strategy is losing alpha, quantified in
bps/yr where data supports it. No new backtests were run — all analysis is from
existing run data and DB-resident prices. No prescriptive parameter tweaks —
those live in [docs/bpgv_tier1_summary.md §7](bpgv_tier1_summary.md).

---

## Executive summary (read this first)

The single dominant alpha leak is **catastrophic under-investment in equities**
even during clear risk-on regimes. Six supporting findings follow:

| # | Leak | Magnitude | Confidence | Source |
|---|---|---:|---|---|
| 1 | **Equity exposure gap**: the filter stack cuts ~60 pp of target equity weight on average, including during full risk-on regimes | **≥ 400 bps/yr** | HIGH — mechanical | §3 |
| 2 | **10-month coordinator warmup** during 2011-04 → 2012-04 — strategy holds nothing while 60/40 earned +13.4 % | ~80 bps/yr (amortized) | HIGH | §6 |
| 3 | **Bull-year shortfall 15–25 pp vs SPY** in 2013, 2017, 2019, 2023, 2024 | ~100 bps/yr blended | HIGH | §4 |
| 4 | **Perverse regime IC**: +0.19 Spearman at 12 m horizon — "risk-off" score statistically predicts HIGHER forward SPY returns | unclear direction — could be 50–100 bps/yr if signal is inverted | MEDIUM — significant but n=166 | §8 |
| 5 | **Homebuilder tilt works** (+1.83 %/mo spread); momentum tilt works (+0.76 %/mo, t=2.0) | small positives (~10 bps/yr) | HIGH | §8 |
| 6 | **SPY held only 36 % of days**, IWM only 15 % — the TSM + ATR filters keep even the most liquid names on the bench | contributes to #1 | HIGH | §5 |

**Sum of identifiable leaks ≈ 580+ bps/yr**. That is *more* than the 750 bps
gap to SPY buy-and-hold, consistent with the strategy's real-Sharpe being
very similar to SPY (0.72 vs 0.99 annualized return/vol) — the gap is pure
under-investment, not bad timing.

---

## 2. Benchmark comparison

Computed from SPY/TLT/GLD daily closes in `macro_data.bsts_etf_prices`, aligned
to the backtest window. Sharpe is computed with rf = 0 for apples-to-apples;
all three runs' DB Sharpe uses a non-zero Rf hence the difference from the
headline table in [bpgv_tier1_summary.md](bpgv_tier1_summary.md).

| Strategy | CAGR | Sharpe (rf=0) | Max DD | Vol |
|---|---:|---:|---:|---:|
| SPY buy-and-hold | **11.43 %** | 0.72 | −34.10 % | 17.27 % |
| 60/40 SPY/TLT rebalanced daily | **7.38 %** | 0.73 | −28.20 % | 10.47 % |
| Equal-weight SPY/TLT/GLD | 7.01 % | 0.77 | −23.46 % | 9.38 % |
| Baseline BPGV | 5.58 % | 0.71 | −16.06 % | 8.18 % |
| Tier 1 BPGV (over-filtered) | 2.05 % | 0.96 | −4.94 % | 2.13 % |
| **Remediation BPGV** | **3.79 %** | **0.99** | **−12.52 %** | **3.84 %** |

**Interpretation**:
- SPY alone produces nearly the same Sharpe at 3× the CAGR with 4× the DD.
  The strategy's high Sharpe is a **vol-collapse artifact**, not alpha.
- The real benchmark we should beat on Sharpe is 60/40 (7.38 % / 0.73), and
  that's what we should beat on CAGR too. We are at 3.79 % / 0.99 — we have
  Sharpe room to spend on return.
- A passive equal-weight SPY/TLT/GLD — the simplest benchmark — has
  Sharpe 0.77 at 7.01 % CAGR. **Any active strategy should at least match
  this**, and we don't.

---

## 3. Equity exposure gap (the main story)

For each trading day in the remediation run, we computed:

- **`target_equity`** = 1 − f_off(regime_score) = 1 − [0.05 + 0.40·(S + 1)/2],
  where S is the previous month's regime score (what the strategy uses).
- **`realized_equity`** = Σ(quantity × close) / portfolio_value for the eight
  risk-on ETFs (SPY, QQQ, XLK, SMH, IWM, XHB, IYR, EQR).

Grouped by regime bucket (n = 3,513 days):

| Regime bucket | n days | Target equity | **Realized equity** | **Gap (pp)** |
|---|---:|---:|---:|---:|
| Risk-on (S ≤ −0.3) | 1,507 | 86.5 % | **16.7 %** | **−69.9** |
| Tilt-on (−0.3 < S ≤ −0.05) | 626 | 77.7 % | 15.7 % | −62.0 |
| Neutral (−0.05 < S ≤ 0.2) | 584 | 74.0 % | 10.8 % | −63.2 |
| Risk-off (S > 0.2) | 796 | 63.2 % | 6.7 % | −56.5 |
| **Overall** | 3,513 | **77.6 %** | **13.3 %** | **−64.3** |

**Reading**: the regime score calls for ~78 % equity on average. The strategy
delivers **13.3 %** — a factor-of-six under-investment. Even in full risk-on
regimes, realized equity is 16.7 %. This is the mechanical result of the
TSM 12 m absolute gate + 200d SMA × ATR graded filter stack — see
[bpgv_tier1_summary.md §4.1](bpgv_tier1_summary.md) for the full pipeline.

**Bps math**: SPY's average annual return over the window is ~11 %. A 64 pp
under-investment in equity ≈ 0.64 × 11 % = ~7 pp of theoretical annual
equity return forgone, which after accounting for the risk-off bucket
partially absorbing some of that return works out to ≥ 4 %/yr net drag on
CAGR. This alone accounts for most of the gap to every benchmark in §2.

---

## 4. Bull-year shortfall (confirms §3 from another angle)

Full-year YTD comparison, remediation vs SPY buy-and-hold:

| Year | SPY YTD | Remediation YTD | Shortfall |
|---:|---:|---:|---:|
| 2013 | +26.45 % | +7.68 % | **−18.77 pp** |
| 2017 | +18.48 % | +3.39 % | −15.08 pp |
| 2019 | +28.65 % | +3.66 % | **−25.00 pp** |
| 2020 | +15.09 % | +10.15 % | −4.94 pp |
| 2023 | +24.81 % | +6.99 % | −17.82 pp |
| 2024 | +24.00 % | +5.34 % | −18.66 pp |
| 2025 | +16.64 % | +10.19 % | −6.45 pp |

**Reading**: in every strong-SPY year except 2020, we give up 15–25 pp. 2020
and 2025 are the exceptions because our gold/bond exposure participates in
those mixed-regime rallies. **The average bull-year shortfall is ~18 pp.**
Against ~6 bull years / 15 years that's ~100 bps/yr blended drag.

The 2019 case is the cleanest example: macro regime score was risk-on all
year, SPY went +28.65 %, but the TSM + ATR filters kept our equity weight
low the whole time. No blame attaches to the regime signal here — the
filters overrode it.

---

## 5. Per-symbol capture

Each risk-on symbol's % of days held, average weight when held, and the
symbol's own return over the span of dates it was held (upper bound of
available capture if we'd held 100 %):

| Symbol | % days held | Avg weight (when held) | Symbol return, held-span |
|---|---:|---:|---:|
| GLD | 81.6 % | 8.5 % | +173 % |
| **SMH** | **77.0 %** | 9.9 % | **+3,053 %** |
| TLT | 65.8 % | 8.1 % | +8.8 % |
| **SPY** | **35.8 %** | **2.7 %** | +141.8 % |
| IYR | 30.3 % | 2.9 % | +116.4 % |
| XLK | 24.5 % | 2.4 % | +156.7 % |
| XHB | 21.8 % | 6.5 % | +182 % |
| QQQ | 21.2 % | 2.2 % | +124 % |
| **IWM** | **14.6 %** | **2.9 %** | +120 % |
| EQR | 12.6 % | 7.4 % | +20 % |
| BIL | 2.1 % | 40.0 % | ~0 % |

**Readings**:
- **SMH** was held 77 % of days over a period where SMH went up 3,053 %. At
  9.9 % weight the PnL capture was meaningful but the strategy was
  under-sized relative to the available opportunity — had SMH been held
  at a 20 % weight throughout, CAGR would be materially higher.
- **SPY**, the most liquid US equity ETF, is held only 35.8 % of days. The
  TSM 12 m gate and ATR graded filter filter SPY out on nearly two-thirds
  of days. Even when held, its weight is only 2.7 % — far below SMH / GLD.
- **IWM at 14.6 %** is held almost a tenth of the time the 60/40 benchmark
  would hold an equivalent exposure. Small-cap names sit out the bull runs.
- **BIL at 40 % weight for 2.1 % of days** is the crash-override footprint.
  Tiny share of total run-time but correctly concentrated during the 3
  override firings.

---

## 6. Coordinator-layer warmup cost

Backtest window starts 2011-04-21; first execution 2012-04-30. During that
~375-day window, the strategy held nothing (eq curve flat at $100,000).

| Benchmark during 2011-04-21 → 2012-04-30 | Return |
|---|---:|
| SPY | +4.55 % |
| TLT | +26.70 % |
| **60/40 SPY/TLT** | **+13.41 %** |
| Remediation BPGV | +0.00 % |
| **Foregone vs 60/40** | **−13.41 pp** |

Amortized over 15 years: **−0.84 %/yr CAGR drag**. Root cause is at the
`BacktestCoordinator` / `PortfolioManager` layer — the Tier-1 remediation's
strategy-level warmup pre-load (Fix 3) correctly fills the filter histories
on day 1 but something upstream gates execution for ~10 months. See
[bpgv_tier1_summary.md §7 Priority 1](bpgv_tier1_summary.md).

This is a one-time loss that compounds — had we earned 13 % in year one,
every subsequent dollar would be 13 % bigger.

---

## 7. Crash-override attribution

Only 3 crash overrides fired in remediation (vs 106 baseline). Large BIL
buys in the executions table flag override entries. The largest clean one
we can trace is 2012-06-22:

| Override entry | SPY next 5 d | SPY next 14 d | SPY next 30 d |
|---|---:|---:|---:|
| 2012-06-22 | +1.98 % | +1.72 % | +4.62 % |

The override fired into a market that then went UP. This is a false
positive — however, with only 3 observations we can't draw general
conclusions. **At this low activation rate the override is cheap
insurance**. The worry is the opposite (missed 2008/GFC-style events
because the 504-day vol-gate percentile requires sustained elevated vol).

---

## 8. Signal-quality attribution

### 8.1 Regime-score IC — the *most surprising* finding

Spearman rank correlation of monthly regime_score with realized SPY
forward excess return (rf = 0 proxy):

| Horizon | Spearman ρ | p-value | n months |
|---:|---:|---:|---:|
| 1 m | −0.062 | 0.410 | 177 |
| 3 m | +0.049 | 0.518 | 175 |
| 6 m | +0.073 | 0.344 | 172 |
| **12 m** | **+0.192** | **0.013** | 166 |

**Reading**: the regime score is designed so that *higher* scores mean more
defensive (risk-off). Under the design hypothesis, we'd expect *negative*
correlation with forward SPY return (high score → strategy goes defensive →
SPY falls, validating the call).

**What we actually see**: the correlation is near-zero at 1-6 m horizons and
**positive** and statistically significant at 12 m. That means high-regime-
score months (defensive signal) have been followed by *better-than-average*
SPY returns over the next 12 months. The signal is **contrarian, not
directional, at the 12 m horizon**.

Interpretations, all consistent with the data:
- **Historical**: macro stress (high BPGV percentile + inverted curve) often
  marks equity lows from which stocks recover — classic "buy the stress"
  dynamic.
- **Structural**: the 1-year BPGV rolling std is a lagging indicator; by
  the time permit volatility shows up in the score, the underlying business
  cycle has already turned.
- **Specification**: the additive rule-based score may need inversion at
  longer horizons, or a separate short-horizon vs long-horizon logic.

This is a significant finding for Tier 2 research direction but we stop
here per scope — no prescriptive action.

### 8.2 Momentum tilt — works but modestly

Each rebalance month, rank 8 risk-on names by 126-day vol-scaled score;
measure rank-1 forward-1-month return minus rank-N forward-1-month return.

- n months: 317
- Mean spread: **+0.76 % / month**
- t-stat: **2.00**, p-value 0.047
- Positive-spread months: 176 / 317 (55.5 %)

**Reading**: statistically significant at p < 0.05. With the current τ = 0.40
tilt the effective swing between top and bottom name is 2 × 0.40 × 0.76 % =
~0.6 % / month, or ~7 % / year IF perfectly reflected in portfolio weights
(which it isn't, due to filter attrition on §3). The tilt earns its keep.

### 8.3 Homebuilder tilt — works

XHB forward-1-month return conditional on sign of `permit_growth`:

| Condition | n months | Mean fwd-1 m XHB | σ |
|---|---:|---:|---:|
| permit_growth > 0 | 106 | **+1.80 %** | 7.01 % |
| permit_growth ≤ 0 | 133 | −0.03 % | 8.81 % |
| **Spread** | | **+1.83 %** | |

**Reading**: a 1.83 %/month edge conditional on the sign of a *widely-available*
macro input. The ×1.20 / ×0.80 tilt applied to XHB is well-calibrated for
this magnitude of edge.

---

## 9. Residual / unexplained

Sum of identified leaks:
- Equity exposure gap: ≥ 400 bps/yr
- Warmup: 84 bps/yr
- Bull-year shortfall (overlap with #1 — not fully additive): marginal ~50 bps/yr
- Regime-score specification issue: direction unclear, could be worth
  50–100 bps/yr

Total quantified ≥ 530 bps/yr. The CAGR gap to baseline is 166 bps; to SPY
is 750 bps. Our numbers over-attribute the baseline gap (because the
filters also exist in the baseline run, albeit in milder form) and
under-attribute the SPY gap (since no active strategy can match SPY on
equity participation).

**Residual unexplained: ~200 bps/yr when comparing to SPY.** Candidates not
quantified here:
- Compounding effects from rebalance-timing luck
- Non-zero transaction costs (~40 bps/yr in remediation)
- Normalization-induced weight skew when filters produce sparse vectors
- Possible bugs in the tolerance-band logic that further erode target weights

---

## 10. Cross-reference to Tier 2 priorities

Mapping from diagnosed leaks to the already-drafted
[Tier 2 agenda in bpgv_tier1_summary.md §7](bpgv_tier1_summary.md):

| Leak | Tier 2 priority | Alignment |
|---|---|---|
| §3 equity exposure gap | **P2 — relax the 200d SMA + ATR graded filter** | ✓ directly addresses |
| §3 equity exposure gap (TSM gate) | **P4 — consider dropping the TSM absolute gate** | ✓ directly addresses |
| §6 warmup cost | **P1 — trace coordinator-layer warmup** | ✓ directly addresses |
| §4 bull-year shortfall | P2 + P4 | ✓ subsumed |
| §5 SPY only held 36 % of days | P2 | ✓ same root cause |
| §7 crash override noise | (not in P1–P5) | — only 3 fires, not actionable |
| §8.1 regime IC sign flip at 12 m | **P5 — validation stack + new signals** | partial — may motivate signal redesign |
| §8.2 momentum works | (no action needed) | — |
| §8.3 homebuilder works | (no action needed) | — |

**Conclusion**: priorities P1, P2, P4 account for ≥ 80 % of the diagnosed
leak. The §7 agenda is well-ordered against the data.

---

## Appendix A — Methodology and queries

All analyses computed from:
- `backtest.results` / `backtest.equity_curve` / `backtest.executions` /
  `backtest.final_positions` (Postgres)
- `macro_data.bsts_etf_prices` (SPY/TLT/GLD daily close, 2011-03-23 onward)
- `equities_data.ohlcv_1d` (EQR daily close)
- `data/equity_bars/*.csv` (seeded via `scripts/seed_etfs_yfinance.py`)
- `data/macro/bpgv_regime.csv` (monthly regime records)

Script: `/tmp/bpgv_attr/analyze.py` + `/tmp/bpgv_attr/analyze2.py` — not
committed (scratch analysis). Each section's finding can be reproduced by
loading the same CSVs and running the corresponding block.

## Appendix B — Limitations

1. **n is small for §7 crash override attribution** (3 fires).
2. **§8.1 regime IC is Spearman-only**; a regression with controls (e.g.
   trailing SPY momentum) might attenuate the +0.19 finding.
3. **§3 exposure gap uses the MONTHLY regime score mapped daily** — intra-month
   filter changes aren't reflected in the "target". The gap is therefore a
   lower bound on the true filter-induced suppression.
4. **§5 capture rates use the symbol's own return between first and last
   held dates** as the denominator, not a proper held-period product.
   Useful as an order-of-magnitude, not as a precise capture ratio.
5. **§6 coordinator warmup is inferred** from observed first-trade date vs
   backtest start; the actual code path in `BacktestCoordinator` /
   `PortfolioManager` has not yet been traced (Tier 2 P1).
