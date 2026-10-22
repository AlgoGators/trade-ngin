# Data sources of truth: what to read, what never to read

Applies to every strategy and every asset class in this repo, not just the currently
configured universes. If you are writing code that reads market or corporate-action data,
this file decides which table and column you use.

Companion docs: `docs/CORP_ACTIONS_DATA_BOUNDARY.md` (per-event-class trust boundaries),
`docs/FUTURES_ROLLS.md` (how the futures series is built and how a roll is booked),
`docs/COST_MODEL.md` (how the contract metadata prices a fill).

---

## 1. The rule in one line

**Read raw prices plus the per-bar event columns, and compute adjustment in our own code.
Never read a vendor-derived adjusted column.**

This holds for both asset classes. Equities are adjusted from `div_cash` and `split_factor`
(section 2). Futures are adjusted from the vendor's instrument id on each bar (section 4).

---

## 2. Equities: `equities_data`

### USE: `equities_data.ohlcv_1d`
The single source for equity prices, adjustment events, and delisting timing.

| Column(s) | Use for | State |
|---|---|---|
| `time`, `symbol` | keys (NOT `date`/`ticker`: that shape is legacy, see the DO-NOT-USE entries below) | current |
| `open, high, low, close, volume` | **raw** prices, the input to our adjustment | current; 852 symbols in the full universe |
| `div_cash` | dividend per share, stamped on the **ex-date** bar | current |
| `split_factor` | splits, ADR-ratio changes, and the price effect of spin-offs | current |
| `delisting_date` | termination timing | maintained |

Adjustment is computed by `build_equity_adjusted_query()` (SQL,
`src/data/market_data_utils.cpp:36`) and mirrored by `compute_backward_adjustment_factors()`
(C++, `src/data/market_data_utils.cpp:69`):

```
f_i = f_{i+1} × ( close_{i+1} / (close_{i+1} + div_cash_{i+1}) ) / split_factor_{i+1}
adjusted = raw × f          (anchored at the newest bar, so latest price == traded price)
```

On the bars for which the vendor's own adjusted series is current (to 2026-08-05, see the next
entry) the formula reproduces that series up to rounding for every symbol whose dividends and
splits all sit on stored bars. A symbol with an event on a date whose bar is missing differs from
the vendor's series by that event's factor on every earlier bar: NSC does, because the vendor's
series carries an adjustment between the bars of 2026-08-06 and 2026-08-10, and the bar of
2026-08-07 is absent.

### ⚠️ DO NOT USE: `adj_open`, `adj_high`, `adj_low`, `adjusted_close`, `adj_volume`
These live in the same table and look authoritative. **They are stale after 2026-08-05.**
The vendor-side re-adjustment job has not run since, so an event after that date is not
folded back into history: MNST's 2026-08-11 2:1 split shows as a fake -50% day, and
ex-dividends since 2026-08-10 are unadjusted. We do not read these columns, so this does not
affect us; it does affect any other consumer (AlgoLens, algosystem) that still reads them.
Do not "fix" our code by switching to them. The repair is tracked in data-ngin #107.

### DO NOT USE: `equities_data.sharadar_ohlcv_1d`
Legacy price table from the Sharadar era, in the old `ticker`/`date`/`closeadj` shape.
Superseded. Only referenced in one explanatory code comment. Its removal is tracked in
data-ngin #112.

### AVAILABLE, UNUSED (not deprecated, just not wired yet)
- `sp500_membership` (`ticker, start_date, end_date`): survivorship-correct universe
  construction. Wanted when we trade a broad index universe.
- `coverage_gaps`, `verified_absent_bars`: data-quality aids for distinguishing
  "no bar because closed/absent" from "missing data".
- `ohlcv_1d_raw`: not read by the engine; confirm its purpose with the data owner before use.

---

## 3. Corporate actions: the two-source picture

Corporate actions split into four **mechanical effect classes**. Each class has exactly
one source. No class reads two tables. (Classification:
`src/live/corporate_actions_classification.cpp`.)

| Class | Vendor labels | Source | State |
|---|---|---|---|
| **PRICE_RESTATING** | split, adrratiosplit, spinoff, spinoffdividend, dividend | `ohlcv_1d.div_cash` / `.split_factor` | ✅ **alive and current** |
| **SERIES_CONTINUITY** | tickerchangefrom/to | `equities_data.ticker_aliases` | ⚠️ alive, **389 rows**: 16 curated rows plus 373 backfilled from the frozen table (below); nothing after 2025-08-29 unless added by hand |
| **TERMINATION, timing** | delisted, bankruptcy, merger, acquisition, … | `ohlcv_1d.delisting_date` | ✅ current |
| **TERMINATION, deal terms** | contra-ticker + ratio for the above | `equities_data.corporate_action` | ❌ **FROZEN at 2025-08-29** |
| **INFORMATIONAL** | listed, relation, initiated | none needed | n/a |

### About `equities_data.corporate_action`: frozen, NOT deprecated
627,169 rows, 19 action types, 1997-12-31 to **2025-08-29**, then nothing real. The shape is
Sharadar ACTIONS; its sibling `sharadar_ohlcv_1d` was the matching price table. The cause is
not a bug: the pipeline **moved prices from Sharadar to Tiingo**, and the Sharadar-sourced
actions feed stopped with it. Tiingo supplies per-bar dividend, split and delisting-date data,
which is why PRICE_RESTATING and the timing of TERMINATION are current. SERIES_CONTINUITY does
not come from Tiingo: it lives on the alias table, which has no source after the freeze date
(below). Tiingo does **not** publish merger or spin-off deal terms.

The `date` column is TEXT (`YYYY-MM-DD`), not a date: the engine compares it as text, between
two dates it passes as text (`src/data/postgres_database.cpp:2909`). Two rows carry
the far-future date 2027-07-18 (an SBDS ticker-change pair); any reader that takes
`max(date)` unbounded gets that value, not the freeze date.

**Keep this table and its code path.** It is the only home for deal terms. The query is
parameterised and returns zero rows today; it activates unchanged the moment rows appear
(pinned by `RevivedFeedActivatesTheRolloverPathWithNoCodeChange`,
`tests/live/corp_actions/test_effect_classes.cpp:325`).

**Distinguish carefully:**
- `corporate_action`: *we still need this data; the feed stopped.* Do **not** phase out.
- `sharadar_ohlcv_1d`: *we stopped needing it.* Safe to phase out.

### What the freeze actually costs
Nothing on price series or returns of what you hold: PRICE_RESTATING is complete and current.
The cost is **position lifecycle on two rare event types**: a stock-for-stock merger
(should roll into acquirer shares; we exit at final close) and a spin-off (should receive
child shares; we do not). Both are correct backtest convention and both diverge from a
real broker, surfacing at the point of sale or broker reconciliation. Cash mergers are
near-harmless (final close ≈ deal price). Delisting *timing* is unaffected.

### Recovery options, best first
A live deal-terms source is tracked in data-ngin #108; consuming it is trade-ngin #127.
1. Restart the Sharadar ACTIONS ingest if the subscription is live: restores full
   history and ongoing coverage.
2. Nasdaq Data Link hosts the same Sharadar tables if the subscription lapsed.
3. Polygon.io / EODHD corporate-action APIs (terms coverage thinner).
4. SEC EDGAR 8-K / S-4: free and authoritative, but parsing-heavy.
5. **Detect-and-confirm** (no vendor dependency): flag candidates automatically
   (`split_factor ≠ 1` with no matching split announcement; a `delisting_date` arriving
   on a held symbol), have a human confirm ratio + contra-ticker, and INSERT the confirmed
   row into `equities_data.corporate_action` using its existing columns. Every code path
   we already built then activates unchanged. Burden at full 852-symbol scale: a handful
   of events per year.

### The alias backfill
The frozen table holds 12,867 historical ticker-change pairs. `scripts/backfill_ticker_aliases.sql`
loads `ticker_aliases` from them for the symbols we carry: every ticker-change pair the table
holds of which either side has bars (`scripts/backfill_ticker_aliases.sql:46-68`). The script has
no date bound, so that is all history to 2025-08-29, and the mis-dated SBDS pair above if either
of its symbols is carried. It is idempotent and is part of a fresh environment's set-up
(section 7). A rename after the freeze date has no source and is added by hand.

---

## 4. Futures: `futures_data` and `metadata`

### 4.1 What the tables are

| Item | Detail |
|---|---|
| `futures_data.ohlcv_1d` | The loader's table: one daily bar per `.v.0` symbol and date, columns `time, symbol, open, high, low, close, volume`. 36 symbols, from 2010-06-07. **It has no primary key and no unique index** (section 4.3). |
| `futures_data.ohlcv_1d_raw` | The vendor's raw landing table. **Primary key `(ts_event, symbol)`**, and it carries `instrument_id` (the vendor's id of the contract the bar was printed in), `rtype` and `publisher_id`. One row per symbol and day. It is the only place the contract behind a bar can be read. |
| `futures_data.new_data_ohlcv_1d`, `new_data_ohlcv_1d_raw` | Dead staging tables (six symbols, ending 2025-11-03). Do not read; their removal is tracked in data-ngin #112. |
| `metadata.contract_metadata` | One row per contract: size, tick, fee, margins (section 4.7). |
| `metadata.symbols` | A second copy of `"IB Symbol"` and `"Contract Months"` per contract; a correction to either column is made in both tables. |

`ohlcv_1d_raw` holds the same print as the loader's kept copy of `ohlcv_1d` on every row
but three: 6A 2025-11-05 (raw holds the 196-lot wrong-instrument copy the loader drops), and
ZM and ZR 2025-10-10 (raw holds a traded bar where `ohlcv_1d` holds a zero-volume flat one).
For that reason the engine takes an instrument id from the raw table only for the exact
print it kept (`kFuturesRawBarTable`, `include/trade_ngin/data/market_data_utils.hpp:134`).

### 4.2 `.v.0` is a raw splice. Nothing is adjusted upstream.

`<ROOT>.v.0` is the vendor's volume-ranked front-contract resolver: each day's bar is the
bar of whichever contract of the root ranked first by volume, at that contract's **own raw
price**. Neither the vendor nor the data pipeline back-adjusts anything. On the bar where
the resolver switches contract, the close jumps by the price gap between two contracts.
That jump is not a return, and the resolver can also flip to another contract for a day and
flip back.

The **engine** does the adjusting, from the instrument id on each consumed bar
(`include/trade_ngin/data/roll_series.hpp:13`, the design comment; the functions are
`classify_instrument_changes` at `:65`, `adjusted_levels` at `:68`, `adjusted_returns` at
`:73`):

| Consumer | Reads |
|---|---|
| Every **return** consumer (the trend signal's EMAs, volatility, covariance, the risk readings' return history) | the back-adjusted series: the raw close with the price gap removed at each contract switch, anchored on the latest bar. The return on a change bar is 0. |
| Every **level** (position sizing, notional, margin, transaction costs, marks and P&L) | the raw close: the real price of the contract held. |

A reader that computes returns straight from `ohlcv_1d.close` books every roll gap as a
price move. `docs/FUTURES_ROLLS.md` describes the change bar, the confirmation, the two
ROLL fills and their costs.

Per-contract bars (one series per expiry, which would let a roll be priced on both
contracts on the same day) do not exist in the database; they are tracked in data-ngin #106.

### 4.3 Duplicate rows, and the rule that keeps one

Because `ohlcv_1d` has no key, a re-load stores a `(symbol, time)` again instead of
replacing it. Known stretches:

| Dates | Symbols | Copies | Do the copies agree? |
|---|---|---|---|
| 2025-10-06 to 2025-11-12 | 6A, 6L, HO, NG, ZF, ZT | 2 or 3 | yes, except 6A on 2025-11-05 (a 76,895-lot bar closing 0.6512 and a 196-lot bar closing 0.65095) |
| 2026-02-03 to 2026-02-05 | all 36 | up to 11 | yes |
| the last date of a load (2026-08-06 on CL, HO, RB) | as loaded | 2 | prices agree; the volume can differ (an earlier and a later snapshot of the same day) |

No row count is quoted here because any re-load can add to it. Count them with:

```sql
SELECT count(*) AS symbol_dates, sum(n - 1) AS extra_rows
FROM (SELECT symbol, time, count(*) AS n FROM futures_data.ohlcv_1d GROUP BY 1, 2 HAVING count(*) > 1) d;
```

**The loader rule.** The engine's bar query keeps exactly one row per `(symbol, time)`:
`DISTINCT ON (symbol, time)` ordered by `kFuturesBarKeepOrder`, which is
`"volume DESC, close, open, high, low"` (`include/trade_ngin/data/market_data_utils.hpp:101`;
the query is `build_futures_bar_query`, `:114`). So the copy kept is the one with the highest
volume, then the lowest close, open, high and low; the tie-break only makes the pick
independent of physical row order. Once per load the loader logs how many rows the rule dropped
and names every pair whose copies disagree (`log_futures_bar_duplicates`,
`src/data/postgres_database.cpp:1185`; the companion query is
`build_futures_duplicate_copies_query`, `market_data_utils.hpp:122`).

**Any other reader must de-duplicate the same way.** A plain `SELECT` from `ohlcv_1d`, a
`JOIN` on `(symbol, time)`, an average or a count over it returns the copies. Adding the
key and removing the copies is tracked in data-ngin #105.

### 4.4 Bad bars that are stored once

De-duplication cannot see a wrong bar that has no second copy. The table gives the three dates
checked bar by bar; the same one-day print on another instrument id occurs on other dates (in
2025: 01-21, 02-18, 04-21, 06-22, 08-18), and the session classifier judges every bar by the
rule below, not from a list.

| Date | Symbols | What the bar is |
|---|---|---|
| 2025-11-05 | 6B, 6C, 6E, 6J, 6M, GC, HG, SI, ZS | a thin print of a different contract: the instrument id differs from the day before and the day after, and the volume is a small fraction of the symbol's usual (GC 428 lots against about 200,000) |
| 2025-09-03 | GC, HG, KE, SI, UB, ZC, ZF, ZL, ZM, ZN, ZR, ZS, ZT, ZW | the same, one day on another id (GC 50 lots) |
| 2025-10-10 | ZM, ZR | zero volume, open = high = low = close |

The engine does not trade on them. The session classifier gives every bar of every symbol a
verdict (`SessionVerdict`, `include/trade_ngin/data/session_classifier.hpp:39`; the rule, in
order, is the comment at `:140`): a locked bar (`high == low`), a volume far below the
symbol's own trailing norm, and a thin print on a changed instrument id (below 0.10 of the
norm, `kIdChangeHoldFraction`, `:54`) are all `JUNK`. On a `JUNK` verdict the position is
held and no order is placed for that symbol; a `SESSION` bar is the only kind that trades.
Another reader has no such guard and must filter these bars itself. Re-fetching them is
part of data-ngin #105.

### 4.5 Dates, weekends and the calendar

- **Every bar is stamped midnight UTC** of its date (`time` is `timestamptz`, always
  `00:00:00+00`). The stamp is a date label, not the time of the close. A reader in a time zone
  behind UTC (New York, Chicago) that converts before truncating to a date moves every bar to
  the previous day; a zone ahead of UTC keeps the date. Read the date in UTC. The engine's own
  loader does not take the stored instant as it is: it reads the text of the stored time as a
  local time of the host, renders that instant in UTC and reads the result as a local time again
  (`src/data/postgres_database.cpp:1376-1383`), so a bar's instant in memory is the stored one
  moved by twice the host's offset from UTC. With the stored time rendered in UTC by the database
  session, the instant is unchanged on a host at UTC and falls later on the same UTC date on a
  host behind UTC, which is what the image in this repository is (`TZ=America/New_York`,
  `Dockerfile:85`): the bar keeps its date on both. `docs/LIVE_RUN_CYCLE.md` says what else
  depends on the host's time zone.
- **The Sunday bar** is the vendor's UTC-day cut of the first hour or two of the CME week:
  the Sunday evening open falls before midnight UTC, so those prints land on Sunday's date
  as their own bar, with a few percent of the following Monday's volume. It is a real,
  thin bar; whether it trades is the session classifier's verdict like any other.
- **Which roots print no Sunday bar:** the livestock roots (LE has none; GF and HE have one
  each, on 2014-05-25) and the grains (ZC, ZS, ZW, ZL, ZM, ZR, KE). The six CBOT grains
  printed one on most Sundays until 2013-03-31; since then the grains have only isolated ones,
  on 2014-05-25, 2014-08-31 and 2018-08-05 (ZM has no bar on the first, ZR none on the
  second), and none after.
  For these a missing Sunday is a closure, not a feed hole.
- **MBT prints Saturday bars** from 2026-06-13 on, although its metadata hours read
  Sunday to Friday like the other CME contracts. No other root has a Saturday bar.
- **There is no CME calendar.** The only calendar the engine has is
  `include/trade_ngin/core/holidays.json`, generated by `scripts/generate_market_holidays.py`
  from the NYSE rules (full-day equity-market closures, no early closes, no per-exchange
  field). Futures trade on several days it names. The classifier therefore decides "no bar
  expected" per symbol from that file plus the symbol's own printing history, never from the
  file alone. A calendar published as data is tracked in data-ngin #111 and trade-ngin #126.

### 4.6 The contract set

The futures book trades **36 contracts**: the 36 `.v.0` symbols of `ohlcv_1d`.

| Sector | Roots |
|---|---|
| FX | 6A, 6B, 6C, 6E, 6J, 6L, 6M, 6N, 6S |
| Equity index (micros) | MES, MNQ, MYM, M2K |
| Interest rates | ZT, ZF, ZN, UB |
| Energy | CL, HO, RB, NG |
| Metals | GC, SI, HG, PL |
| Agriculture | ZC, ZS, ZW, KE, ZL, ZM, ZR, LE, GF, HE |
| Crypto | MBT |

**The four equity index micros and their listing date.** MES, MNQ, MYM and M2K were listed
on 2019-05-06, but the vendor's series under those symbols starts earlier (MES, MNQ and MYM
on 2010-06-07, M2K on 2017-07-09). The bars before the listing date are the **E-mini's**
prices under the micro's symbol: the price is right, the contract did not exist. There are
no rows stored under `ES.v.0`, `NQ.v.0`, `YM.v.0` or `RTY.v.0`.

With the `listing_dates` block of `portfolio.json` (see `docs/CONFIG_GUIDE.md`) the engine
trades each contract only when it existed (`include/trade_ngin/data/listing_dates.hpp:108`,
`ListingDates`; `tradeable` at `:137`):

| Signal bar dated | The book holds | Size against the micro | Metadata row |
|---|---|---|---|
| before 2019-05-06 | ES, NQ, YM, RTY | ten times | its own: contract size, tick, fee and margins of the E-mini |
| on or after 2019-05-06 | MES, MNQ, MYM, M2K | one | its own |

The E-mini has no stored rows, so a backtest whose window starts before the listing date
adds it as a symbol of the run and hands it the rows stored under the micro's symbol
(`predecessor_symbols`, `:156`; `add_predecessor_bars`, `:164`). The pair is one instrument
with one price history. On the switch day the book exits the E-mini and enters the micro
(fill ids `LC-` and `LO-`). A live run trades no predecessor and refuses one in its universe
or its stored book (`include/trade_ngin/live/live_listing_guard.hpp:14`). Without the block
the micro's symbol is traded at the micro's size over the whole stored history.

Two things follow for anyone counting "the book's 36 symbols": a backtest that trades
before 2019-05-06 holds positions and fills in four symbols that are not among the 36, and
the Russell pair has no history at all before 2017-07-09.

**The vendor relabel of 2026-02-22.** On that date the instrument id on all four micros
changes at once (for example MES from `42140878` to `42003800`), in the middle of a
contract's life. It is not a roll: no expiry changes hands. Read naively it is a change bar,
and the engine would hold through it and book two ROLL fills with their costs. The
`instrument_id_relabels` list of `portfolio.json` declares the four changes, and the loader
then reads a bar carrying the new id as carrying the old one from that date
(`InstrumentIdRelabel`, `include/trade_ngin/data/listing_dates.hpp:54`; `apply_relabels`,
`:177`). A reader of `ohlcv_1d_raw.instrument_id` that detects rolls by an id change must
apply the same list.

### 4.7 `metadata.contract_metadata`: the basis of every size, cost and margin

40 rows: the 36 traded contracts plus ES, NQ, YM and RTY (the contracts held before the
micros' listing date). Every column is text. The database layer reads the first 21 columns by
their position in the table (`src/data/postgres_database.cpp:1529-1551`) and
`"Fee Per Contract"` by name (`:1815`); the instrument registry then reads the converted table
by column name (`InstrumentRegistry::load_instruments`,
`src/instruments/instrument_registry.cpp:117`). The column order of the table must therefore
not change.

| Column | Meaning | Read at |
|---|---|---|
| `"Contract Size"` | **dollars per 1.0 of quoted price** (the multiplier): 50 for ES, 5 for MES, 1000 for CL, 100000 for 6A. Notional = contracts × price × this. | `instrument_registry.cpp:375` |
| `"Tick Size"` | one tick in price units (the unit the close is stored in) | `:402` |
| `"Minimum Price Fluctuation"` | the same tick in dollars per contract. It must equal `"Tick Size"` × `"Contract Size"`; all 40 rows do, and the loader warns on a row that does not (`:408`). | `:386` |
| `"Fee Per Contract"` | dollars per contract per side that the cost model charges, **per row**, from IBKR's published futures schedule (migration 026): commission plus the exchange and regulatory fee recovery charges. 0.610 to 0.614 on the four micros, 1.510 to 3.010 on the rest (ES 2.247 against MES 0.614). A CHECK (`contract_metadata_fee_per_contract_positive`) refuses a cell that is not a plain decimal number above zero. | `:422` |
| `"Overnight Initial Margin"`, `"Overnight Maintenance Margin"` | the margin per contract the engine uses | `:435`, `:436` |
| `"Intraday Initial Margin"`, `"Intraday Maintenance Margin"` | not read by the instrument registry | |
| `"IB Symbol"`, `"Contract Months"` | read by the daily email (the broker symbol and the rollover-warning cells); the registry falls back to `"IB Symbol"` only for a row with an empty `"Databento Symbol"` (`:363`), and no row has one | |
| `"Trading Hours (EST)"` | carried on the instrument; the session classifier does not read it | `:437` |

Migration 019 holds the corrections these rows carry: the tick of 6A and 6L is half a point
(`"Tick Size"` 0.00005, `"Minimum Price Fluctuation"` 5); `"IB Symbol"` of 6B, 6E, 6S, ZS and
ZW names the full-size contract the engine trades (GBP, EUR, CHF, ZS, ZW), not the broker's
micro FX or mini grain; `"Contract Months"` of HO, NG and 6L reads `All Months`. Apart from
the four equity index micros and MBT, every row is the full-size contract.

Three cells carry a stray character: a trailing non-breaking space on `"Tick Size"` of 6N and
HG, and a leading tab on `"Overnight Maintenance Margin"` of YM. The loader reads the number
beside it (`src/data/postgres_database.cpp:1577-1581`). A SQL cast of the column fails on those
rows, and `trim()` does not remove the non-breaking space, so a SQL reader strips non-numeric
characters first. Only the fee column has a CHECK.

`docs/COST_MODEL.md` shows how the tick, the fee and the day's volume become the cost of a
fill.

**Never re-seed this table from an old CSV export.** A snapshot taken before the
dollar-multiplier basis carries contract sizes in other units (grains 100 times too large,
ZT half size) and none of the corrections above. The migrations are the record.

---

## 5. Other schemas

| Schema | Use | State |
|---|---|---|
| `trading` | `live_results`, `positions`, `executions`, `signals`, `equity_curve`, `live_run_metadata` and `corp_action_applied` (written by the runners), and `strategy_trading_days_metadata` (read by them) | positions/executions/signals carry `strategy_name` and **do** attribute per strategy; `live_results`/`equity_curve` aggregate (no `strategy_name`). `docs/LIVE_RUN_CYCLE.md` says what one row of each table is. |
| `backtest` | backtest runs and per-day rows | `executions`, `final_positions`, `signals` and `run_metadata` carry `strategy_id` and attribute per sleeve; `results` and `equity_curve` are one per run, the whole book |
| `macro_data` | regime pipeline inputs (`bsts_etf_prices`, `credit_spreads`, `growth`, `inflation`, `liquidity`, `market`, `yield_curve`) | not read by any code in this repository; keeping it current is tracked in data-ngin #109 |
| `eia`, `research` and one other schema that is not this repository's | not consumed by this repo | n/a |

---

## 6. Rules for new code

1. Equity prices: read raw + `div_cash` + `split_factor`; adjust via `market_data_utils`.
   Never read `adj_*`/`adjusted_close`/`closeadj`.
2. Equity keys are `symbol` / `time`. `ticker` / `date` is the legacy Sharadar shape.
3. Corporate actions: go through the effect-class layer, never query raw action strings.
4. Design for the **full 852-symbol universe**, not the 10 symbols currently configured
   for `equity_mr`: other and future strategies may hold any symbol with data.
5. Anything reading `corporate_action` must behave correctly with **zero rows** returned,
   and must light up unchanged when rows appear.
6. When a feed's staleness would change a result, fail loudly: do not silently proceed.
7. Futures bars: read `ohlcv_1d` through the loader, or de-duplicate with the loader's
   order (section 4.3). Never compute a return across a contract switch from the raw close
   (section 4.2), and never read the bars before a contract's listing date as that
   contract's (section 4.6).
8. Futures sizes, ticks, fees and margins come from `metadata.contract_metadata` (section 4.7
   says how its columns are found). Nothing about a contract is hard-coded.

---

## 7. Required database objects: engine dependencies

Any environment running this engine (including the AlgoGators mono repo after migration,
and any fresh, staging or production database) MUST have the objects below. They are not
optional: without the indexes `equities_data.corporate_action` has no index at all, so the
deal-terms read scans the whole table, and `equities_data.ohlcv_1d` has only its `(symbol, time)`
primary key, which does not pick out the few bars that carry an event or a delisting date; and
without the tables the corp-action path cannot dedup and will re-apply events.

### Owned by this repo: `trading` schema

| Object | Created by | Purpose |
|---|---|---|
| `trading.positions.portfolio_type` + widened keys | `migrations/001_add_portfolio_type.sql` | dual-portfolio streams (system/qt) |
| `trading.equity_curve` unique key incl. `portfolio_type` | same | upsert target for live equity-curve writes |
| `trading.corp_action_applied` | `migrations/002_corp_action_applied.sql` | durable corporate-action dedup. Replaces a JSON file under a container path with no volume, where state loss was the default and re-application the consequence |

The later migrations add the columns the runners write (on `trading`, `backtest` and
`metadata.contract_metadata`) and make the schema and data corrections they depend on: 003 adds
indexes on `equities_data`, 004 replaces the function `trading.get_trading_days`, 012 widens
`strategy_id`, 019 and 026 change rows of `metadata.contract_metadata` (026 also adds the fee
CHECK). Each file's header says what it does and why.

### NOT owned by this repo: `equities_data` schema (data-ngin owns it)

`migrations/003_equity_query_indexes.sql` creates indexes inside `equities_data`. This is
a deliberate boundary crossing: additive and reversible (`DROP INDEX`), but the schema
belongs to the data pipeline.

**Two consequences the data owner must know:**
1. If data-ngin ever rebuilds or recreates these tables, the indexes disappear with them.
   The only symptom is the live equity run silently getting slower: nothing points at the
   cause.
2. data-ngin's own migration tooling has no record of these objects, so its schema
   definition and reality drift apart.

The durable form is for data-ngin to adopt these indexes into its own schema definition, so
they survive table rebuilds and are visible to its tooling. Until then they work, but are
not durable against upstream changes.

What each index is for (`migrations/003_equity_query_indexes.sql`):

| Index | On | The query it serves |
|---|---|---|
| `idx_ohlcv_1d_corp_events` | `equities_data.ohlcv_1d ("time", symbol)`, only the bars that carry a dividend or a split factor other than 0 or 1 | the per-bar corporate actions of a date window, read on every live equity run (`get_per_bar_corporate_actions`, `src/data/postgres_database.cpp:2998`). It leads with `time` because the query filters a date range across every held symbol |
| `idx_ohlcv_1d_delisting` | `equities_data.ohlcv_1d (symbol, delisting_date)`, only the bars that carry a delisting date | the termination timing of the held symbols (`get_delisting_dates`, `:3132`) |
| `idx_corporate_action_ticker_date` | `equities_data.corporate_action (ticker, date)` | the deal terms of a set of held symbols over a window (`get_corporate_actions`, `:2888`). The table has no other index |

The two indexes on `ohlcv_1d` are partial, so they stay small while covering the whole
condition of their query; the table's only other index is its `(symbol, time)` primary key.

### Known cost that indexing does NOT solve

The equity adjustment query computes the backward cumulative product in the query itself, as a
window aggregate over every bar of every symbol in the window it is asked for, each time it runs
(`build_equity_adjusted_query`, `src/data/market_data_utils.cpp:36`). Its cost is that aggregate,
and it grows with the number of symbols times the number of bars. The query reads every row of
its window, so no index shortens it. Making it fast means materialising the adjustment factors
instead of recomputing them per query. Budget for it in run planning; it is not a defect.

### Migration order

The migrations in `migrations/` are **001 to 006, 012 to 020 and 026**. Apply them in
ascending number order, every one of them, before running a binary built from this
repository, then run `scripts/backfill_ticker_aliases.sql`. The repository holds no migration
with a number between those, and nothing in it depends on one. Each migration carries a rollback file beside it
(003's rollback drops only the indexes it created).

How to apply them, what each one's safety guards do, and the shell tests that exercise
them are in `docs/performance_upkeep.md` (section 9, "Migrations").
