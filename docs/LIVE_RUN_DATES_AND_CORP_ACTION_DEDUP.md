# Live run dates and corporate-action de-duplication

Current-state reference for how the live equity runner
(`apps/strategies/live_equity_mean_reversion.cpp`) and the helpers it shares with the
futures runners decide *which day a run is*, *which rows it reads*, and *which corporate
actions it has already applied*. Section 6 states where the futures runners
(`apps/strategies/live_portfolio_conservative.cpp`, twin `live_portfolio.cpp`) differ.

For what the corporate-action feed can and cannot say, and over which dates, see
`docs/CORP_ACTIONS_DATA_BOUNDARY.md`. This document does not repeat it.

## 1. The run date

| Question | Answer |
|---|---|
| How is an explicit date read? | `YYYY-MM-DD` argv is parsed by `core::parse_utc_date` to **UTC midnight** (`00:00Z`). That instant is `now` for the whole run. |
| What does a bare invocation do? | `now` is the wall clock, email is turned **on** (`send_email = true` when no date override is present), logs go to the flat `logs/` directory. A dated run keeps email off unless `--send-email` is passed and logs under `logs/<YYYY-MM-DD>/`. |
| What gets stamped with the run date? | Every row this run writes: `trading.positions.last_update = now`, executions' `fill_time = now`, `live_results` and `equity_curve` dates, and every dedup row's `run_date`. A dated run therefore stores `00:00:00Z` timestamps. |
| Order ids | `ExecutionManager::generate_date_string` formats the fill time with `gmtime_r`, so an execution is `DAILY_<SYMBOL>_<YYYYMMDD>` in the UTC date of the run. This id is the key `delete_stale_executions` uses to replace a re-run day's fills. |
| Data cut-off | A dated run loads bars with `end_date = now - 24h`; a live run loads through `now`. |

Everything downstream renders `now` through `gmtime_r`/`format_utc_date`, so the run's
calendar date is the same on every host time zone.

## 2. Previous trading day

`HolidayChecker::find_previous_trading_day(now)` in
`include/trade_ngin/core/holiday_checker.hpp` starts at `now - 24h` and walks back one
calendar day at a time, up to 14 days, returning the first candidate that is neither a
Saturday/Sunday nor a date in the holiday calendar. Both the weekday test and the date
string are computed with `gmtime_r`, so the candidate is tested in the same frame it is
returned in.

| Run date | `previous_date` |
|---|---|
| Tuesday–Friday | the prior weekday, unless it is a holiday |
| Monday | the preceding Friday |
| day after a holiday | the last open day before the holiday |
| Saturday / Sunday | Friday |
| no open day in 14 days | `std::nullopt`; the runner logs an error and exits 1 |

**Calendar coverage guard.** The calendar (`config/market_holidays.json`, produced by
`scripts/generate_market_holidays.py`) covers a finite set of years. `is_holiday` answers
`false` outside that range because the answer is unknown, so before any trading-day
arithmetic the runner checks `holiday_checker.covers_date(today)` and exits 1 if the
year is not loaded. A calendar that fails to load at all is also fatal.

The runner logs one line, `Resolved previous trading day = D for run date T`, with the
number of calendar days walked.

**Closed-day runs.** `LiveDailyCycle::is_non_trading_day(now_tm, holiday_checker)`
(`include/trade_ngin/live/live_daily_cycle.hpp`) is the single predicate for "is today
a weekend or holiday". When it is true the run does not generate signals or executions;
it writes `positions`, `live_results` and `equity_curve` from
`LiveDailyCycle::carry_forward(previous_positions)`, which copies quantity, basis and
mark unchanged and sets `realized_pnl = 0` (the column is a daily flow, and a closed day
realizes nothing). This applies to dated replays as well as live runs.

## 3. Which rows the loader reads

`PostgresDatabase::load_positions_by_date` (`src/data/postgres_database.cpp`) is the
only way the runner reads a prior book. Its query is

```sql
SELECT symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl, last_update
FROM trading.positions
WHERE strategy_id = $1 AND strategy_name = $2 AND portfolio_id = $3
  AND DATE(last_update) = DATE($4)
```

| Key part | Value the equity runner passes |
|---|---|
| `strategy_id` | `LIVE_EQUITY_MEAN_REVERSION` |
| `strategy_name` | `EQUITY_MEAN_REVERSION` |
| `portfolio_id` | the configured portfolio (defaults to `BASE_PORTFOLIO` when empty) |
| `$4` | `previous_date` from section 2, formatted in UTC by `format_timestamp` |

The match is exact-date, not "most recent on or before". The `Timestamp` contract at the
top of `include/trade_ngin/data/postgres_database.hpp` is that every timestamp and every
`YYYY-MM-DD` key crossing the DB layer is UTC; `last_update` is `timestamptz` and is
read back from a UTC session and parsed with `parse_utc_datetime`. So `DATE(last_update)`
is the UTC calendar date, which is why the `00:00Z` stamp in section 1 matters.

A seed or manually inserted row must carry all four key parts exactly: the same
`strategy_id`, `strategy_name` and `portfolio_id`, and a `last_update` whose UTC date is
the previous trading day of the first run that should see it. A row on any other date,
or under a different `strategy_name`, is invisible to the loader.

## 4. Trading-day count and annualisation

`trading.get_trading_days(strategy_id, target_date, portfolio_id)`
(`migrations/004_get_trading_days_portfolio_scope.sql`) returns

```
GREATEST(1, (target_date - live_start_date) + 1)
```

where `live_start_date` comes from `trading.strategy_trading_days_metadata` for that
`strategy_id` **and** `portfolio_id` (earliest row if several). If no metadata row
exists it falls back to `MIN(DATE(date))` over `trading.live_results` for the same
strategy and portfolio, and if that is empty too it returns 1.

This is a **calendar-day count**, not an exchange-session count: weekends and holidays
are included. The equity runner calls the 3-argument form, and the since-inception
metrics (`live_metrics_calculator.cpp`, `years = trading_days / 252.0`) consume that
count as the code comments describe. Because the count is calendar days, the
"annualized" figures for a book that runs five days a week are computed over a longer
denominator than a session count would give.

Two operational rules follow:

- A metadata row must exist per `(strategy_id, portfolio_id)` before the first run, with
  `live_start_date` equal to the book's first day. A row dated *after* the book's first
  day is worse than none: the function stops at the first row it finds.
- The equity runner compares the metadata anchor against `MIN(date)` in `live_results`
  for its portfolio; if the anchor is later it logs a warning and uses the earlier date
  for that run. The two-argument form of the function still exists for the futures
  runners and keys on `strategy_id` alone.

## 5. Corporate-action de-duplication

`trading.corp_action_applied` (`migrations/002_corp_action_applied.sql`, extended by
`005_*` and `006_*`) is the durable record of which events have already changed the
book. `CorporateActionsAuditLog` (`src/live/corporate_actions_audit_log.cpp`) is its
only reader and writer.

| Column | Meaning |
|---|---|
| `portfolio_id`, `strategy_id`, `strategy_name` | which book; supplied by the `WHERE` on load |
| `symbol`, `action_type`, `ex_date` | the in-memory `AppliedKey`; together with the three above they form the primary key |
| `run_date` | the run date (section 1) of the pass that wrote the row; NULL on rows older than the column |
| `basis_ratio` | the factor the event divided the cost basis by (`PositionAdjustment.ratio_change`); NULL for a TERMINATION, which restates no basis |
| `applied_at`, `qty_held`, `dividend_per_share`, `total_cash` | audit detail; dividend cash is informational and never added to P&L |

**A row means "this event's effect is in the stored position."** Rows are written by
`record()` and persisted by `save()` only when the applier actually adjusts a position.
A deferred event (for example a spinoff with no price yet) or a refused event writes
nothing, so it resurfaces on the next run's window and is retried.
`load()` also mirrors every row under the symbol's current ticker via
`equities_data.ticker_aliases`, so an event applied under an old name is still seen as
applied after a rename. A dedup record that cannot be read (table or alias map) is an
error and the run exits 1 rather than adjusting against an empty applied-set.

**The event window.** The runner asks for events over `[window_start, today]`, with
`window_start` derived from position inception rather than a fixed 14 days, so a missed
run does not lose events; the dedup rows are what stop the reach-back from re-applying.

**Two replay guards.** Both exist because a dedup row from a *later* pass would make
the current pass skip an event whose effect is not in the T-1 row it just loaded.

1. Inline, after load: if `latest_applied_ex_date() >= today`, the runner exits 1. A
   row with today's or a future ex-date can only have been written by a later pass.
2. On load, with `RunDateCheck::Enforce` (the default after `set_run_date(today)`):
   any row whose `run_date >= today` is reported and `load()` returns an error, so the
   runner exits 1. This catches the case the first guard cannot see, where the later
   pass applied an event whose ex-date is already in the past. NULL `run_date` rows are
   accepted. The lifecycle (termination) instance later in the same run uses
   `RunDateCheck::StampOnly`, because the rows it would otherwise refuse are this run's
   own.

**Reset rule.** Neither guard deletes anything. A re-run of a day is valid only when the
book and the dedup rows come from the same pass, so a replay from date D resets
`trading.corp_action_applied` for the portfolio **together with** `positions`,
`live_results`, `equity_curve` and `executions` from D forward, then replays in order.
Resetting one without the other either double-applies (rows gone, book adjusted) or
skips (rows kept, book reset). Never re-run a day after a later day has run.

The position write (`store_positions`) and the dedup write (`save()`) are two
statements, not one transaction; `save()` failing after positions are stored is an
error that stops the run, and the operator resets both as above before continuing.

## 6. How the futures runners differ

| Aspect | Equity runner | Futures runners (`live_portfolio_conservative`, `live_portfolio`) |
|---|---|---|
| CLI date parse | `parse_utc_date`, UTC midnight | `std::get_time` + `std::mktime`, **local** midnight; on the deployed `America/New_York` host that is 04:00/05:00Z of the same calendar date |
| `now_tm` | `gmtime_r` | `std::localtime` |
| Run cadence | one run per calendar day; closed days are carry-forwards | **seven days a week**, every calendar day; the loader's exact-date match requires an unbroken daily chain |
| Previous day | `find_previous_trading_day` walk (section 2) | strict `now - 24h`; the shared `HolidayChecker` is loaded and `is_holiday(yesterday)` is checked, but no walk is performed |
| Saturday row | carry-forward, no executions | a normal trading cycle whose fills price at **Friday's settle** (T-1 = Friday) |
| Sunday row | carry-forward | `is_sunday` marks T-1 as non-trading; if no T-1 close is found the Saturday book is carried forward. Monday's run then finalizes Sunday against the Sunday stub bar, so the Sunday row carries the Friday-to-Sunday move for instruments that print on Sunday |
| Closed-market guard | `is_non_trading_day(today)` decides the carry-forward | `early_previous_day_close_prices.empty()` gates the carry-forward branch; a single instrument reporting a T-1 bar is enough to declare the market open, which is why 24/7 listings such as `MBT` can make a Sunday run trade |
| Order ids | `DAILY_<SYM>_<YYYYMMDD>` via the shared `generate_date_string` (UTC) | same function; the local-midnight parse still reads as the same date in UTC |
| Trading-day count | 3-argument `get_trading_days` with `portfolio_id` | 2-argument form keyed on `strategy_id` |
| Corp-action dedup | `trading.corp_action_applied` | not used; futures positions carry no corporate actions |
