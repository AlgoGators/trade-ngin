# `Position::average_price`: lifecycle, meanings, and the rules that keep them apart

`average_price` carries more than one meaning, and code written against one meaning can read a
value written under another. This document maps the meanings and the rules that keep them apart.

---

## 1. The three meanings

| Meaning | What it is | Who is entitled to write it |
|---|---|---|
| **COST BASIS** | Volume-weighted price actually paid. The anchor for realized P&L, unrealized P&L, and the mean-reversion stop-loss. | `BaseStrategy::on_execution()`, the sole legitimate writer. Corporate actions restate it. |
| **MARK** | What the position is worth right now (a close). | Risk/margin read it this way. Nothing should *write* a mark here. |
| **FILL PRICE** | The price a synthetic execution is booked at. | Policy-dependent (`PricingPolicy`, `execution_manager.hpp`). `MARK_FALLBACK` (the parameter's default) reads `average_price`, a real price only where the field holds a mark, as `trend_following.cpp:880` stores one. Every runner passes `STRICT` (equities in `LiveDailyCycle::execute_day_t`, futures in `execute_strategy_day_strict`, `session_book_gate.hpp:394`), which never reads it: absent price ⇒ no fill. |

A basis is *what it cost*. A mark is *what it is worth*. Substituting one for the other
is the root defect; every rule below exists to enforce the distinction.

---

## 2. One live day, in order

Equity runner: `apps/strategies/live_equity_mean_reversion.cpp`. This table is the contract to read
before touching P&L code.

| # | Step | Reads | Writes | Meaning in force |
|---|---|---|---|---|
| 1 | Load T-1 rows; `split_open_and_closed`: qty-0 rows are parked in `previous_closed_rows` and never reach the held book | `trading.positions` | none | COST BASIS (carried) |
| 2 | Class-1 corporate actions (dividends/splits from per-bar columns) restate the held book. Gated three ways: price horizon, ex-date eligibility on basis provenance, and the refusal of a dedup row dated ≥ today. Placeholder rows persist **only when ≥1 adjustment applied**, with `realized_pnl = 0` | `previous_positions` | `previous_positions` in place; day-T placeholder + `corp_action_applied` in one transaction | COST BASIS |
| 3 | Class-2/3 lifecycle (renames, terminations), trading days only; a termination's `realized_delta` is accumulated per symbol | held book | `previous_positions`; placeholder rows when anything moved | COST BASIS |
| 4 | `prepare_strategy_for_signals` seeds `positions_` with **realized zeroed**; `PortfolioManager::process_market_data` then calls `on_data` once | post-action book | strategy `positions_` | COST BASIS |
| 5 | T-1 finalization from `select_finalization_book` (pre-action snapshot, or the restated book for a deferred event covering T-1); under `MARK_TO_MARKET` the finalizer keeps the row's trade realized and carries the last mark for an unprinted symbol; `restore_loaded_realized` re-asserts the loaded figure; dead rows dropped; closed rows re-appended; write refused if no T-1 prices | T-1/T-2 closes | `trading.positions` (T-1 date), fatal on failure | COST BASIS; `daily_realized_pnl` = T-1's own flow |
| 6 | Day-T placeholder writes the T-1 close into `average_price` | `previous_day_close_prices` | `positions[*].average_price` | **MARK in a basis field** |
| 7 | `LiveDailyCycle::execute_day_t` (STRICT): resolve prices (widened ≤5 days), generate fills, roll back the unpriceable | explicit price map | `positions` (rollback only) | FILL PRICE, from a **separate map** |
| 8 | `on_execution` for real fills only: corp-action exits are excluded; a rejected fill is fatal | executions | strategy `positions_` | COST BASIS |
| 9 | `resolve_and_apply_basis` (strategy → carried → 0, never a mark; copies the strategy's per-day realized onto the row); `add_rowless_exits`; corp-action realized added onto the terminated symbol's row | strategy + carried | `positions[*].average_price`, `unrealized_pnl`, `realized_pnl` | COST BASIS restored |
| 10 | Dead-row filter (`is_dead_row`: no qty **and** no realized); closed rows written with `average_price = 0`; today's rows cleared; the assertion `Σ rows == aggregate` within 1e-4, fatal; persist | `positions` | `trading.positions` (day T) | COST BASIS / 0 on closed rows |

On a weekend or holiday (including an explicit replay date that lands on one) steps 3, 4 and 7 are skipped, the held book is carried forward as the day's book (`LiveDailyCycle::carry_forward`), and step 8 has no fill to process. Steps 2, 5, 6, 9 and 10 still run: step 9 puts the carried basis back over the step 6 placeholder. The book, live_results and equity_curve are carried forward and the previous mark is reused.

**Steps 6 to 9 are the danger window.** Between them the day-T map's `average_price` holds a
*mark*. Anything reading it as a basis in that window reads a lie. Step 9 ends the window; it
needs step 8 to have run first, and it must run before any persistence or basis-dependent logic.

---

## 3. Every reader, and which meaning it assumes

| Site | Assumes | Safe? |
|---|---|---|
| `mean_reversion.cpp:408` (stop-loss) | COST BASIS | Yes: reads the strategy's own `positions_`, never the day-T map |
| `LivePnLManager::unrealized_from_cost_basis` | COST BASIS | Yes: guards `average_price <= 0` |
| `risk_manager.cpp:148,299,306` | MARK | Intended on the target map (see §5). The equity runner's metrics snapshot also reaches it with the day-T rows and no price map (`live_equity_mean_reversion.cpp:4442`), after step 9, so the printed leverage values a position at its cost basis |
| `margin_manager.cpp:30,165` | MARK | Yes on the equity path: the runner passes the T-1 close map (`live_equity_mean_reversion.cpp:4180`) and `average_price` is read only for a symbol absent from it |
| `execution_manager.cpp:51,90` under `MARK_FALLBACK` | FILL PRICE | Not reached by any runner: the equity and the futures runners all pass `STRICT` |

---

## 4. The rules

1. **On the equity path a fill is priced from a real close, or it is not priced at all** (`execute_day_t` passes `PricingPolicy::STRICT`). `MARK_FALLBACK`, the parameter's default, still books at `average_price` and no runner passes it; never call `generate_daily_executions` from a cost-basis strategy without `STRICT`.
   Under `STRICT` there is no fallback: an absent or non-positive price ⇒ ERROR, skip, and
   report the symbol, and `average_price` is not read.

2. **A missing T-1 close is not automatically fatal.**
   `ExecutionPriceResolver` substitutes the most recent *real* close within a staleness
   bound (`live.execution_price_max_staleness_days`, default 5: a three-day weekend plus
   a further holiday reaches Wednesday, the longest ordinary gap). Substitutions are
   logged with the date they came from. A present T-1 close always wins, so the normal
   path is unchanged.

3. **A symbol that could not be priced did not trade.**
   Its day-T target is rolled back to the carried quantity, or dropped if never held.
   Otherwise the runner persists a position no execution supports, a phantom that reads
   back next session as real.

4. **A basis comes from the strategy or the carried book. Never from a mark.**
   `resolve_day_t_cost_basis`: strategy → carried → unresolved.

5. **The residual is loud and inert, never silent.**
   Unresolved ⇒ ERROR naming the symbol, basis 0, unrealized 0.

6. **Marks and fills come from the same map.**
   `ExecutionOutcome::execution_prices` is returned and reused for marking, so P&L and
   executions cannot disagree about what a symbol was worth.

7. **A closed row carries no basis.** A position closed to zero on
   date D keeps a `trading.positions` row for D when it realized anything, so the exit's
   realized P&L has somewhere to live. That row is written with `average_price = 0`,
   `daily_unrealized_pnl = 0`, `quantity = 0`. `on_execution` leaves the *exit price* in
   `average_price` after a full close; persisting it would be a fourth meaning of the
   column (a price attached to a position that no longer exists), which is the category error
   this document prevents. Zero reads unambiguously as "closed".
   Closed rows are split out at load time (`LiveDailyCycle::split_open_and_closed`) and
   reach only the T-1 write set; no reader of the held book ever sees one.

---

## 5. Why seeding does not blow up risk

- `RiskManager` reads `average_price` as a mark on the **target map**: `MeanReversionStrategy::get_target_positions()` writes `current_price` there deliberately (`mean_reversion.cpp:230`), and the stored backtest `average_price` is a mark for the same reason. That is a live, intended reader; the seeded `positions_` are what must never reach it.

Seeding makes `positions_` non-empty in live. Risk and margin read `average_price` as a
**mark**, so a seeded book reaching them would be valued at cost. It is contained only because:

- `portfolio_manager.cpp:4204` `get_positions_internal()` has two callers, **both dead**:
  `:307` is documented in-tree as having no production consumer, and `:4129` sits inside
  `get_portfolio_value(const map&)`, which has no callers anywhere.
- `MeanReversionStrategy` **overrides** `get_target_positions()`, building from
  `inst_data.target_position`. The base returns `positions_`; had it not overridden,
  seeding would have become the day's targets directly.

**The containment rests on these two facts of the code.** With a live caller of
`get_positions_internal()`, or with a seeded strategy that does not override
`get_target_positions()`, risk reads cost bases as marks.

---

## 6. Traceability

The exceptions of the basis path each emit a `BASIS TRACE` line: a widened price, an unpriced
symbol, a rollback, an unresolved basis, and rows marked from the execution price map. A symbol
whose price and basis resolved on the normal path emits none at the runner's log level (INFO); its
inputs are logged at DEBUG only. The lines below are illustrative and abbreviated:

```
BASIS TRACE | price widened | THIN @ 2026-08-27 (2 days stale) (no T-1 close; used most recent real session)
BASIS TRACE | unpriced | DELISTED has no close within 5 days - it did not trade today
BASIS TRACE | rolled back | DELISTED day-T target discarded, book restored to carried quantity 10.000000 (unpriced, no execution); mark carried at ...
BASIS TRACE | inputs | AAPL strategy=150.250000 carried=148.100000            (DEBUG: not written at INFO)
BASIS TRACE | UNRESOLVED | ORPHAN holds a non-zero quantity with no cost basis ...
BASIS TRACE | row marks | 1 symbol(s) marked from the execution price map rather than the T-1 close map ...
```

`grep 'BASIS TRACE.*<SYMBOL>'` returns the exceptions recorded for one symbol on that day; an
empty result means its price and basis resolved on the normal path. The `row marks` line carries a
count, not a symbol.

---

## 7. What a run checks, and what it does not

A run enforces the rules above in three places:

- The staleness bound of rule 2 is a configuration value (`live.execution_price_max_staleness_days`,
  default 5). Each substitution writes one `price widened` line with the date the close came from.
- The rows of day T must sum to the day's aggregate. Realized P&L is checked within 1e-4 and a
  violation is fatal (step 10). Unrealized P&L is checked the same way on a trading day; on a closed
  day the aggregate is the previous row's stored figure, so a difference above 1e-2 is a warning.
- An unresolved basis is an ERROR line and is stored as basis 0 with unrealized 0 (rule 5).

A run does not count how many symbols of the configured universe lack a T-1 close or how often the
widened path is taken: the `price widened` and `unpriced` lines are the only record. A run does not
compare its `BASIS TRACE` lines with the stored `trading.positions` rows; the stored rows are
checked against the aggregate only.

---

## 8. Futures exposure

The futures runners price fills under `PricingPolicy::STRICT` as well
(`execute_strategy_day_strict`, `include/trade_ngin/live/session_book_gate.hpp:394`, reached through
its forecast-sign-close overload at `:436`, which
`apps/strategies/live_portfolio_conservative.cpp:2369` calls; `live_portfolio.cpp` carries the same
lines). The book gate holds every symbol whose T-1 bar is not a session at its stored quantity
before the execution step (`hold_non_session_symbols`, `session_book_gate.hpp:269`), so no changed
symbol reaches it unpriced; one that still does gets its stored row back, or is dropped if it was
not held, and is logged as an ERROR. `MARK_FALLBACK` remains the default argument of
`ExecutionManager::generate_daily_executions` (`include/trade_ngin/live/execution_manager.hpp:108`)
and no runner passes it. The futures runners do not use `ExecutionPriceResolver`.

## 8b. Transaction cost: what the model is told, and where its window ends

(This section is about the *cost* of a fill, not its basis. It lives here because cost is the
other thing the day-T executions carry into `trading.executions` and `live_results`, and because
the same "which frame, which window" question governs both. The model itself is in
`docs/COST_MODEL.md`; its section 5 holds the table of what feeds each cost manager.)

`TransactionCostManager::calculate_costs` needs two things the caller never passes it:
the symbol's **ADV**, read from `impact_model_.get_adv()`, and its **volatility
multiplier**, read from `spread_model_.get_volatility_multiplier()`
(`src/transaction_cost/transaction_cost_manager.cpp:110-111`). Both come only from
`update_market_data` (`:265`). Registering the tier config (`register_equity_costs_from_bars`,
`:325`) does **not** supply them: that writes the spread ticks and the impact caps, and nothing
else. A symbol the model has never been fed is priced against the fallbacks, `adv = 100000`
shares (`:117`) and `vol_mult = 1.0` (`:121`). The live equity runner therefore registers the
tier and feeds the model in one block, from the same bars
(`apps/strategies/live_equity_mean_reversion.cpp:995`, `:997`).

Two rules follow, and both are enforced by `LiveDailyCycle::feed_cost_model`
(`include/trade_ngin/live/live_daily_cycle.hpp:210`):

1. **Feed the model before you price a fill.** Same bars the tier is registered from, in
   date order, so the ADV that picks the tier and the ADV that scales the impact are the
   same twenty observations.
2. **The first bar of a symbol passes `prev_close = 0.0`, never its own close.**
   `update_market_data` records the volume unconditionally and gates the log return on
   `prev_close > 0` (`record_log_return`, `transaction_cost_manager.cpp:290`), so zero means
   "volume yes, return omitted". Passing the bar's own close, which is what `ExecutionManager`'s
   3-arg form does for an unseen symbol (`src/live/execution_manager.cpp:185`), injects a
   fabricated `log(close/close) = 0` return that pulls the sample stdev down and biases
   `vol_mult` low.

**The window ends at the signal bar in both the live run and the backtest.** A day-T fill is
priced at the T-1 close, and the cost model reads nothing later than that bar on either side.

| | Tier | ADV and returns |
|---|---|---|
| Live | `register_equity_costs_from_bars`, the last 20 loaded bars, which end at T-1 (`live_equity_mean_reversion.cpp:995`) | `feed_cost_model`, the same bars (`:997`) |
| Backtest | `EquityCostRetier::retier` on both cost managers, called before the cycle's own group is appended, so its 20 bars end at the previous group, T-1 (`src/backtest/backtest_coordinator.cpp:697-711`; `include/trade_ngin/backtest/equity_cost_retier.hpp:148`) | the signal group `portfolio_previous_bars_`, each bar's return taken against the group fed on the previous cycle (`backtest_coordinator.cpp:731-782`) |

The backtest applies rule 2 as well: a symbol with no bar in the previously fed group passes
`prev_close = 0.0` (`backtest_coordinator.cpp:744-780`).

What still differs between the two is the start of the window and the share unit, not its end:

- **The first cycles of a backtest.** Before the first cycle the tier comes from the 30 calendar
  days before the start date (`register_equity_cost_warmup`,
  `include/trade_ngin/backtest/equity_cost_warmup.hpp:83`), and the same bars seed the re-tier's
  trailing windows (`load_equity_cost_retier`, `backtest_coordinator.cpp:1845`, `:1908`). Those
  bars set tiers only. The ADV and the return windows start empty. The first cycle feeds nothing and
  each later cycle adds one volume, so `get_adv` averages fewer than twenty volumes until the
  twenty-first cycle
  (`ImpactModel::get_adv`, `src/transaction_cost/impact_model.cpp:75`). The live runner feeds
  every loaded bar on every run, so its windows are full.
- **The share unit.** The backtest's prices and quantities are adjusted to the end of the
  window, so it expresses each volume in that unit (`split_consistent_volume`,
  `equity_cost_retier.hpp:128`). The live runner feeds the stored volume. The two agree on
  every bar with no later split inside the backtest window; the header comment of
  `equity_cost_retier.hpp` states both frames.
