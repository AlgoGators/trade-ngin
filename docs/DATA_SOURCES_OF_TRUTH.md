# Data sources of truth — what to read, what never to read

Status: 2026-08-31. Applies to every strategy and every asset class in this repo, not
just the currently-configured universes. If you are writing code that reads market or
corporate-action data, this file decides which table and column you use.

Companion docs: `docs/CORP_ACTIONS_DATA_BOUNDARY.md` (per-event-class trust boundaries),
`docs/DB_AND_DATA_AUDIT_2026-08-27.md` (evidence for every claim below).

---

## 1. The rule in one line

**Read raw prices plus per-bar event columns, and compute adjustment in our own code.
Never read a vendor-derived adjusted column.**

---

## 2. Equities — `equities_data`

### USE: `equities_data.ohlcv_1d`
The single source for equity prices, adjustment events, and delisting timing.

| Column(s) | Use for | State |
|---|---|---|
| `time`, `symbol` | keys (NOT `date`/`ticker` — that shape is legacy, see §2 DO-NOT-USE) | current |
| `open, high, low, close, volume` | **raw** prices — the input to our adjustment | current through **2026-08-27**, 852 symbols (700 active in the last 30 days) |
| `div_cash` | dividend per share, stamped on the **ex-date** bar | current |
| `split_factor` | splits, ADR-ratio changes, and the price effect of spin-offs | current |
| `delisting_date` | termination timing | maintained, latest value 2026-04-09 |

Adjustment is computed by `build_equity_adjusted_query()` (SQL) and mirrored by
`compute_backward_adjustment_factors()` (C++, `market_data_utils`):

```
f_i = f_{i+1} × ( close_{i+1} / (close_{i+1} + div_cash_{i+1}) ) / split_factor_{i+1}
adjusted = raw × f          (anchored at the newest bar, so latest price == traded price)
```

Verified against the vendor's own adjusted series to a **max relative deviation of
9.489e-13** over 2023-01-01 → 2026-08-05, 12 symbols, 10,800 bars, 132 corporate actions
including both GE spin-offs. See phase 4.2.

### ⚠️ DO NOT USE: `adj_open`, `adj_high`, `adj_low`, `adjusted_close`, `adj_volume`
These live in the same table and look authoritative. **They are stale after 2026-08-05.**
The vendor-side re-adjustment job died in the 2026-08-06 data-ngin incident, so any event
after that date was never folded back into history — e.g. MNST's 2026-08-11 2:1 split
shows as a fake −50% day, and ~50 ex-dividends since 08-10 are unadjusted. We do not read
these columns, so this does not affect us; it does affect any other consumer
(AlgoLens, algosystem) that still reads them. Do not "fix" our code by switching to them.

### DO NOT USE: `equities_data.sharadar_ohlcv_1d`
Legacy price table from the pre-migration Sharadar era, in the old `ticker`/`date`/
`closeadj` shape. Superseded. Only referenced in one explanatory code comment. Safe to
phase out once the data owner confirms nothing external depends on it.

### AVAILABLE, UNUSED (not deprecated — just not wired yet)
- `sp500_membership` (`ticker, start_date, end_date`) — survivorship-correct universe
  construction. Wanted when we trade a broad index universe.
- `coverage_gaps`, `verified_absent_bars` — data-quality aids for distinguishing
  "no bar because closed/absent" from "missing data".
- `ohlcv_1d_raw` — purpose unconfirmed; ask the data owner before use.

---

## 3. Corporate actions — the two-source picture

Corporate actions split into four **mechanical effect classes**. Each class has exactly
one source. No class reads two tables. (Classification: `corporate_actions_classification.cpp`.)

| Class | Vendor labels | Source | State |
|---|---|---|---|
| **PRICE_RESTATING** | split, adrratiosplit, spinoff, spinoffdividend, dividend | `ohlcv_1d.div_cash` / `.split_factor` | ✅ **alive and current** |
| **SERIES_CONTINUITY** | tickerchangefrom/to | `equities_data.ticker_aliases` | ⚠️ alive but **thin — 16 curated rows** vs 12,867 historical pairs in the frozen table |
| **TERMINATION — timing** | delisted, bankruptcy, merger, acquisition, … | `ohlcv_1d.delisting_date` | ✅ current |
| **TERMINATION — deal terms** | contra-ticker + ratio for the above | `equities_data.corporate_action` | ❌ **FROZEN at 2025-08-29** |
| **INFORMATIONAL** | listed, relation, initiated | none needed | n/a |

### About `equities_data.corporate_action` — frozen, NOT deprecated
627,167 rows, 19 action types, 2000 → **2025-08-29**, then nothing. The shape is
Sharadar ACTIONS; its sibling `sharadar_ohlcv_1d` was the matching price table. The
probable cause is not a bug: the pipeline **migrated prices Sharadar → Tiingo**, and the
Sharadar-sourced actions feed stopped with it. Tiingo supplies excellent per-bar
dividend/split data — which is why classes 1–3 still work — but does **not** publish
merger/spin-off deal terms.

**Keep this table and its code path.** It is the only home for deal terms. The query is
parameterised and returns zero rows today; it activates unchanged the moment rows appear
(pinned by `RevivedFeedActivatesTheRolloverPathWithNoCodeChange`).

**Distinguish carefully:**
- `corporate_action` — *we still need this data; the feed stopped.* Do **not** phase out.
- `sharadar_ohlcv_1d` — *we stopped needing it.* Safe to phase out.

### What the freeze actually costs
Nothing on price series or returns of what you hold — class 1 is complete and current.
The cost is **position lifecycle on two rare event types**: a stock-for-stock merger
(should roll into acquirer shares; we exit at final close) and a spin-off (should receive
child shares; we do not). Both are correct backtest convention and both diverge from a
real broker, surfacing at the point of sale or broker reconciliation. Cash mergers are
near-harmless (final close ≈ deal price). Delisting *timing* is unaffected.

### Recovery options, best first
1. Restart the Sharadar ACTIONS ingest if the subscription is live — restores full
   history and ongoing coverage. **This is the ask on the data owner.**
2. Nasdaq Data Link hosts the same Sharadar tables if the subscription lapsed.
3. Polygon.io / EODHD corporate-action APIs (terms coverage thinner).
4. SEC EDGAR 8-K / S-4 — free and authoritative, but parsing-heavy.
5. **Detect-and-confirm** (no vendor dependency): flag candidates automatically
   (`split_factor ≠ 1` with no matching split announcement; a `delisting_date` arriving
   on a held symbol), have a human confirm ratio + contra-ticker, and INSERT the confirmed
   row into `equities_data.corporate_action` using its existing columns. Every code path
   we already built then activates unchanged. Burden at full 852-symbol scale: a handful
   of events per year.

### Backfill opportunity
The frozen table holds 12,867 historical ticker-change pairs while `ticker_aliases` has
16 rows. Aliases can be backfilled from it for all history up to 2025-08-29.

---

## 4. Futures — `futures_data`

| Item | Detail |
|---|---|
| `futures_data.ohlcv_1d` | 36 `.v.0` symbols. **Feed DEAD since 2026-08-06** (6J/ZC/ZF stopped 08-04) |
| Adjustment | none applied by us — `.v.0` is an unadjusted volume-rolled splice, back-adjusted upstream by convention |
| `metadata.contract_metadata` | contract sizes/tick data. **`Contract Size` = dollar multiplier per 1.0 of quoted price** since the 2026-05-02 migration; 36/36 rows pass both invariants |
| ⚠️ root `contract_metadata.csv` | **stale PRE-migration snapshot.** Never re-seed the DB from it — it would silently revert the dollar-multiplier fix (100× notional inflation on grains, half-size ZT) |

---

## 5. Other schemas

| Schema | Use | State |
|---|---|---|
| `trading` | live results, positions, executions, signals, equity_curve | positions/executions/signals carry `strategy_name` and **do** attribute per strategy; `live_results`/`equity_curve` aggregate (no `strategy_name`) |
| `backtest` | backtest runs and per-day rows | attributes per strategy correctly |
| `macro_data` | regime pipeline inputs | stale since ~2026-04-08; `credit_spreads` exists as a DFM input — confirm refresh before regime runs |
| `eia`, `research`, `jonah_nissan` | not consumed by this repo | — |

---

## 6. Rules for new code

1. Equity prices: read raw + `div_cash` + `split_factor`; adjust via `market_data_utils`.
   Never read `adj_*`/`adjusted_close`/`closeadj`.
2. Equity keys are `symbol` / `time`. `ticker` / `date` is the legacy Sharadar shape.
3. Corporate actions: go through the effect-class layer, never query raw action strings.
4. Design for the **full 852-symbol universe**, not the 10 symbols currently configured
   for `equity_mr` — other and future strategies may hold any symbol with data.
5. Anything reading `corporate_action` must behave correctly with **zero rows** returned,
   and must light up unchanged when rows appear.
6. When a feed's staleness would change a result, fail loudly — do not silently proceed.

---

## 7. Required database objects — engine dependencies

Any environment running this engine (including the algogators mono repo after migration,
and any fresh/staging/prod database) MUST have the objects below. They are not optional
performance tuning: without the indexes the live equity path degrades from ~1 s to ~14 s
per query at full universe scale, and without the tables the corp-action path cannot
dedup and will re-apply events.

### Owned by this repo — `trading` schema

| Object | Created by | Purpose |
|---|---|---|
| `trading.positions.portfolio_type` + widened keys | `migrations/001_add_portfolio_type.sql` | dual-portfolio streams (system/qt) |
| `trading.equity_curve` unique key incl. `portfolio_type` | same | upsert target for live equity-curve writes |
| `trading.corp_action_applied` | `migrations/002_corp_action_applied.sql` | durable corporate-action dedup. Replaces a JSON file under a container path with no volume, where state loss was the default and re-application the consequence |

### NOT owned by this repo — `equities_data` schema (data-ngin owns it)

`migrations/003_equity_query_indexes.sql` creates indexes inside `equities_data`. This is
a deliberate boundary crossing: additive and reversible (`DROP INDEX`), but the schema
belongs to the data pipeline.

**Two consequences the data owner must know:**
1. If data-ngin ever rebuilds or recreates these tables, the indexes disappear with them.
   The only symptom is the live equity run silently getting ~15x slower — nothing points
   at the cause.
2. data-ngin's own migration tooling has no record of these objects, so its schema
   definition and reality drift apart.

**Requested resolution:** adopt these indexes into data-ngin's own schema definition so
they survive table rebuilds and are visible to its tooling. Until then they work, but are
not durable against upstream changes.

Measured impact at 852 symbols (before → after):

| Query | Before | After |
|---|---|---|
| per-bar corporate actions (every live run) | 14,301 ms | 800 ms |
| `get_delisting_dates` | 14,144 ms | 1,865 ms |
| deal terms lookup | 491 ms | 281 ms |

### Known cost that indexing does NOT solve

The equity adjustment query is ~25 s at 852 symbols over two years. The cost is the
`WindowAgg` computing the backward cumulative product (~364k rows), not I/O or sorting —
`work_mem` tuning removes the disk spills but not the time. Making it genuinely fast means
materialising adjustment factors rather than recomputing per query. Budget for it in run
planning; it is not a defect.

### Migration order for a fresh environment

001 → 002 → 003, then `scripts/backfill_ticker_aliases.sql`. All are idempotent and carry
rollback files (003's rollback drops only the indexes it created).
