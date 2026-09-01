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
| **FILL PRICE** | The price a synthetic execution is booked at. | Historic misuse. **Eliminated** — `ExecutionManager` now takes prices explicitly. |

A basis is *what it cost*. A mark is *what it is worth*. Substituting one for the other
is the root defect; every rule below exists to enforce the distinction.

---

## 2. One live day, in order

Equity runner: `apps/strategies/live_equity_mean_reversion.cpp`.

| # | Step | Reads | Writes | Meaning in force |
|---|---|---|---|---|
| 1 | Load `previous_positions` from `trading.positions` | DB | — | COST BASIS (carried) |
| 2 | Corporate actions restate the carried book | `previous_positions` | `previous_positions` (in place, non-const ref) | COST BASIS |
| 3 | `LiveDailyCycle::prepare_strategy_for_signals` — seed `positions_`, then `on_data` | post-action `previous_positions` | strategy `positions_` | COST BASIS |
| 4 | Optimizer / risk produce day-T targets | — | `positions` | *(undefined — see §4)* |
| 5 | **Day-T placeholder** writes the T-1 close into `average_price` | `previous_day_close_prices` | `positions[*].average_price` | **MARK written into a basis field** |
| 6 | `LiveDailyCycle::execute_day_t` — resolve prices, generate fills, roll back the unpriceable | explicit price map | `positions` (rollback only) | FILL PRICE, from a **separate map** |
| 7 | `on_execution()` feeds fills back | executions | strategy `positions_` | COST BASIS |
| 8 | `LiveDailyCycle::resolve_and_apply_basis` | strategy + carried basis | `positions[*].average_price`, `unrealized_pnl` | COST BASIS restored |
| 9 | Persist | `positions` | `trading.positions` | COST BASIS |

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
| `execution_manager.cpp` (removed) | FILL PRICE | **Deleted** — was the root cause |

---

## 4. The rules

1. **A fill is priced from a real close, or it is not priced at all.**
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

---

## 5. Why seeding did not blow up risk

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

`execution_manager.cpp` is shared. Removing the fallback is reachable for futures.

**Not reachable** in the main agricultural-Monday case: the futures runners revert the
position to Friday's quantity, so `current_qty == prev_qty`, the delta check fails, and no
execution is generated — the fallback is never consulted. That logic is untouched.

**Reachable** in one narrow branch: agricultural symbol, Monday, no T-1 price, *and no
previous position* (`"No Sunday data and no previous position - keeping current"`). There
the delta is non-zero and an execution is generated. Previously it was priced at
`average_price`; now it is skipped with an ERROR.

**This is an improvement, not a regression** — that execution was being booked at a cost
basis, which for a new position is 0. Futures was silently exposed to the same
zero-price defect. But it *is* a behaviour change on the futures path, and the futures
runners were deliberately **not** given the widened lookup (equity-branch scope). Wiring
`ExecutionPriceResolver` into the futures runners would convert that skip into a correct
Friday-close fill and is the right follow-up — **merge-time, with a futures regression
run**, not on this branch.
