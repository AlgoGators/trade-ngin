# `Position::average_price` — lifecycle, meanings, and the rules that keep them apart

**Status:** current as of the E1 residual fix (2026-08-31). Branch `equities_integration`.

This document exists because four separate defects — F-D, F-E, the Wave 2 residual, and
the zero-price chain — were all the same underlying mistake: **`average_price` carries
more than one meaning, and code written against one meaning read a value written under
another.** Mapping it out is what stops the next one.

---

## 1. The three meanings

| Meaning | What it is | Who is entitled to write it |
|---|---|---|
| **COST BASIS** | Volume-weighted price actually paid. The anchor for realized P&L, unrealized P&L, and the mean-reversion stop-loss. | `BaseStrategy::on_execution()` — sole legitimate writer. Corporate actions restate it. |
| **MARK** | What the position is worth right now (a close). | Risk/margin read it this way. Nothing should *write* a mark here. |
| **FILL PRICE** | The price a synthetic execution is booked at. | Policy-dependent (`PricingPolicy`, `execution_manager.hpp`). `MARK_FALLBACK` (default, futures) still reads `average_price` — correct there because `trend_following.cpp:623` stores a mark in it. `STRICT` (equities, `LiveDailyCycle::execute_day_t`) never reads it: absent price ⇒ no fill. |

A basis is *what it cost*. A mark is *what it is worth*. Substituting one for the other
is the root defect; every rule below exists to enforce the distinction.

---

## 2. One live day, in order

Equity runner: `apps/strategies/live_equity_mean_reversion.cpp`.

| # | Step | Reads | Writes | Meaning in force |
|---|---|---|---|---|
| 1 | Load T-1 rows; `split_open_and_closed` — qty-0 rows are parked in `previous_closed_rows` and never reach the held book | `trading.positions` | — | COST BASIS (carried) |
| 2 | Class-1 corporate actions (dividends/splits from per-bar columns) restate the held book. Gated three ways: price horizon (E2-F15), ex-date eligibility on basis provenance (E2-F17), and the F23 detector (refuse a dedup row dated ≥ today). Placeholder rows persist **only when ≥1 adjustment applied**, with `realized_pnl = 0` | `previous_positions` | `previous_positions` in place; day-T placeholder + `corp_action_applied` in one transaction | COST BASIS |
| 3 | Class-2/3 lifecycle (renames, terminations) — trading days only (F21); a termination's `realized_delta` is accumulated per symbol | held book | `previous_positions`; placeholder rows when anything moved | COST BASIS |
| 4 | `prepare_strategy_for_signals`: seed `positions_` with **realized zeroed**, then `on_data` | post-action book | strategy `positions_` | COST BASIS |
| 5 | T-1 finalization from `select_finalization_book` (pre-action snapshot, or the restated book for a deferred event covering T-1); under `MARK_TO_MARKET` the finalizer keeps the row's trade realized and carries the last mark for an unprinted symbol; `restore_loaded_realized` re-asserts the loaded figure; dead rows dropped; closed rows re-appended; write refused if no T-1 prices | T-1/T-2 closes | `trading.positions` (T-1 date), fatal on failure | COST BASIS; `daily_realized_pnl` = T-1's own flow |
| 6 | Day-T placeholder writes the T-1 close into `average_price` | `previous_day_close_prices` | `positions[*].average_price` | **MARK in a basis field** |
| 7 | `LiveDailyCycle::execute_day_t` (STRICT) — resolve prices (widened ≤5 days), generate fills, roll back the unpriceable | explicit price map | `positions` (rollback only) | FILL PRICE, from a **separate map** |
| 8 | `on_execution` for real fills only — corp-action exits are excluded (E2-F11); a rejected fill is fatal | executions | strategy `positions_` | COST BASIS |
| 9 | `resolve_and_apply_basis` (strategy → carried → 0, never a mark; copies the strategy's per-day realized onto the row); `add_rowless_exits`; corp-action realized added onto the terminated symbol's row | strategy + carried | `positions[*].average_price`, `unrealized_pnl`, `realized_pnl` | COST BASIS restored |
| 10 | Dead-row filter (`is_dead_row`: no qty **and** no realized); closed rows written with `average_price = 0`; today's rows cleared (R-3); L5 assert `Σ rows == aggregate` within 1e-4, fatal; persist | `positions` | `trading.positions` (day T) | COST BASIS / 0 on closed rows |

On a weekend or holiday (including an explicit replay date that lands on one) steps 2-3 and 7-9 are skipped; the book, live_results and equity_curve are carried forward and the previous mark is reused.

**Steps 5 → 8 are the danger window.** Between them `average_price` holds a *mark*.
Anything reading it as a basis in that window reads a lie. Step 8 is what ends the
window, and it must run before any persistence or basis-dependent logic.

---

## 3. Every reader, and which meaning it assumes

| Site | Assumes | Safe? |
|---|---|---|
| `mean_reversion.cpp:391` (stop-loss) | COST BASIS | Yes — reads the strategy's own `positions_`, never the day-T map |
| `LivePnLManager::unrealized_from_cost_basis` | COST BASIS | Yes — guards `average_price <= 0` |
| `risk_manager.cpp:64,236,243` | MARK | Reachable only via dead callers (see §5) |
| `margin_manager.cpp:30,165` | MARK | Same |
| `execution_manager.cpp` under `MARK_FALLBACK` | FILL PRICE | Live for futures (mark in the field); bypassed on the equity path by `STRICT` |

---

## 4. The rules

1. **On the equity path a fill is priced from a real close, or it is not priced at all** (`execute_day_t` passes `PricingPolicy::STRICT`). `MARK_FALLBACK` — the default, kept for futures where `average_price` is a mark — still books at `average_price`; never call `generate_daily_executions` from a cost-basis strategy without `STRICT`.
   `ExecutionManager` has no fallback. Absent or non-positive price ⇒ ERROR, skip, and
   report the symbol. It never reads `average_price`.

2. **A missing T-1 close is not automatically fatal.**
   `ExecutionPriceResolver` substitutes the most recent *real* close within a staleness
   bound (`live.execution_price_max_staleness_days`, default 5 — a three-day weekend plus
   a further holiday reaches Wednesday, the longest ordinary gap). Substitutions are
   logged with the date they came from. A present T-1 close always wins, so the normal
   path is unchanged.

3. **A symbol that could not be priced did not trade.**
   Its day-T target is rolled back to the carried quantity, or dropped if never held.
   Otherwise the runner persists a position no execution supports — a phantom that reads
   back next session as real.

4. **A basis comes from the strategy or the carried book. Never from a mark.**
   `resolve_day_t_cost_basis`: strategy → carried → unresolved.

5. **The residual is loud and inert, never silent.**
   Unresolved ⇒ ERROR naming the symbol, basis 0, unrealized 0. Previously the guard
   `if (cost_basis > 0.0)` *skipped the write*, leaving step 5's mark in place — so the
   one path that could not find a basis was the one path that substituted a mark for it.

6. **Marks and fills come from the same map.**
   `ExecutionOutcome::execution_prices` is returned and reused for marking, so P&L and
   executions cannot disagree about what a symbol was worth.

5. **A closed row carries no basis.** (E2-F19, 2026-09-02.) A position closed to zero on
   date D keeps a `trading.positions` row for D when it realized anything, so the exit's
   realized P&L has somewhere to live. That row is written with `average_price = 0`,
   `daily_unrealized_pnl = 0`, `quantity = 0`. `on_execution` leaves the *exit price* in
   `average_price` after a full close; persisting it would be a fourth meaning of the
   column — a price attached to a position that no longer exists — which is exactly the
   category error this document exists to prevent. Zero reads unambiguously as "closed".
   Closed rows are split out at load time (`LiveDailyCycle::split_open_and_closed`) and
   reach only the T-1 write set; no reader of the held book ever sees one.

---

## 5. Why seeding did not blow up risk

- Note (2026-09-03): `RiskManager` DOES read `average_price` as a mark on the **target map** — `MeanReversionStrategy::get_target_positions()` writes `current_price` there deliberately (`mean_reversion.cpp:213`), and `e566bbf9` kept the stored backtest `average_price` a mark for the same reason. That is a live, intended reader; the seeded `positions_` are what must never reach it.

Seeding (F-B) makes `positions_` non-empty in live where it was *always* empty. Risk and
margin read `average_price` as a **mark**, so this was live blast radius. It is contained
only because:

- `portfolio_manager.cpp:1579` `get_positions_internal()` has two callers, **both dead** —
  `:195` is documented in-tree as having no production consumer, and `:1509` sits inside
  `get_portfolio_value(const map&)`, which has no callers anywhere.
- `MeanReversionStrategy` **overrides** `get_target_positions()`, building from
  `inst_data.target_position`. The base returns `positions_`; had it not overridden,
  seeding would have become the day's targets directly.

**This is a narrow escape resting on dead code.** If either caller is revived, or a
strategy without that override is seeded, risk will start reading cost bases as marks.
Pin this before adding a seeded strategy.

---

## 6. Traceability

Every basis decision emits a `BASIS TRACE` line, so one symbol can be followed end to end:

```
BASIS TRACE | price widened | THIN @ 2026-08-27 (2 days stale) (no T-1 close; used most recent real session)
BASIS TRACE | unpriced      | DELISTED has no close within 5 days - it did not trade today
BASIS TRACE | rolled back   | DELISTED day-T target discarded, book restored to carried quantity 10.0
BASIS TRACE | inputs        | AAPL strategy=150.25 carried=148.10
BASIS TRACE | resolved      | AAPL basis=150.25 from strategy(on_execution)
BASIS TRACE | UNRESOLVED    | ORPHAN holds a non-zero quantity with no cost basis ...
```

`grep 'BASIS TRACE.*<SYMBOL>'` reconstructs one symbol's full day.

---

## 7. What only a real run can settle

Unit tests fix the *shape* of these rules. They do not prove the following, which belong
to E2:

- Whether any symbol in the 852-name universe actually lacks a T-1 close, and how often.
- Whether 5 days is the right bound against the real equity calendar.
- Whether the widened path fires on real data at a plausible rate (silence would suggest
  it is not wired; constant firing would suggest a feed problem).
- Whether `BASIS TRACE` reconciles against `trading.positions` row-for-row.
- Whether the futures agricultural-Monday path (§8) ever reaches the skip in production.

---

## 8. Futures exposure

None by construction (corrected 2026-09-03). `edc02dee` restored the fallback behind
`PricingPolicy::MARK_FALLBACK`, which is the default and what both futures runners use, pinned by
three futures-preservation tests. The agricultural-Monday branch is priced at the mark exactly as
before; nothing on the futures path skips with an ERROR. Wiring `ExecutionPriceResolver` into the
futures runners remains a merge-time change with a futures regression run.

## 8b. Transaction cost: what the model is told, and the one offset that survives

(Added by D-4-FIX, 2026-09-04, for E2-F62. This section is about the *cost* of a fill, not
its basis — it lives here because it is the other thing the day-T executions carry into
`trading.executions` and `live_results`, and because the same "which frame / which window"
mistake produced both.)

`TransactionCostManager::calculate_costs` needs two things the caller never passes it:
the symbol's **ADV**, read from `impact_model_.get_adv()`, and its **volatility
multiplier**, read from `spread_model_.get_volatility_multiplier()`
(`transaction_cost_manager.cpp:26-27`). Both come only from `update_market_data`.
Registering the tier config (`register_equity_costs_from_bars`) does **not** supply them —
that writes the spread ticks and the impact caps, and nothing else. Until D-4-FIX the live
equity runner registered the tier and never fed the model, so every live equity fill was
priced against the fallbacks: `adv = 100000` shares (`:33`) and `vol_mult = 1.0` (`:37`).

Two rules follow, and both are enforced by `LiveDailyCycle::feed_cost_model`:

1. **Feed the model before you price a fill.** Same bars the tier is registered from, in
   date order, so the ADV that picks the tier and the ADV that scales the impact are the
   same twenty observations.
2. **The first bar of a symbol passes `prev_close = 0.0`, never its own close.**
   `update_market_data` records the volume unconditionally and gates the log return on
   `prev_close > 0`, so zero means "volume yes, return omitted". Passing the bar's own
   close — what `ExecutionManager`'s 3-arg form does for an unseen symbol
   (`execution_manager.cpp:186`) — injects a fabricated `log(close/close) = 0` return that
   pulls the sample stdev down and biases `vol_mult` low.

**The permanent 1-bar offset (by construction, not a defect).** The backtest feeds day T's
bar to the cost model and then costs executions that fill at T-1's close
(`backtest_coordinator.cpp:568` runs ahead of the costing loop), so its 20-bar window ends
at **T**. The live runner has bars only through T-1 — asking for T would be lookahead — so
its window ends at **T-1**. The two cost models therefore average volumes over windows
offset by one bar, forever. Measured on the `equity_mr` universe that is ~0.2-2 % of ADV
and moves `vol_mult` in its third decimal; it cannot change an ADV tier except for a symbol
sitting on a bucket boundary. It is the price of not looking ahead, and after D-4-FIX it is
the only backtest/live transaction-cost difference that remains.

## 9. Status

Rewritten 2026-09-03 after the E2-F19..F24 fix batch (`ff10ded8..6401e2ed`) and the doc staleness
audit in `docs/equities/final_finishing/audits/DOCS_STALENESS_AUDIT.md`. The live-day table above
is the contract every session must read before touching P&L code.
