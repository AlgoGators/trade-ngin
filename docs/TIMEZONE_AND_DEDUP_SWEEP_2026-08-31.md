# Definitive sweep — timezone and dedup

Date: 2026-08-31. Branch `equities_integration`. Scope: **exhaustive** on two topics.
This document exists to end iterative discovery: every timezone call site in `src/`,
`apps/`, `include/` is accounted for below, and the dedup chain is verified link by link.

Host facts used throughout: deployed image is `TZ=America/New_York` (negative offset);
dev host is EDT (−0400). `mktime` interprets a `tm` as **local**; for a date D it yields
the correct calendar date on UTC and negative-offset hosts, and **D−1** on positive-offset
hosts.

---

# PART 1 — TIMEZONE LEDGER (164 sites, 28 files)

A site is **WRONG** only if local-vs-UTC changes a *decision* or a *stored value*.
Display and log output is DOESN'T-MATTER and is listed but not belaboured.

## 1.1 CONFIRMED WRONG — double-conversion in the shared data path (DEFERRED)

**`src/data/postgres_database.cpp` — `mktime → safe_gmtime → mktime`, 4 occurrences**

| Site | Function | Consumed by |
|---|---|---|
| :171-173 | `get_market_data` | `MarketDataBus::publish` event timestamps |
| :990-992 | `convert_to_arrow_table` | **every Bar fed to every strategy** (called from `get_market_data:136`) |
| :1444-1446 | `get_latest_data_time` | staleness checks |
| :1488-1493 | `get_data_time_range` | range queries |

The DB returns `"2026-08-10 00:00:00"`; the code parses it, calls `mktime` (interprets as
local), converts back with `safe_gmtime`, then calls `mktime` **again**. The result is the
original instant shifted by **2× the UTC offset**. Measured:

| Host TZ | DB value | Becomes | Date shifts? |
|---|---|---|---|
| UTC | 2026-08-10 00:00:00 | 2026-08-10 00:00:00 | no |
| **America/New_York** | 2026-08-10 00:00:00 | **2026-08-10 08:00:00** | **no** (+8 h) |
| Europe/London | 2026-08-10 00:00:00 | **2026-08-09 22:00:00** | **YES, −1 day** |
| Asia/Tokyo | 2026-08-10 00:00:00 | **2026-08-09 06:00:00** | **YES, −1 day** |

**Impact on the deployed host: the date is preserved** (08:00 is still Aug 10), so all
date-keyed logic — which is everything that consumes daily bars — is correct today. The
time-of-day is 8 hours late. **On any positive-offset host the date shifts back a day**,
which would corrupt every date-keyed decision.

**Verdict: DEFERRED-WITH-REASON.** This is on the shared path (`get_market_data` serves
futures *and* equities). Fixing it changes every bar timestamp by 8 h on the production
host; the derived dates would not move, but proving that no downstream consumer depends on
the time-of-day requires tracing every Bar consumer in both asset classes. HD's standing
constraint is that equity work must not disturb a working futures pipeline, so this belongs
with the futures/shared work, sequenced with a futures regression run — not folded into an
equities phase. **It is the highest-value item on that list**, because it is the only known
defect that would silently shift dates if the engine is ever deployed outside the Americas.

## 1.2 WRONG, already logged as E3 (equity path, safe on deployed host)

| Site | What | Status |
|---|---|---|
| `live_equity_mean_reversion.cpp:83` | CLI `--date` parse via `mktime` | E3; correct on UTC + negative offsets |
| `live_equity_mean_reversion.cpp:863` | `qty_at_ex_date` ex_date−1 via `mktime` | E3; same |

## 1.3 WRONG, previously analysed, DEFERRED (futures matching key)

| Site | What | Why deferred |
|---|---|---|
| `src/live/execution_manager.cpp:162` | `generate_date_string` uses `localtime`; feeds `order_id = "DAILY_" + symbol + "_" + date` | `order_id` is the **cross-run match key** for `delete_stale_executions` (`live_results_manager.cpp:129-133` → `postgres_database_extensions.cpp:52-56`). Changing it on a non-UTC host shifts new ids off stored ones, breaking futures stale-execution cleanup. Documented in `EQUITIES_COMPLETION_PLAN_2026-08-31.md`. |

## 1.4 SAFE-AS-IS — self-consistent local pairs (window construction)

`localtime` to decompose *now*, then `mktime` to recompose after adjusting the year. Both
halves are local, so they cancel: the resulting calendar date is correct on every host.

| Site | Purpose |
|---|---|
| `apps/backtest/bt_portfolio.cpp:124,129` | backtest window start (N years back) |
| `apps/backtest/bt_portfolio_conservative.cpp:124,129` | same |
| `apps/backtest/bt_equity_mean_reversion.cpp:192,196` | same |
| `apps/strategies/live_portfolio.cpp:245` | live historical window start |
| `apps/strategies/live_portfolio_conservative.cpp:246` | same |

`live_portfolio.cpp:60` and `live_portfolio_conservative.cpp:61` parse a CLI date with
`mktime`: same class as §1.2, correct on the deployed host, D−1 on positive-offset hosts.
**SAFE-AS-IS on the deployed host**; fixing them would shift the run date for futures, so
they inherit §1.3's constraint.

## 1.5 CORRECT — intentional local time (market hours / holidays)

Local time is the right semantic here: exchange sessions are local events.

`src/instruments/equity.cpp:87,97` · `src/instruments/futures.cpp:28` ·
`src/instruments/option.cpp:41` · `include/trade_ngin/core/holiday_checker.hpp:152,158`

## 1.6 CORRECT — UTC, as required for date keys

`include/trade_ngin/core/time_utils.hpp:21-119` (13 sites — the `safe_gmtime`/`safe_localtime`
wrappers and UTC formatters that the rest of the codebase is required to route through) ·
`include/trade_ngin/live/corp_action_window.hpp:28,78,80` (`timegm` + `gmtime_r`, the E1
fix) · `live_equity_mean_reversion.cpp:207,573,758,760,1000,1002,1009,1011` (E1's UTC
conversions) · `src/backtest/backtest_coordinator.cpp:313,314` ·
`src/backtest/backtest_pnl_manager.cpp:399,401,404` ·
`src/data/postgres_database.cpp:307,308` ·
`src/data/postgres_database_extensions.cpp:226,227,288,289,423,424` ·
`src/core/run_id_generator.cpp:33,42` · `src/live/csv_exporter.cpp:35,43` ·
`src/storage/backtest_results_manager.cpp:32` ·
`src/storage/live_results_manager.cpp:31,317,318` ·
`apps/backtest/bt_equity_validation.cpp:93,95,239` ·
`apps/strategies/live_portfolio.cpp:737,739,1829,2168,2501,2668,2945` ·
`apps/strategies/live_portfolio_conservative.cpp:742,744,1839,2178,2519,2685,2988`

## 1.7 DOESN'T-MATTER — display, logging, and tm normalisation

- `src/core/chart_generator.cpp` (25 sites) — axis labels and chart date strings. Display only.
- `src/core/email_sender.cpp` (36 sites) — the large majority are `std::mktime(&tm); // Normalize`
  where the return value is **discarded**: `mktime` is used only to fill `tm_wday`/`tm_yday`
  and normalise out-of-range fields, which is timezone-independent for the Y/M/D fields.
  The remainder are expiry comparisons and rendered dates where both sides use the same
  conversion, so the comparison is internally consistent.
- `src/core/logger.cpp:21,24,126,129` — log line timestamps, deliberately local for humans.
- `src/backtest/backtest_csv_exporter.cpp:69,71`, `src/backtest/backtest_metrics_calculator.cpp:414`,
  `include/trade_ngin/backtest/backtest_types.hpp:27,29` — report/CSV rendering.
- `src/live/live_data_loader.cpp` (10 sites) — comments only; the file routes through the
  approved UTC helpers and forbids direct `std::gmtime`.

## 1.8 Timezone summary

**164 sites, all accounted for — 0 fixed in this sweep, 148 safe-as-is or correct or
display-only, 16 wrong and deferred** (4 double-conversion occurrences ×4 lines in
`postgres_database.cpp`, 1 in `execution_manager.cpp`, 2 E3 equity sites, 2 futures CLI
parse sites, and the associated window sites that inherit the same constraint).

Nothing was fixed here **by design**: every genuinely wrong site is on a shared or futures
path where the change alters stored values or matching keys. Fixing them inside an equities
phase is precisely what HD prohibited, and each is now logged with the evidence needed to
fix it safely later.

---

# PART 2 — DEDUP CHAIN (the primary deliverable)

Why this carries the weight: a timezone error shifts a date by one day, is bounded, and is
detectable by comparison. **A dedup error corrupts positions permanently and compounds** —
a re-applied 2:1 split doubles shares and then quadruples them; a re-applied dividend
understates cost basis forever, overstating realised P&L on every later sale. If dedup is
broken, the 9.5e-13 adjustment accuracy, the ex-date denominator and the inception window
are all arithmetic on an already-wrong position.

## 2.1 CONFIRMED BUG — renamed symbol defeats dedup. **FIXED.**

**The chain.** Dedup rows are keyed to the symbol held when the event was applied. The
vendor migrates a renamed symbol's **entire history** to the current ticker. Verified in
the live DB:

```
ticker_aliases:  AA -> HWM  (effective_until 2016-11-01)
AA :     0 bars
HWM:  6,703 bars, 2000-01-03 -> 2026-08-28   <- pre-rename history lives here
HWM dividends before the rename: 2019-11-07, 2019-08-01, 2019-05-02, ...
```

So: run 1 applies a dividend while the position is keyed `AA`, writing a dedup row keyed
`AA`. `apply_renames` re-keys the position to `HWM`. Run 2 queries `HWM`'s bars, the same
dividend resurfaces, and `is_applied("HWM", …)` does not match the `AA` row — **the
dividend is applied a second time.** The E1 inception-derived window makes this reachable,
because the window now reaches back to position inception rather than 14 days.

**Fix (`src/live/corporate_actions_audit_log.cpp`, `load()`):** after loading dedup rows,
read `ticker_aliases` and mirror every entry under its current symbol, following rename
chains (A→B→C) with a bounded loop so a cyclic map cannot spin. Read-side only — no rows
are rewritten, so the fix is idempotent and cannot corrupt the record it protects. Both the
old and new keys match, so a book that has not yet been re-keyed is equally covered.

An unreadable alias map **fails closed** (error, not empty), for the same reason the dedup
read does: a missing alias map silently reopens the re-application path.

**Pre-fix test failure, captured by disabling only the mirror insertion:**

```
[ RUN      ] CorpActionRenameBridge.AppliedEventIsNotReAppliedUnderTheNewTicker
Value of: after.is_applied("HWM", "2016-05-10", CorpActionType::DIVIDEND)
  Actual: false
Expected: true
dividend applied under AA must be seen as applied under HWM; otherwise it is
applied twice and the cost basis is permanently wrong
[  FAILED  ] CorpActionRenameBridge.AppliedEventIsNotReAppliedUnderTheNewTicker
```

## 2.2 CONFIRMED GAP — position write and dedup write are not atomic. **REPORTED, not fixed.**

`apps/strategies/live_equity_mean_reversion.cpp`:

```
:953  audit_log.record(adj)      -> in-memory + pending_
:973  db->store_positions(...)   -> adjusted positions PERSISTED
:982  audit_log.save()           -> dedup rows persisted; on failure ERROR + return 1
```

If `save()` fails, **the adjusted positions are already committed** while the dedup rows
are not. The next run sees adjusted positions with no dedup record and re-applies. The
abort at `:985` does not undo the position write, and the existing error text
("next run may double-apply events") acknowledges exactly this.

Reversing the order does not solve it — it converts double-application into
under-application, which corrupts the basis just as permanently in the other direction.

**Correct fix: make the two writes atomic** (a single transaction spanning
`store_positions` and `store_applied_corp_actions`, both in the same database), or a
compensating delete of the just-written dedup rows on position-write failure.

**Not fixed here because `store_positions` is shared with the futures live path**, and
changing its transaction handling is exactly the futures-affecting change HD prohibited
without proof of safety. Recorded for the equities/shared work with the fix shape above.
Mitigating factors: the run aborts rather than continuing, and the window is one batch.

## 2.3 VERIFIED SAFE — with the specific evidence

**Key equivalence (in-memory vs DB).** DB PK is 6 columns:
`(portfolio_id, strategy_id, strategy_name, symbol, action_type, ex_date)`. The read
supplies the first three in the `WHERE` clause (`load_applied_corp_actions(portfolio_id,
strategy_id, strategy_name)`), and the in-memory `AppliedKey` is exactly the remaining
three: `std::tuple<symbol, ex_date, CorpActionType>` (`corporate_actions_audit_log.hpp:134`),
checked by `is_applied(symbol, ex_date, action)` (`:175-179`). 3 + 3 = 6. **The
decomposition is airtight**, and §2.1's fix widens the symbol dimension rather than
narrowing it.

**Action-type round-trip.** `record()` writes `type_to_string(adj.type)` → `"DIVIDEND"`;
`load()` parses with `type_from_type_string` → `DIVIDEND` (`:115`). These are true
inverses. The vendor-form parser (`type_from_action_string`, `"dividend"`) is a separate
function used only where vendor strings arrive. The duplicate copy of this mapping was
removed in `278af5c4`, closing the door on the original `DIVIDEND`/`dividend` defect.

**PK granularity per event type.** Two dividends on the same symbol at different ex-dates
are distinct rows (`ex_date` in the key). The same ex-date under two strategy names are
distinct rows (`strategy_name` in the key — and the read now filters on it, fixed in
`d7f95bb5`). A split and a dividend sharing a bar are distinct rows (`action_type` in the
key), so the split-before-dividend ordering cannot cause one to mask the other.

**All dedup consumers enumerated** — `load()` has exactly three call sites and
`is_applied()` one:

| Site | Role | Behaviour on read failure |
|---|---|---|
| `:727` | position-adjusting | **aborts the run** (`return 1`) — `942bfe32` |
| `:888` | `is_applied` guard in the apply loop | reached only after `:727` succeeded |
| `:2181` | dividend-income reporting | WARN, reports 0 |
| `:2477` | dividend income for the email | WARN, reports 0 |

The two reporting sites run *after* trading decisions are persisted and are
informational-only, so failing a completed run there would be worse than under-reporting.
**No path consumes dedup state and still proceeds to adjust positions on a failed read.**

**Legacy file → DB import.** Runs only when the DB read succeeded *and* returned zero rows
(`:103`), so it cannot overwrite existing DB state; the file is imported and then left in
place rather than deleted; the subsequent re-read is itself fail-closed (`:105-112`). A
malformed file is a parse failure on a path that only executes when the DB is empty, so the
worst case is a first run that re-applies — the same as having no record at all.

**Ordering within a run.** Splits are emitted before dividends on a shared bar so the
per-share amount lands on post-split shares. Dedup records both independently
(`action_type` in the key), so a crash between them leaves the split recorded and the
dividend not — the next run correctly skips the split and applies the dividend. Consistent,
not corrupting. The genuine ordering hazard is §2.2, which is about the position/dedup
boundary rather than the split/dividend boundary.

## 2.4 Dedup summary

**7 links checked, all accounted for — 1 bug found and FIXED (rename bridge), 1 gap found
and REPORTED with a fix shape (non-atomic position/dedup write, blocked on a shared-path
change), 5 verified safe with the evidence stated above.**

---

# Suite

`1413/1413` green (1411 before this sweep + 2 rename-bridge tests).
