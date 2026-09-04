# Corporate actions: what we can trust, and over which dates

Companion to the Phase 4.3 effect-class rework. Verified against
`new_algo_data` on 2026-08-31.

Corporate actions are handled by **mechanical effect**, not by the vendor's 19
labels, because each effect has a different data source with a different
coverage window. The taxonomy lives in
`include/trade_ngin/live/corporate_actions_classification.hpp`; this document
records what each class's source can and cannot tell us today.

For the wider question of which table and column any equity path should read --
including why the vendor `adj_*` columns must never be read back in, and why
`corporate_action` is frozen-but-needed while `sharadar_ohlcv_1d` is genuinely
superseded -- see `DATA_SOURCES_OF_TRUTH.md`.

## Summary

| Class | Vendor labels | Source | Trustworthy over | Handler |
|---|---|---|---|---|
| **PRICE_RESTATING** | split, adrratiosplit, spinoff, spinoffdividend, dividend | `equities_data.ohlcv_1d.div_cash` / `.split_factor` (per bar) | **Complete and current** — full history through the latest bar | `CorporateActionsApplier` |
| **SERIES_CONTINUITY** | tickerchangefrom, tickerchangeto | `equities_data.ticker_aliases` | **Partial** — 16 curated rows, not the 12,867-row rename history | `CorporateActionsLifecycle::apply_renames` |
| **TERMINATION** | mergerfrom/to, acquisitionby/of, delisted, voluntarydelisting, regulatorydelisting, bankruptcyliquidation, spunofffrom | timing: `ohlcv_1d.delisting_date`; terms: `equities_data.corporate_action` | timing **current** (152 symbols, latest 2026-04-09); terms **frozen after 2025-08-29** | `CorporateActionsLifecycle::apply_terminations` |
| **INFORMATIONAL** | listed, relation, initiated | — | n/a | none — no position effect |

## Why price-restating events moved off `corporate_action`

`equities_data.corporate_action` last received an event on **2025-08-29**. The
live applier queried it over a 14-day window, so from the day it shipped it has
been reading an empty result set: splits and dividends were never applied to
live positions. Phase 4.3 re-sources class 1 from the per-bar columns, which
are written on the ex-date bar and never restated — the same primitives Phase
4.2 validated reproduce the vendor's adjusted series to 9.5e-13 relative.

This also makes class 1 immune to the failure that took out the vendor's
derived `adj_*` columns on 2026-08-06: those need a restating job to rewrite
history after every event, and that job can stall. `div_cash` and
`split_factor` are facts about one bar and need no maintenance.

## What the frozen feed still costs us

Only **TERMINATION deal terms** — the `contraticker` and ratio that say what a
holding *became*. Concretely, for an event after 2025-08-29:

- **Cash merger / plain delisting** — no loss. The position exits at the final
  close, which is what a cash deal pays.
- **Stock-for-stock merger** — approximated. The true path rolls the holding
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

To restore full coverage the data owner needs to revive
`equities_data.corporate_action` and backfill 2025-08-29 → present (tracked as
item 3 in `docs/DATA_OWNER_ASKS_2026-08.md`).

## Dividend cash is never double-counted

Equity prices are total-return adjusted, so a dividend is already in
mark-to-market P&L via price continuity. `total_dividend_income` is therefore
**informational only** and must never be added to P&L totals. Two tests pin
this from both directions: the applier books a dividend as a basis rescale and
does not touch `realized_pnl`
(`CorpActionClass1.DividendCashIsRecordedButNeverAddedToPositionPnl`), and the
metrics path keeps the income figure out of `total_pnl` (Phase 4.2).

## Verification queries

Run read-only against `new_algo_data`:

```sql
-- Class 1 is alive: recent per-bar events.
SELECT count(*) FILTER (WHERE div_cash <> 0)        AS dividends,
       count(*) FILTER (WHERE split_factor NOT IN (0,1)) AS splits
FROM equities_data.ohlcv_1d WHERE time >= now() - interval '60 days';

-- Class 3 terms are frozen: latest event per label.
SELECT action, max(date) FROM equities_data.corporate_action GROUP BY action ORDER BY 2 DESC;

-- Class 2 coverage is thin.
SELECT count(*) FROM equities_data.ticker_aliases;                 -- 16
SELECT count(*) FROM equities_data.corporate_action
 WHERE action = 'tickerchangeto';                                  -- 12,867
```
