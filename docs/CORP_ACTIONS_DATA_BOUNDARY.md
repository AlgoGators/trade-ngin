# Corporate actions: what we can trust, and over which dates

Current-state reference for where the equity runner reads each class of corporate action and
what each source covers.

Corporate actions are handled by **mechanical effect**, not by the vendor's 19
labels, because each effect has a different data source with a different
coverage window. The taxonomy lives in
`include/trade_ngin/live/corporate_actions_classification.hpp`; this document
records what each class's source can and cannot tell us today.

For the wider question of which table and column any equity path should read,
including why the vendor `adj_*` columns must never be read back in, and why
`corporate_action` is frozen-but-needed while `sharadar_ohlcv_1d` is genuinely
superseded, see `DATA_SOURCES_OF_TRUTH.md`.

## Summary

| Class | Vendor labels | Source | Trustworthy over | Handler |
|---|---|---|---|---|
| **PRICE_RESTATING** | split, adrratiosplit, spinoff, spinoffdividend, dividend | `equities_data.ohlcv_1d.div_cash` / `.split_factor` (per bar) | **Complete and current**: full history through the latest bar | `CorporateActionsApplier` |
| **SERIES_CONTINUITY** | tickerchangefrom, tickerchangeto | `equities_data.ticker_aliases` | **Partial**: 389 rows, 16 curated and 373 backfilled from the rename history, not the whole 12,867-row rename history | `CorporateActionsLifecycle::apply_renames` |
| **TERMINATION** | mergerfrom/to, acquisitionby/of, delisted, voluntarydelisting, regulatorydelisting, bankruptcyliquidation, spunofffrom | timing: `ohlcv_1d.delisting_date`; terms: `equities_data.corporate_action` | timing **current** (written on the symbol's bars); terms **frozen after 2025-08-29** (no termination event is later) | `CorporateActionsLifecycle::apply_terminations` |
| **INFORMATIONAL** | listed, relation, initiated | none | n/a | none: no position effect |

## Why price-restating events do not read `corporate_action`

`equities_data.corporate_action` holds no price-restating or termination event
after **2025-08-29** (its only later rows are one rename pair, `SBDS`, dated
2027-07-18), so a query of it for those events over a recent window returns
nothing. Price-restating events are therefore
read from the per-bar columns, which are written on the ex-date bar and never
restated; applied in order they reproduce the vendor's adjusted series up to
rounding, on the bars for which that series is current.

It also keeps PRICE_RESTATING independent of the vendor's derived `adj_*` columns: those
need a restating job to rewrite history after every event, and that job can stall. `div_cash` and
`split_factor` are facts about one bar and need no maintenance.

## What the frozen feed still costs us

Only **TERMINATION deal terms**: the `contraticker` and ratio that say what a
holding *became*. Concretely, for an event after 2025-08-29:

- **Cash merger / plain delisting**: no loss. The position exits at the final
  close, which is what a cash deal pays.
- **Stock-for-stock merger**: approximated. The true path rolls the holding
  into the acquirer; we exit at the final close instead. Returns up to the
  event are correct; the post-event path is not modelled.

Every such fallback emits a WARN naming the symbol, the event, and the gap, so
these are visible in the run log rather than silent.

**No price or return is affected by the frozen feed**, because price-restating
events do not come from it.

## What changes when the feed revives

Nothing in the code. `apply_terminations()` already takes `contra_ticker` and
`ratio` and rolls the position over at a basis-preserving ratio when they are
present; the query that populates them runs today against the full vendor
schema and simply returns no rows. Pinned by
`CorpActionClass3.RevivedFeedActivatesTheRolloverPathWithNoCodeChange`.

To restore full coverage the data owner needs to provide a live source of deal
terms and backfill from 2025-08-29 to the present (tracked as AlgoGators/data-ngin
issue #108, the corporate-actions data issue).

## Dividend cash is never double-counted

Equity prices are total-return adjusted, so a dividend is already in
mark-to-market P&L via price continuity. `total_dividend_income` is therefore
**informational only** and must never be added to P&L totals. Two tests pin
this from both directions: the applier books a dividend as a basis rescale and
does not touch `realized_pnl`
(`CorpActionClass1.DividendCashIsRecordedButNeverAddedToPositionPnl`), and the
metrics path keeps the income figure out of `total_pnl`.

## De-duplication: which events are already in the book

Equity-only. The futures runners carry no corporate actions and do not use this table. For how
a run is dated and which rows it reads, see `LIVE_RUN_CYCLE.md`.

`trading.corp_action_applied` (`migrations/002_corp_action_applied.sql:51`, extended by
`migrations/005_corp_action_applied_run_date.sql:86` and
`migrations/006_corp_action_applied_basis_ratio.sql:85`) is the durable record of which events
have already changed the book. `CorporateActionsAuditLog` (`src/live/corporate_actions_audit_log.cpp`) is its only reader and writer.

| Column | Meaning |
|---|---|
| `portfolio_id`, `strategy_id`, `strategy_name` | which book; supplied by the `WHERE` on load |
| `symbol`, `action_type`, `ex_date` | the in-memory key; together with the three above they form the primary key |
| `run_date` | the run date of the pass that wrote the row; NULL on rows older than the column |
| `basis_ratio` | the factor the event divided the cost basis by (`PositionAdjustment.ratio_change`); NULL for a TERMINATION, which restates no basis (`src/live/corporate_actions_audit_log.cpp:350-352`) |
| `applied_at`, `qty_held`, `dividend_per_share`, `total_cash` | audit detail; dividend cash is informational and never added to P&L |

**A row means "this event's effect is in the stored position."** Rows are recorded by
`record()` (`src/live/corporate_actions_audit_log.cpp:333`) only when the applier actually adjusts a position, and are inserted
with `ON CONFLICT DO NOTHING` (`store_applied_corp_actions_in`,
`src/data/postgres_database.cpp:3547-3548`), so the first
application is the one kept. A deferred event (its ex-date bar is not yet in the loaded
window, `apps/strategies/live_equity_mean_reversion.cpp:2503-2518`) or a refused event writes nothing, so it resurfaces in the next run's
window and is retried. `load()` (`src/live/corporate_actions_audit_log.cpp:90`) also mirrors every row under the symbol's current
ticker through `equities_data.ticker_aliases` (`:197`), so an event applied under an old name
is still seen as applied after a rename. A de-duplication record that cannot be read (the
table or the alias map) is an error and the run exits 1 rather than adjusting against an empty
applied-set (`apps/strategies/live_equity_mean_reversion.cpp:1476-1491`).

**The event window.** The runner asks for events over `[window_start, today]`, with
`window_start` derived from position inception rather than a fixed number of days
(`derive_corp_action_window`, `include/trade_ngin/live/corp_action_window.hpp:70`, called at
`apps/strategies/live_equity_mean_reversion.cpp:1407`), so a missed run does not lose events; the de-duplication rows are what stop the
reach-back from re-applying them.

**Two replay guards.** Both exist because a row from a *later* pass would make the current
pass skip an event whose effect is not in the previous-day row it just loaded.

1. Inline, after load: if `latest_applied_ex_date() >= today`, the runner exits 1
   (`apps/strategies/live_equity_mean_reversion.cpp:1503-1512`). A row with today's or a future ex-date can only have been written by a
   later pass.
2. On load, with `RunDateCheck::Enforce` (the default after `set_run_date(today)`,
   `apps/strategies/live_equity_mean_reversion.cpp:1474-1475`): any row whose `run_date >= today` is reported and `load()` returns an
   error (`src/live/corporate_actions_audit_log.cpp:141-145`), so the runner exits 1. This catches the case the first guard cannot
   see, where the later pass applied an event whose ex-date is already in the past. NULL
   `run_date` rows are accepted. The lifecycle (termination) instance later in the same run
   uses `RunDateCheck::StampOnly` (`apps/strategies/live_equity_mean_reversion.cpp:3442-3443`), because the rows it would otherwise refuse
   are this run's own.

**Reset rule.** Neither guard deletes anything. A re-run of a day is valid only when the book
and the de-duplication rows come from the same pass, so a replay from date D resets
`trading.corp_action_applied` for the portfolio **together with** `positions`, `live_results`,
`equity_curve` and `executions` from D forward, then replays in order. Resetting one without
the other either double-applies (rows gone, book adjusted) or skips (rows kept, book reset).
Never re-run a day after a later day has run.

**One transaction.** The adjusted positions and the rows that stop those events being applied
again commit as one unit of work: `store_positions` and `save_in` share a transaction, for the
price-restating events (`apps/strategies/live_equity_mean_reversion.cpp:3053-3077`) and again for the lifecycle events (`apps/strategies/live_equity_mean_reversion.cpp:3477-3504`).
Either both land or neither does; a failure in either rolls both back and the run exits 1.

## Verification queries

Run read-only against the database the equity runner reads:

```sql
-- PRICE_RESTATING is alive: recent per-bar events.
SELECT count(*) FILTER (WHERE div_cash <> 0)        AS dividends,
       count(*) FILTER (WHERE split_factor NOT IN (0,1)) AS splits
FROM equities_data.ohlcv_1d WHERE time >= now() - interval '60 days';

-- TERMINATION terms are frozen: latest event per label (2025-08-29 or earlier for every
-- label but the two rename labels, which show the one SBDS pair dated 2027-07-18).
SELECT action, max(date) FROM equities_data.corporate_action GROUP BY action ORDER BY 2 DESC;

-- SERIES_CONTINUITY coverage is thin.
SELECT count(*) FROM equities_data.ticker_aliases;                 -- 389
SELECT count(*) FROM equities_data.corporate_action
 WHERE action = 'tickerchangeto';                                  -- 12,867
```
