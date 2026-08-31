# Pre-E2 fix tracker — LIVE DOCUMENT

**Status: NOTHING IMPLEMENTED YET.** This is the working register for the pre-E2 wave.
Update the Status column as each item lands. Sources: `E2_READINESS_AUDIT_2026-08-31.md`
(34 findings), `E2_AUDIT_CORROBORATION_2026-08-31.md` (independent verification), plus
first-hand checks recorded here.

Branch `equities_integration` @ f64d6870 · suite 1431/1431 · 36 commits, nothing pushed.

---

## WAVE 1 — blocking, implement in this order

| ID | Finding | Sev | Status | Notes |
|---|---|---|---|---|
| FIX-0 | Results-manager key mismatch: `LiveTradingConfig::portfolio_id` defaults to `BASE_PORTFOLIO` (`live_trading_coordinator.hpp:30`); equity runner sets 5 fields but not that one (`:409-414`); `ResultsManagerBase` passes `strategy_id_` as BOTH id and name (`results_manager_base.cpp:87,122,158`). Read key `(LIVE_EQUITY_MEAN_REVERSION, EQUITY_MEAN_REVERSION, EQUITY_MR_PORTFOLIO)` vs write key `(…, LIVE_EQUITY_MEAN_REVERSION, BASE_PORTFOLIO)` — 2 of 3 columns differ | CRITICAL | ☐ TODO | Run 2 loads an empty book. Futures runner already sets this (`live_portfolio_conservative.cpp:637`) — restores a line equity omitted |
| FIX-0b (F-J) | `save_all_results` swallows per-table failures (ERROR + continue, returns success) — exit 0 ≠ all tables written | LOW | ☐ TODO | Fold into FIX-0, same file |
| FIX-1 | `apply_renames` re-keys currently-trading symbols. **META → METV (until 2022-01-31)** from our own backfill; META has 3,589 bars through 2026-08-28, METV has none → live position re-keyed onto a symbol with no prices | HIGH | ☐ TODO | **131** historical tickers still actively trading (corroboration corrected 130→131). Fix must handle the general case, not just META. L5 (±1-day `effective_until` convention) absorbed here |
| FIX-2 | Close top-up fetches exactly one day, never the range. Proven: whenever `deep_symbols` is non-empty, `w.start == w.deep_start` necessarily → `$2 == $3` → half-open SQL yields one day | HIGH | ☐ TODO | Silently breaks E2's >730-day scenario |
| FIX-3 | Dividend denominator frame mixing under stacked events (two distinct mechanisms: later-split deflation of adjusted closes, and raw top-up closes in the same map) | MEDIUM | ☐ TODO | L7 (`close_t_minus_1` misnomer) absorbed here |
| F-D | `MeanReversionInstrumentData::entry_price` never written → guard at `:1549` never passes → every persisted `trading.positions.unrealized_pnl` is 0 while `live_results.total_unrealized_pnl` is nonzero | MEDIUM | ☐ TODO | **Guaranteed log↔DB mismatch on E2 day 1.** Note: `entry_price` is vestigial — the stop-loss reads `Position::average_price`, not this field |

## WAVE 2 — F-B + F-E, approved pre-E2 (must land TOGETHER)

| ID | Finding | Sev | Status | Notes |
|---|---|---|---|---|
| F-B | Live never seeds the strategy's `positions_`, so `generate_signal` always takes the flat-book entry branch: a held long exits at z > −2.0 (entry threshold) instead of −0.5 (exit threshold), and the **5% stop-loss is dead code** | HIGH | ☐ TODO | **Verified 2026-08-31**: `BaseStrategy::seed_positions()` (`base_strategy.cpp:344`) is generic — its own doc says "Required in live … backtest doesn't need it because state is continuous in-memory". Futures calls it (`live_portfolio_conservative.cpp:906`); equities never does. It supplies BOTH fields MR needs: `quantity` (entry/exit branch) and `average_price` (stop-loss, `mean_reversion.cpp:391`). NOT futures machinery misapplied |
| F-E | Untraded holdings' `average_price` reset to the T-1 close daily (`:1300-1308`); `on_execution` corrects only symbols that traded | MEDIUM | ☐ TODO | **MUST land with F-B.** Seeding restores `average_price`; if F-E is unfixed, the re-enabled stop-loss compares against yesterday's close instead of the true entry basis — the fix would be worse than the bug |

## WAVE 2b — housekeeping (HD-approved)

| ID | Item | Status | Notes |
|---|---|---|---|
| L10 | Doc drift: `CORP_ACTIONS_DATA_BOUNDARY.md:22` says "16 curated rows" (now 389); START_HERE commit count stale | ☐ TODO | Docs-only commit |
| L11 | `trend_following_fast.{hpp,cpp}.backup` | ☑ KEEP | HD decision: untracked, kept for reference |
| F-C | `order_id` localtime ALSO affects equities (same `ExecutionManager`). Failure needs two runs **straddling 20:00 EDT**: run 1 at 19:00 stores `execution_time`=Mon UTC; re-run at 21:00 asks `DATE(execution_time)=Tue` → run 1's rows never matched → duplicate executions | ☐ RUNBOOK | **E2 mitigation: run before 20:00 EDT.** Durable fix (recommended: drop the date predicate — `order_id` already embeds the date, making it redundant) stays with the deferred order_id item |

## Corrections to earlier claims (recorded so they are not repeated)

- "Re-pollutes the futures namespace" **overstates** FIX-0: `store_positions`' DELETE is
  also scoped by `strategy_id`, and `LIVE_EQUITY_MEAN_REVERSION` cannot collide with
  `LIVE_TREND_*`. Contamination, not corruption. CRITICAL stands on the empty-book
  consequence alone.
- FIX-0's "futures depend on base defaults" caution is **over-specified**: futures never
  populates the setters, so `save_positions_snapshot` returns early. Verified: **0 of
  3,781** position rows and **0 of 644** executions carry the save_all key shape. The
  futures runner has a comment documenting this very defect and deliberately avoiding it.
- **G-1 is NOT an active corruption.** Every 2026 futures row has `unrealized_pnl = 0`
  (1,176 rows); the 246 non-zero rows are a closed historical window (2025-05-19 →
  2025-11-11). Latent inconsistency for the merge gate, not a live problem. HD's proposed
  fix (gate on asset class: futures 0/realized, equities mark-to-market) is correct.
- `order_id` defect is **not futures-only** — START_HERE §6's scope claim is wrong.
- Change-ledger claim "only A1–A8 and D12 touch shared/futures paths" is true **only of
  the Aug 27–31 window**, not the whole branch (see G-series).
- Audit line references have drifted; re-derive at implementation time.

## Deferred — already slotted into existing stages (NOT new scope)

**E3:** F-F, F-H, F-I (T-OR.4), G-4 (SEC/TAF fees dead — gated on `quantity < 0`, callers
pass absolute), G-7 (borrow fees skip weekends, ~28% undercount), L3, L4, L8, L9, L14.
**E4:** G-8 (margin default drops ×qty). **E5:** F-G (T-1 queries lack portfolio_id).
**Merge-gate/futures:** G-1, G-2, G-3, G-5, G-6, L1, L2, L13.
**Main batch-3:** L1, L2, L6, L12 (incl. no-postgres-in-CI).

## Verification bar for this wave

Every fix needs a test that provably fails before it. FIX-0 and F-B/F-E additionally need
DB-level verification (read back what actually landed, per the log↔DB rule). No runner
executes until Wave 1+2 are green. Suite must not drop below 1431.

---

## E2 SCOPE — expanded (HD directive, 2026-08-31)

E2 verifies **both asset classes**, each with the full bar: inline logs reconciled against
**every** table written, per-action sanity checks, performance observed, and baseline
comparison where a baseline exists. Runners strictly sequential throughout.

### E2-A — futures (regression: nothing may have moved)
Full run + log↔DB reconciliation + performance. Baseline exists (integration-gate window
2024-06-25 → 2026-06-25, pre-dates the 08-06 feed death), so this is a true before/after.
Expected: **identical**. Any delta must be attributed or is a blocker.

### E2-B — equities (first execution ever of this path)
No live baseline exists — the first run *establishes* it. Two sequential runs minimum:
run 1 creates the book, run 2 is the first that can exercise corporate actions at all
(run 1 skips the block by its own `!previous_positions.empty()` guard).

### E2-C — mean-reversion behaviour-change sanity checks (because of F-B seeding)

Seeding deliberately changes live exit behaviour. These bound the change to exactly what
was intended and no more:

1. **Backtest must be UNCHANGED.** The backtest never used seeding — `positions_`
   accumulates in-process. Re-run the equity backtest before/after the F-B change:
   **byte-identical output required.** A backtest delta means the fix leaked somewhere it
   does not belong. This is the strongest single guard.
2. **Day 1 (empty book) must be inert.** With nothing to seed, seeded and unseeded runs
   must produce identical signals, positions and executions. Proves the change cannot
   affect a flat book.
3. **Day 2+ differences must be confined to HELD symbols.** Any symbol that was flat at
   the start of the day must produce the same signal as before. A flat symbol changing
   behaviour means seeding altered the entry branch, which it must not.
4. **Exit threshold now governs exits.** A held long must exit at `z > −exit_threshold`
   (−0.5), not at `z > −entry_threshold` (−2.0). Capture the z-score at each exit and
   confirm which threshold it corresponds to. This is the intended change, and it must be
   observed rather than assumed.
5. **Stop-loss must be reachable.** Confirm the 5% stop can fire — via a seeded scenario
   if the replay window contains no natural trigger. It has been dead code; "no stop-loss
   fired" is not evidence that it works.
6. **Stop-loss must fire against the TRUE entry basis, not the T-1 close.** This is the
   F-E coupling: verify `average_price` on a held-but-untraded position does not drift
   day over day. If it drifts, F-E is not fixed and the stop-loss is measuring from the
   wrong reference.
7. **Live↔backtest parity should IMPROVE.** Before F-B they diverge structurally (live
   always takes the flat-book branch). After, the same window should converge. Quantify
   both — a parity check that was meaningless before should now have content. This is
   T-OR.5 strengthened from construction-drift to genuine runner-path parity.
8. **Turnover and holding periods should move toward backtest expectations.** Live
   previously exited early, so it over-traded. Compare trade counts and average holding
   period against the backtest for the same window; the gap should narrow, not widen.

### E2-D — mandatory observations (from earlier, still in force)
- `find_previous_trading_day` resolved date, checked against weekend/holiday/last-bar.
- Corp-action window start must report its DERIVATION RULE; a 14-day floor reported while
  positions are held is a contradiction to investigate.
- Runs scheduled **before 20:00 EDT** (F-C order_id boundary).

### Sequence agreed
docs (this) → Wave 1 → Wave 2 → **independent confirmation agent** → E2. No runner
executes before that confirmation passes.
