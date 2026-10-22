# The live run cycle

Current-state reference for one live run: which day a run is, which rows it reads, what it
writes and when a written row is final, how to read the P&L columns, and what a refused run
leaves behind. It covers the three live runners:

| Runner | Book | Source |
|---|---|---|
| `live_portfolio_conservative` | futures, one trend sleeve (`CONSERVATIVE_PORTFOLIO`) | `apps/strategies/live_portfolio_conservative.cpp` |
| `live_portfolio` | futures, two trend sleeves (`BASE_PORTFOLIO`, a placeholder test book) | `apps/strategies/live_portfolio.cpp` |
| `live_equity_mr` | equities, mean reversion | `apps/strategies/live_equity_mean_reversion.cpp` |

The two futures runners are twins: the same code at the same line numbers through the
execution step, differing in the configuration they load
(`apps/strategies/live_portfolio_conservative.cpp:161`, `apps/strategies/live_portfolio.cpp:161`).
Futures citations below name the conservative runner. Section 9 lists where the futures
runners and the equity runner differ.

Citations are `path:line`. A bare `:N` cites the file last named in the
same paragraph or table cell; in a table cell that names no file it cites the conservative
futures runner in a futures row or column and the equity runner in an equity row or column.

Related documents: `AVERAGE_PRICE_LIFECYCLE.md` (what `average_price` means on each book),
`CORP_ACTIONS_DATA_BOUNDARY.md` (corporate actions and their de-duplication),
`FUTURES_ROLLS.md` (roll legs), `COST_MODEL.md` (what a fill costs and netting),
`RISK_MODULES.md` (the risk overlay and its stored record), `OPTIMIZER_AND_RISK_DESIGN.md`
(sizing capital and the rebalance), `performance_upkeep.md` (operations: schedule, wrapper,
watchdog, logs).

## 1. The run date

| Question | Answer |
|---|---|
| How is an explicit date read? | Equity runner: `core::parse_utc_date` reads `YYYY-MM-DD` as **UTC midnight**, `00:00Z` (`apps/strategies/live_equity_mean_reversion.cpp:102`). Futures runners: `std::get_time` and `std::mktime` read it as **local midnight** (`apps/strategies/live_portfolio_conservative.cpp:115-117`); on the deployed image, whose time zone is `America/New_York` (`Dockerfile:85`), that is **05:00Z of the same calendar date, all year** (the parse leaves `tm_isdst` at 0, so standard time applies in summer as well). That instant is `now` for the whole run. |
| What does a run without a date do? | `now` is the wall clock and email is turned **on** (`apps/strategies/live_portfolio_conservative.cpp:128-131`, `apps/strategies/live_equity_mean_reversion.cpp:114-117`). A dated run keeps email off unless `--send-email` is passed. See section 11: a live run always carries a date. |
| What gets stamped with the run date? | Every row the run writes: `trading.positions.last_update = now`, an execution's `execution_time = now`, the `live_results` and `equity_curve` dates, `trading.signals.timestamp`, and (equities) every de-duplication row's `run_date`. A dated equity run stores `00:00:00Z`; a dated futures run stores local midnight, which is `05:00:00Z` on the New York image. A run without a date stores the wall clock's time of day (section 1.2). |
| Execution ids | `ExecutionManager::generate_exec_id` builds `EXEC_<symbol>_<YYYYMMDD>` and the order id is `DAILY_<symbol>_<YYYYMMDD>`, both from `generate_date_string`, the UTC date of the run (`src/live/execution_manager.cpp:146`, `:202`, `:230-250`). The order id is the key `delete_stale_executions` uses to replace a re-run day's fills (`src/data/postgres_database_extensions.cpp:60-66`). Section 9 lists the futures-only suffixes. |
| Data cut-off | A dated run loads bars with `end_date = now - 24h`; a run without a date loads through `now` (`apps/strategies/live_portfolio_conservative.cpp:319`, `apps/strategies/live_equity_mean_reversion.cpp:258`). |
| Instants versus dates | A stored daily bar is stamped `D 00:00:00Z`. In memory a bar's instant is not the stored one: four parse sites run the stored text through `mktime`, `gmtime`, `mktime` (`src/data/postgres_database.cpp:213-215`, `:1381-1383`, `:1887-1889`, `:1931-1936`), which on a New York host yields `D 10:00Z` and on a host at UTC leaves `D 00:00Z`. The date is preserved in both, and every date key is derived by flooring to the UTC day, so on a host at UTC or west of it the shift changes no date (section 1.1). It is visible in stored rows that carry a bar's instant: backtest rows on a New York host are stamped `10:00:00Z`. Live rows carry the run instant, not a bar's: `00:00Z` on a dated equity run, and `05:00Z` on a dated futures run on the New York image. **A reader must render and key every stored instant on its UTC date**, never on a local day. |

### 1.1 The run date and the host

A live run takes an optional date, `YYYY-MM-DD`. The two futures runners read it as midnight of that date in
the host's local time (`apps/strategies/live_portfolio_conservative.cpp:113-117`); the equity runner reads it
as midnight UTC (`apps/strategies/live_equity_mean_reversion.cpp:101-104`,
`include/trade_ngin/core/time_utils.hpp:134-153`). With no date the run clock is the wall clock
(`apps/strategies/live_portfolio_conservative.cpp:311` in the futures runners,
`apps/strategies/live_equity_mean_reversion.cpp:242` in the equity runner).

Every stored key is rendered from that instant in UTC: the date and timestamp columns go through
`PostgresDatabase::format_timestamp` (`src/data/postgres_database.cpp:472-479`) and
`core::format_utc_date` (`include/trade_ngin/core/time_utils.hpp:71`), and so do the missed-run check, the
calendar check and the feed measure in the runners. On a host at UTC, or west of it, local midnight falls on
the same UTC date, so the stored date is the date given. On a host east of UTC a futures run stores the day
under the previous date.

The futures runners read three things through `now_tm`, the pointer `std::localtime` returned for the run
instant (`apps/strategies/live_portfolio_conservative.cpp:313`): the Sunday test behind one log line
(`:1079-1080`, `:1593`), the date printed on the console (`:2741-2743`), and the date in the report's subject
and in the body file's name (`:4409-4416`). The structure behind the pointer is not copied. The Sunday test is
the first of the three reads, above the runner's own first `std::gmtime` call (`:1098`). The console date is
read after that call, and the subject date after the calls at `:3088`, `:3861` and `:4039` as well; the
storage and export code calls `std::gmtime` too. A C library may keep one structure for both functions: there
the two dates are the fields the latest such call left, a UTC rendering of the instant that call was given. They are the host's local date
of the run instant only where the library keeps the results of the two functions apart. A fourth thing is
taken from the host's local time on a structure of its own (`localtime_r`) and is always the host's local
date: the test of whether the run date is the host's date, which decides whether a held symbol without bars
stops the run (`include/trade_ngin/live/session_book_gate.hpp:159-178`). No stored key passes through
`now_tm`: on a run with no date the stored keys are the UTC date of the run instant, which differs from the
host's local date whenever the two dates differ.

The equity runner renders its own date in UTC everywhere (`apps/strategies/live_equity_mean_reversion.cpp:242-251`),
so its date handling does not depend on the host's time zone. One step is shared by all three runners and
does depend on it: the database layer turns each bar's stored time into an instant through the host's local
time (`src/data/postgres_database.cpp:1376-1383`). The instant a bar gets is the stored one moved by twice the
host's offset from UTC, when the database session renders the stored time in UTC: unchanged on a host at UTC,
later on the same UTC date on a host behind UTC by less than twelve hours, so a bar keeps its date on both.

The backtest runners take no date. Their window ends at the wall clock, or at midnight of
`backtest.frozen_end_date` in the host's local time when the key is set, and starts
`backtest.lookback_years` earlier on the host's local calendar (`src/core/config_loader.cpp:807-866`).

No runner sets the zone or checks it: each takes it from the environment of its process. The image in this
repository sets `TZ=America/New_York` (`Dockerfile:85`), and the entrypoint passes `TZ` on to the scheduled
job (`scripts/docker-entrypoint.sh:28-34`, `scripts/run_live_portfolio.sh:31-34`). That zone is west of UTC:
the stored date is the date given, a dated futures run stores `05:00:00Z` in every month of the year (the
parse leaves the daylight flag at 0, so local midnight is read as standard time), and a bar keeps its date,
with the in-memory instant `D 10:00Z`.

A book is run under one zone for its whole life, because the run instant is part of what is stored and of
what is read. The same date run under another zone has another instant (`00:00:00Z` on a host at UTC), and
on a futures book this follows:

- The bar window starts `live.historical_days` times 24 hours before the run instant
  (`apps/strategies/live_portfolio_conservative.cpp:316`) and is read with `time BETWEEN` its two instants
  against bars stamped `00:00:00Z`. At `00:00:00Z` the bar on the start edge is inside the window; at
  `05:00:00Z` it is not. The window differs by one bar a symbol.
- The run of D rewrites the `trading.equity_curve` point of D-1 through a key that includes the exact
  instant (section 3). Under the other zone it does not match the stored point and a second point is
  inserted, so the day before the change has two.
- The key of `trading.signals` includes the instant as well, so a date run again under the other zone keeps
  the signal rows of both runs.
- `trading.positions.last_update` and an execution's `execution_time` carry the other time of day.

The comment above the date parse lists these effects
(`apps/strategies/live_portfolio_conservative.cpp:81-112`).

### 1.2 The three run modes

A live run is in one of three modes, by its arguments and by the host's date: no date; a date equal to the
host's date (the scheduled run); a past date (a replay or a catch-up). A date later than the host's date is
accepted and behaves as a past date.

| Behaviour | No date | Date equal to the host's date | Past date |
|---|---|---|---|
| Run clock | the wall clock, with its time of day | midnight of the date (host local in the futures runners, UTC in the equity runner) | the same as for the host's date |
| Bar window | ends at the run clock, so a bar dated the run day, if already loaded by the feed, is read | ends 24 hours before the run clock: bars up to the day before | the same as for the host's date |
| Previous-day price of a symbol (futures) | missing for any symbol whose last bar read is dated the run day | present when the symbol has a bar for the day before | the same as for the host's date |
| A requested symbol with no bar at all | the run refuses, exit 1 | a warning; the run goes on | a warning; the run goes on |
| No symbol with a usable bar date | the run refuses, exit 1 | a warning; the run goes on | a warning; the run goes on |
| The stalest symbol more than `live.data_staleness_tolerance_days` behind | the run refuses, exit 1; measured to the run day | a warning; the run goes on; measured to the day before | a warning; the run goes on; measured to the day before |
| A held symbol with no bar for more than `live.data_staleness_tolerance_days` (futures runners only) | the run refuses, exit 1 | the run refuses, exit 1 | a warning; the run goes on |
| Email | always sent | sent only with `--send-email` | sent only with `--send-email` |
| `TRADE_NGIN_EMAIL_BODY_DIR` (futures runners only) | ignored, with one warning if it is set | used only without `--send-email`; with it, ignored with one warning | the same as for the host's date |
| Stored timestamps | carry the wall clock's time of day, so they differ on every run | one fixed instant per date, the same on every run of that date | the same as for the host's date |
| Console and log | prints the processing time; the futures runners' log says run type LIVE | no processing time; the futures runners' log says run type HISTORICAL | the same as for the host's date |
| Log directory (equity runner only) | `logs/` | `logs/<date>/` | `logs/<date>/` |

Everything else is the same in all three modes: the missed-run check, the listing-date check, the margin
metadata check, the holiday calendar checks, the session classifier, the roll state, the stored-book check,
the sizing read, the carry of a day with no session, the strict assertion and every store. The scheduled run
always passes a date and `--send-email` (`scripts/run_live_portfolio.sh:71-74`). No code refuses a run with
no date; `scripts/dev_build_run.sh` starts the binary it builds with no argument.

## 2. The previous day, and closed days

**Equity runner.** `HolidayChecker::find_previous_trading_day(now)`
(`include/trade_ngin/core/holiday_checker.hpp:253`) starts at `now - 24h` and walks back one
calendar day at a time, up to 14 days, returning the first candidate that is neither a
Saturday or Sunday nor a date in the holiday calendar. The weekday test and the date string
are both computed with `gmtime_r`, so the candidate is tested in the frame it is returned in.

| Run date | `previous_date` |
|---|---|
| Tuesday to Friday | the prior weekday, unless it is a holiday |
| Monday | the preceding Friday, unless it is a holiday |
| day after a holiday | the last open day before the holiday |
| Saturday or Sunday | Friday, unless it is a holiday |
| no open day in 14 days | `std::nullopt`; the runner logs an error and exits 1 |

The runner logs one line, `Resolved previous trading day = D for run date T`, with the number
of calendar days walked (`apps/strategies/live_equity_mean_reversion.cpp:364-371`), and the
price manager is keyed on the same resolved day (`:1049`).

`LiveDailyCycle::is_non_trading_day(now_tm, holiday_checker)`
(`include/trade_ngin/live/live_daily_cycle.hpp:52`) is the single predicate for "is today a
weekend or holiday". When it is true the equity run generates no signal and no execution; it
writes `positions`, `live_results` and `equity_curve` from `LiveDailyCycle::carry_forward`
(`include/trade_ngin/live/live_daily_cycle.hpp:71`), which copies quantity, basis and mark
unchanged and sets the realized figure to 0 (the column is a daily flow, and a closed day
realizes nothing). This applies to dated runs as well.

**Futures runners.** The previous day is strictly `now - 24h`. There is no walk: the runner
runs every calendar day and the loader's exact-date match requires an unbroken daily chain.
Whether that day was a session is decided per symbol by the session classifier, not by the
calendar (section 9).

**Calendar coverage.** The calendar (`include/trade_ngin/core/holidays.json`, produced by
`scripts/generate_market_holidays.py`; the path can be overridden with `TRADE_NGIN_HOLIDAYS_JSON`,
`include/trade_ngin/core/holiday_checker.hpp:71-85`) covers a finite set of years (2000 to 2035 in
the tracked file). `is_holiday` answers
`false` outside that range because the answer is unknown, so every runner checks
`covers_date` before any trading-day arithmetic and exits 1 if the year is not loaded
(`apps/strategies/live_equity_mean_reversion.cpp:334-341`; the futures runners check the run
date and the previous day, `apps/strategies/live_portfolio_conservative.cpp:1117-1134`). A
calendar that fails to load at all is also fatal (`:1089-1095`). These checks run on every
run, whether or not a date is given, and before the day's `live_run_metadata` row is written,
so a calendar refusal stores nothing.

## 3. Which rows the loader reads

`PostgresDatabase::load_positions_by_date` (`src/data/postgres_database.cpp:972`) is the only
way a runner reads a prior book. Its query is

```sql
SELECT symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl,
       last_update, instrument_id
FROM trading.positions
WHERE strategy_id = $1 AND strategy_name = $2 AND portfolio_id = $3
  AND DATE(last_update) = DATE($4)
```

(`src/data/postgres_database.cpp:1005-1012`; with an empty `strategy_name` the sleeve
predicate is dropped and the rows of every sleeve are returned, `:1022-1028`).

| Key part | Equity runner | Futures runners |
|---|---|---|
| `strategy_id` | `LIVE_EQUITY_MEAN_REVERSION` (`apps/strategies/live_equity_mean_reversion.cpp:59`) | `LIVE_` followed by every sleeve enabled for live trading, sorted and joined by `_` (`apps/strategies/live_portfolio_conservative.cpp:288-300`): `LIVE_TREND_FOLLOWING` on the conservative book, `LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST` on the two-sleeve book |
| `strategy_name` | `EQUITY_MEAN_REVERSION` (`:60`) | the sleeve, for example `TREND_FOLLOWING`: one set of rows per sleeve |
| `portfolio_id` | the configured portfolio (`BASE_PORTFOLIO` when the loader is given an empty one, `src/data/postgres_database.cpp:999`) | the configured portfolio |
| `$4` | `previous_date` from section 2 | `now - 24h` |

The match is exact-date, not "most recent on or before". The contract at the top of
`include/trade_ngin/data/postgres_database.hpp` is that every timestamp and every `YYYY-MM-DD`
key crossing the database layer is UTC; `last_update` is `timestamptz`, read back and parsed
with `parse_utc_datetime` (`src/data/postgres_database.cpp:1054`). The engine does **not** set
the session time zone on its connections: `DATE(last_update)` and every `DATE(date)` and
`DATE(timestamp)` predicate return the UTC calendar date only when the database session is in
UTC. The database must therefore be configured with a UTC time zone.

The ids are matched exactly. A row stored under any other `strategy_id` (for example one whose
names are joined by `&`, a format no runner in this tree writes) is invisible to the loader. A
futures run also refuses to start when the previous book holds positions under a strategy id
or a sleeve the run does not load (`include/trade_ngin/live/stored_book_ownership.hpp`,
`apps/strategies/live_portfolio_conservative.cpp:1447-1461`).

A seed or manually inserted row must carry all four key parts exactly: the same
`strategy_id`, `strategy_name` and `portfolio_id`, and a `last_update` whose UTC date is the
previous day of the first run that should see it (the previous trading day for equities, the
previous calendar day for futures).

### Re-running a date: what is replaced, what accumulates

| Table | A second run of date D |
|---|---|
| `trading.positions` | **Replaced** per sleeve: the writer deletes the rows of (`strategy_id`, `strategy_name`, `portfolio_id`, `DATE(last_update)`) and inserts (`src/data/postgres_database.cpp:605-611`, `:694-701`). The D-1 rows are rewritten the same way. A futures sleeve that stores no row on the second run deletes nothing, so its earlier rows of D stay (`apps/strategies/live_portfolio_conservative.cpp:2954-2978`); the equity runner clears its date explicitly (`apps/strategies/live_equity_mean_reversion.cpp:5959`). |
| `trading.executions` | **Replaced by order id**: rows with the order ids of the second run's fills are deleted for the sleeve and portfolio, then inserted (`src/data/postgres_database_extensions.cpp:60-66`). A sleeve's `ROLL` rows of the date are swept by date first (`:93-96`). A `STRATEGY` fill the second run no longer makes keeps its earlier row. |
| `trading.live_results` | **Replaced**: deleted by (`strategy_id`, `portfolio_id`, `DATE(date)`) and inserted (`src/storage/live_results_manager.cpp:111-131`, `src/data/postgres_database_extensions.cpp:753-756`, `:900-903`). The D-1 row is updated in place. |
| `trading.equity_curve` | The D point is **replaced** by UTC date (`src/data/postgres_database_extensions.cpp:803-806`). The D-1 point is rewritten through `ON CONFLICT (portfolio_id, strategy_id, timestamp, portfolio_type)` on the exact instant `now - 24h` (`src/data/postgres_database.cpp:2754-2758`): two runs of D at different instants would leave two D-1 points. Dated runs always share the instant. |
| `trading.signals` | **Never deleted.** Upserted on (`portfolio_id`, `strategy_id`, `strategy_name`, `symbol`, `timestamp`), the exact instant (`src/data/postgres_database.cpp:775-781`). Dated runs of one date overwrite; runs at different wall-clock instants accumulate, and a reader takes the last instant per UTC day. |
| `trading.live_run_metadata` | **Upserted** on (`date`, `strategy_id`, `portfolio_id`) (`src/data/postgres_database_extensions.cpp:951-963`). |
| `trading.corp_action_applied` | **Kept.** A row whose `run_date` is D or later makes the run refuse, exit 1 (`src/live/corporate_actions_audit_log.cpp:141-145`): a second run of D is refused if the first run of D applied a corporate action, until the rows and the book are reset together (see `CORP_ACTIONS_DATA_BOUNDARY.md`). |

Replacing a date does not make a lone re-run valid: every later date was computed from the
rows the first run left. Section 11 states the rule.

## 4. The P&L frame

### 4.1 A row dated D is final only after the run of D+1

A run dated D loads bars through D-1, prices its fills at the D-1 close and stamps its rows D.
It cannot know the P&L of the book it just stored, because the bar that marks it does not
exist yet. The run dated D+1 loads that bar and writes the P&L back onto the D rows.

| Row | Written by the run of D | Written by the run of D+1 |
|---|---|---|
| futures `positions` dated D | quantity, `average_price` = the D-1 close, `daily_realized_pnl` = 0, `daily_unrealized_pnl` = 0 (`apps/strategies/live_portfolio_conservative.cpp:2890-2901`) | `daily_realized_pnl` = the settled move of the D bar (`:2249-2255`, stored at `:2725`) |
| futures `live_results` dated D | `daily_realized_pnl` = 0, `daily_pnl` = minus the day's transaction costs (`:3050-3053`), totals carried from the previous row | realized, `daily_pnl`, `total_pnl`, `total_realized_pnl`, `current_portfolio_value`, the returns (`:3294-3360`) and the since-inception statistics (`:3581-3606`); the `equity_curve` point of D (`:3483`) |
| equity `positions` dated D | quantity, cost basis, that day's trade-realized P&L (final), the mark as of the fill | `daily_unrealized_pnl` against the D close (`apps/strategies/live_equity_mean_reversion.cpp:3758`, stored at `:3838`); the realized figure is kept |
| equity `live_results` dated D | `daily_realized_pnl` and the day's costs (final) | `total_unrealized_pnl`, `daily_unrealized_pnl`, `daily_pnl`, `total_pnl`, `current_portfolio_value`, the returns (`:4934-4967`) |

So the newest row of any book is provisional. On a futures book it shows a realized P&L of
exactly 0 and a `daily_pnl` equal to minus the day's costs; that is "not yet settled", never
"flat day". This is also why runs must be consecutive: a missed run leaves its previous day
unsettled and the next run without the book it sizes from.

### 4.2 Two accounting models

`LivePnLManager::UnrealizedPolicy` (`include/trade_ngin/live/live_pnl_manager.hpp:40-61`)
names them.

| | Futures: SETTLED | Equities: MARK_TO_MARKET |
|---|---|---|
| `average_price` on a row | the D-1 close the row was entered or settled at; reset every day | the true weighted cost basis; 0 on a closed row |
| P&L of holding over bar D | all of it is **realized** on the D row: quantity × (close(D) - previous consumed close) × point value | the change in the **unrealized** level: quantity × (close(D) - basis), compared with the level before |
| Unrealized | 0 by identity, on every row | a level on the position row; `live_results` carries both the level and its daily change |
| An exit | realizes 0 on the exit day (the fill is struck at the close the row was already settled at); a closed symbol has no row | realizes the whole accumulated gain on the exit day; a quantity-0 row is kept for that one date with `average_price` 0, to carry the figure |
| Passed at | `apps/strategies/live_portfolio_conservative.cpp:2254` | `apps/strategies/live_equity_mean_reversion.cpp:3758` |

`AVERAGE_PRICE_LIFECYCLE.md` explains the meanings of `average_price` and the rules that keep
them apart; this document does not repeat it. On a futures book the settled move reads the
symbol's **consumed** bars: a bar the classifier withholds and a roll's change bar book 0, and
a symbol with no bar on D books 0 for D and settles against its last consumed close when it
next prints (`include/trade_ngin/live/session_book_gate.hpp:205`; `FUTURES_ROLLS.md` covers
the roll cases).

### 4.3 Where the weekend move lands: futures

Daily futures bars are stamped by UTC date. Most contracts print a short bar dated Sunday (the
start of the trading week), a bar dated Saturday is rare, and the futures runner runs
every calendar day.

Worked example of the arithmetic: a replay of dated live runs of the conservative book on its
template configuration (with the listing-date, relabel and rule-removal blocks), `ZF.v.0`, held
short 2 contracts, 2026-04-24 to 2026-04-28. Point value 1,000 dollars per point. Closes from `futures_data.ohlcv_1d`: Thursday
04-23 108.09375, Friday 04-24 108.28125, no Saturday bar, Sunday 04-26 108.1640625, Monday
04-27 108.1953125, Tuesday 04-28 108.0546875.

| Row date | Day | `quantity` | `average_price` | `daily_realized_pnl` | Hand check | Settled by the run of |
|---|---|---|---|---|---|---|
| 2026-04-24 | Fri | -2 | 108.093750 | -375.000000 | -2 × (108.28125 - 108.09375) × 1,000 | Sat 04-25 |
| 2026-04-25 | Sat | -2 | 108.281250 | 0.000000 | no bar dated Saturday: nothing to settle | never (stays 0) |
| 2026-04-26 | Sun | -2 | 108.281250 | 234.375000 | -2 × (108.1640625 - 108.28125) × 1,000 | Mon 04-27 |
| 2026-04-27 | Mon | -2 | 108.164063 | -62.500000 | -2 × (108.1953125 - 108.1640625) × 1,000 | Tue 04-28 |
| 2026-04-28 | Tue | -2 | 108.195313 | 281.250000 | -2 × (108.0546875 - 108.1953125) × 1,000 | Wed 04-29 |

The same dates on the book's `trading.live_results` rows (one sleeve, so every netting
adjustment is 0):

| Row date | Day | `daily_realized_pnl` | `daily_transaction_costs` | `daily_pnl` | `total_pnl` | `current_portfolio_value` |
|---|---|---|---|---|---|---|
| 2026-04-24 | Fri | -161.500000 | 39.975496 | -201.475496 | -2,760.6022 | 497,239.3978 |
| 2026-04-25 | Sat | 0.000000 | 0.000000 | 0.000000 | -2,760.6022 | 497,239.3978 |
| 2026-04-26 | Sun | 234.375000 | 0.000000 | 234.375000 | -2,526.2272 | 497,473.7728 |
| 2026-04-27 | Mon | 6,143.125000 | 3.933885 | 6,139.191115 | 3,612.9639 | 503,612.9639 |
| 2026-04-28 | Tue | 4,312.875000 | 88.096891 | 4,224.778109 | 7,837.7420 | 507,837.7420 |

Hand checks: Friday -161.5 - 39.975496 = -201.475496; Sunday's total -2,760.6022 + 234.375 =
-2,526.2272; Tuesday 4,312.875 - 88.096891 = 4,224.778109 and 503,612.9639 + 4,224.7781 =
507,837.7420. Sunday's 234.375 is `ZF.v.0` alone: of the three held symbols with a bar dated
that Sunday it is the only one that settled a move. `MBT.v.0` booked 0 because that bar was the
change bar of a roll (its two `ROLL` legs are stored on 2026-04-28 with the id date 20260427,
the confirming bar). `6L.v.0` booked 0 because its Sunday bar, 107 lots, was withheld as a junk
print; its Monday row holds the whole Friday-to-Monday move, 2 × (0.2008 - 0.2004) × 100,000 =
80.00. The other held symbols had no bar dated that Sunday.

What to read from it:

- **Friday's row** holds the Friday session's move and is settled by Saturday's run. The
  Saturday run is a normal trading cycle whose fills price at Friday's close.
- **Saturday's row** settles nothing: with no bar dated Saturday no symbol has a T-1 price on
  Sunday's run, the whole book is carried (`apps/strategies/live_portfolio_conservative.cpp:1585`)
  and the Day T-1 update is skipped (`:3075-3077`). Its realized P&L stays 0 and its
  `daily_pnl` stays minus the costs of the Saturday run's fills. A contract that does print a
  Saturday bar (`MBT.v.0` from 2026-06-13) settles on Saturday's row like any other day.
- **Sunday's row carries the Friday-to-Sunday move** for every symbol that prints on Sunday,
  written by Monday's run against the previous consumed close, which is Friday's.
- **Monday's row** holds the move from the Sunday bar to Monday's close. A symbol with no
  Sunday bar is held on Monday's run (its T-1 verdict is not a session) and its Monday row
  holds the whole Friday-to-Monday move.

### 4.4 Where the weekend move lands: equities

Equities have no weekend bars and the equity runner treats Saturday, Sunday and holidays as
carry-forward days: the rows of those dates repeat the last session's book with a realized
figure of 0 and an unchanged mark.

Worked example of the arithmetic: a replay of dated live runs of the equity mean-reversion
book, starting capital 100,000, `TMUS`, 2026-07-02 to 2026-07-07 (Friday 07-03 is a market
holiday).

`trading.positions`:

| Row date | Day | `quantity` | `average_price` | `daily_unrealized_pnl` (level) | `daily_realized_pnl` (flow) |
|---|---|---|---|---|---|
| 2026-07-02 | Thu | 24.794787 | 173.970000 | 88.021494 | -0.730662 |
| 2026-07-03 | Fri (holiday) | 24.794787 | 173.970000 | 88.021494 | 0.000000 |
| 2026-07-04 | Sat | 24.794787 | 173.970000 | 88.021494 | 0.000000 |
| 2026-07-05 | Sun | 24.794787 | 173.970000 | 88.021494 | 0.000000 |
| 2026-07-06 | Mon | 23.961342 | 173.970000 | 187.377694 | 2.958730 |
| 2026-07-07 | Tue | 0.000000 | 0.000000 | 0.000000 | 187.377694 |

`trading.live_results`:

| Row date | `daily_realized_pnl` | `daily_unrealized_pnl` (change) | `daily_transaction_costs` | `daily_pnl` | `total_realized_pnl` | `total_unrealized_pnl` | `total_transaction_costs` | `total_pnl` | `current_portfolio_value` |
|---|---|---|---|---|---|---|---|---|---|
| 2026-07-02 | -0.730662 | 111.315394 | 1.010487 | 109.574245 | -339.4681 | 88.0215 | 71.829696 | -323.2763 | 99,676.7237 |
| 2026-07-03 to 07-05 | 0 | 0 | 0 | 0 | -339.4681 | 88.0215 | 71.829696 | -323.2763 | 99,676.7237 |
| 2026-07-06 | 2.958730 | 99.356194 | 1.010888 | 101.304036 | -336.5094 | 187.3777 | 72.840584 | -221.9723 | 99,778.0277 |
| 2026-07-07 | 187.377694 | -187.377700 | 1.319813 | -1.319818 | -149.1317 | 0.0000 | 74.160397 | -223.2921 | 99,776.7079 |

Hand checks, Monday 07-06: `daily_pnl` = 2.958730 - 1.010888 + 99.356194 = 101.304036;
`total_pnl` = -336.5094 - 72.840584 + 187.3777 = -221.9723; value = 100,000 - 221.9723 =
99,778.0277. The mark on 07-02 is 173.97 + 88.021494 / 24.794787 = 177.52, so Monday's sale of
0.833445 shares realizes 0.833445 × (177.52 - 173.97) = 2.958730.

What to read from it:

- **The closed days repeat Thursday's book**: the unrealized level is carried, its daily
  change is 0, and no P&L is booked on a day the market never opened
  (`apps/strategies/live_equity_mean_reversion.cpp:5384-5389`).
- **The whole move from the last session's close to Monday's close lands once, in Monday's
  row**, as the change in the unrealized level (99.356194), written by Tuesday's run.
- **A fill on the first open day is struck at the last session's close.** Monday's sale is
  priced at Thursday's 177.52, because Monday's run prices at its previous trading day.
- **The exit on Tuesday realizes the accumulated gain** (187.377694) and takes the unrealized
  level to 0 in the same row, so `daily_pnl` moves only by the day's costs. The quantity-0 row
  exists for that one date.

### 4.5 A deferred corporate action

A split or dividend is applied to an equity position only once its ex-date bar is inside the
loaded window; until then the event is deferred, no de-duplication row is written and the next
run reconsiders it (`apps/strategies/live_equity_mean_reversion.cpp:2503-2518`). When a
deferred event whose ex-date is on or before D-1 catches up, that symbol's D-1 row is
finalized from the restated book so that basis and price are in the same units
(`LiveDailyCycle::select_finalization_book`,
`include/trade_ngin/live/live_daily_cycle.hpp:785`). `CORP_ACTIONS_DATA_BOUNDARY.md` covers the
rest.

## 5. What each table holds

One row per table the runners read or write in the `trading`, `backtest` and `metadata`
schemas. "Key" is the constraint in the schema where one exists, and the writer's own replace
rule where it does not. Section 3 gives the re-run rules for the live tables in full.

| Table | One row is | Key | Written by, at which step | Final when; does a re-run replace it | Joins to | It is NOT |
|---|---|---|---|---|---|---|
| `trading.positions` | one sleeve's holding of one symbol on one run date | Primary key (`portfolio_id`, `strategy_id`, `strategy_name`, `date`, `symbol`, `portfolio_type`). The writer replaces by (`strategy_id`, `strategy_name`, `portfolio_id`, `DATE(last_update)`): delete, then insert (`src/data/postgres_database.cpp:605-611`). `portfolio_type` is left to its column default, `system`. | Futures runner: the D rows after the day's executions are stored (`apps/strategies/live_portfolio_conservative.cpp:2962`); the D-1 rows with their settled P&L just before (`:2725`). Equity runner: the D rows at the end of the run through `LiveResultsManager::save_all_results` (`src/storage/live_results_manager.cpp:78`), the D-1 rows after finalization (`apps/strategies/live_equity_mean_reversion.cpp:3838`), and rows restated by a corporate action (`:3061`, `:3485`). | After the run of D+1 (section 4.1). A re-run of D replaces the D rows and rewrites D-1. | `trading.executions` on (`portfolio_id`, `strategy_id`, `strategy_name`, `date`, `symbol`); `trading.live_results` on (`portfolio_id`, `strategy_id`, `date`), summing the sleeves; `futures_data.ohlcv_1d_raw` on `instrument_id`. | Not the account's net holding of a symbol when two sleeves hold it (there is one row per sleeve), and not a list of every symbol: a futures holding of 0 has no row. |
| `trading.executions` | one fill of one sleeve in one symbol on one run date, or one leg of a roll, with its own cost and its netting adjustment | Primary key (`portfolio_id`, `strategy_id`, `strategy_name`, `date`, `exec_id`). The writer replaces by `order_id` within (`strategy_name`, `portfolio_id`) (`src/data/postgres_database_extensions.cpp:60-66`). | Futures runner: after netting, before any position of the day: the sleeves with roll legs in one transaction (`apps/strategies/live_portfolio_conservative.cpp:2632`), then the others (`:2703`). Equity runner: at the end of the run (`src/storage/live_results_manager.cpp:81`). | Final when written; no later run revises it. A re-run of D replaces the rows whose order ids it produces again. | `trading.positions` as above; `futures_data.ohlcv_1d_raw` on `instrument_id` (roll legs only). | Not a broker fill: it is the modelled fill at the D-1 close. `total_transaction_costs` is the row's own cost; what the account is charged is that minus `netting_adjustment`. |
| `trading.signals` | one sleeve's signal for one symbol at one run instant | Unique (`portfolio_id`, `strategy_id`, `strategy_name`, `symbol`, `timestamp`); surrogate primary key `id`. Upsert on the unique key (`src/data/postgres_database.cpp:775-781`). | Futures runner: after the rebalance, before executions (`apps/strategies/live_portfolio_conservative.cpp:1940`). Equity runner: at the end of the run (`src/storage/live_results_manager.cpp:84`). | Final when written. A dated re-run overwrites; runs at other instants add rows. | `trading.positions` on (`portfolio_id`, `strategy_id`, `strategy_name`, `symbol`) and the UTC date of `timestamp`. | Not one row per day: no row is written on a day with no rebalance (a carried futures day, a sizing hold). `signal_value` is a capped forecast between -20 and +20 on a trend sleeve and a z-score on the equity book: the two are not comparable. |
| `trading.live_results` | one book's daily result: one portfolio and strategy id on one run date | Unique (`portfolio_id`, `strategy_id`, `date`); surrogate primary key `id`. The writer deletes by (`strategy_id`, `portfolio_id`, `DATE(date)`) and inserts (`src/data/postgres_database_extensions.cpp:753-756`, `:900-903`). | Both runners: at the end of the run, in `LiveResultsManager::save_all_results` (`src/storage/live_results_manager.cpp:87`, called at `apps/strategies/live_portfolio_conservative.cpp:4336`). The D-1 row is updated earlier in the same run (`:3294-3360`). | After the run of D+1. A re-run of D replaces the D row and updates D-1. | `trading.equity_curve` on (`portfolio_id`, `strategy_id`) and the UTC date; `trading.live_run_metadata` on (`date`, `strategy_id`, `portfolio_id`); `trading.positions` as above. | Not final on the day it is written: the row dated D shows no P&L for D until the run of D+1. Not one row per sleeve either: a two-sleeve book has one row. |
| `trading.equity_curve` | one book's account value at one run instant | Unique (`portfolio_id`, `strategy_id`, `timestamp`, `portfolio_type`); surrogate primary key `id`. Upsert on the unique key (`src/data/postgres_database.cpp:2754-2758`); the D point is first deleted by UTC date (`src/data/postgres_database_extensions.cpp:803-806`). | Both runners: the D point with `live_results` at the end (`src/storage/live_results_manager.cpp:91`); the D-1 point after the D-1 update (`apps/strategies/live_portfolio_conservative.cpp:3483`). | After the run of D+1. A re-run replaces the D point. | `trading.live_results` on (`portfolio_id`, `strategy_id`) and the UTC date of `timestamp`: `equity` equals `current_portfolio_value`. | Not keyed by date: the key is the instant, so read one point per UTC date. Its newest point excludes the newest day's P&L. |
| `trading.live_run_metadata` | the configuration one run used: allocations, the portfolio configuration and each sleeve's configuration, for one book and date | Unique (`date`, `strategy_id`, `portfolio_id`); surrogate primary key `id`. Upsert (`src/data/postgres_database_extensions.cpp:951-963`). | Futures runner: after every start-up guard and the sizing read have passed (`apps/strategies/live_portfolio_conservative.cpp:1554`); written again with a mark when the day is refused (section 10). Equity runner: after its start-up checks (one strategy, a symbol list, the calendar and the previous trading day, instrument registration, strategy start) and before its data guards, the market data load and the feed checks (`apps/strategies/live_equity_mean_reversion.cpp:689`; the feed checks are at `:896-940`). | Final at the end of the run. A re-run overwrites it. | `trading.live_results` on (`date`, `strategy_id`, `portfolio_id`). | Not proof that a run completed, and not written by every refused run (section 10). |
| `trading.strategy_trading_days_metadata` | the first live date of one strategy id in one portfolio | Primary key (`portfolio_id`, `strategy_id`). | No runner writes it: the row is seeded by hand before a book's first run (section 7). | Final once seeded. | Read by `trading.get_trading_days` on (`strategy_id`, `portfolio_id`). | Not a count and not maintained by the engine: a missing or late row silently changes every annualised figure. |
| `trading.corp_action_applied` | one corporate action whose effect is already in a stored equity position | Primary key (`portfolio_id`, `strategy_id`, `strategy_name`, `symbol`, `action_type`, `ex_date`). Insert with `ON CONFLICT DO NOTHING` (`store_applied_corp_actions_in`, `src/data/postgres_database.cpp:3547-3548`). | Equity runner only, in one transaction with the positions the action restated (`apps/strategies/live_equity_mean_reversion.cpp:3053-3077`, `:3477`). | Final when written; never replaced. | `trading.positions` on the three book keys and `symbol`. | Not a list of the events that occurred: a deferred or refused event writes no row. |
| `backtest.results` | one backtest run's summary statistics and cost totals | Primary key (`run_id`). Plain insert (`src/data/postgres_database_extensions.cpp:206`). | The backtest runners at the end of the run, first of the end-of-run writes (`src/storage/backtest_results_manager.cpp:61`, called from `BacktestCoordinator::save_portfolio_results_to_db`, `src/backtest/backtest_coordinator.cpp:2114`). | Final when written. Nothing replaces a run; a second run with the same `run_id` fails on the key. | The other `backtest` tables on `run_id`. | Not one row per sleeve or per day: one row per run. |
| `backtest.equity_curve` | one run's account value after one bar date | Primary key (`run_id`, `timestamp`); foreign key to `backtest.results`. | End of run, one batch (`src/storage/backtest_results_manager.cpp:140`). `risk_detail` is filled on the row of every sized rebalance. | Final when written. | `backtest.results` on `run_id`. | Not lagged like the live curve: a row is written once, at the end of the run, and never revised. Its first rows are flat warm-up rows. |
| `backtest.executions` | one fill of one sleeve in one run | Primary key (`run_id`, `strategy_id`, `execution_id`); foreign key to `backtest.results`. | End of run, per sleeve (`src/storage/backtest_results_manager.cpp:266`). | Final when written. | `backtest.results` on `run_id`; `backtest.final_positions` on (`run_id`, `strategy_id`, `symbol`) and the date. | Not keyed like the live table: ids are run-wide counters (`EX-<sleeve>-<n>`, `RL-<sleeve>-<n>` for roll legs), not `EXEC_<symbol>_<date>`. |
| `backtest.final_positions` | one sleeve's holding of one symbol on one bar date of one run | Primary key (`run_id`, `strategy_id`, `date`, `symbol`). The writer deletes (`run_id`, `strategy_id`, `DATE(date)`) and inserts (`src/data/postgres_database_extensions.cpp:378-382`). No foreign key: it is written before the run's `results` row exists. | **During** the run, once per bar date after warm-up (`BacktestCoordinator::save_daily_positions`, `src/backtest/backtest_coordinator.cpp:416`, `:1970`). | Final when written. | `backtest.results` on `run_id`. | Despite its name, not the end-of-run book: it is a **daily snapshot**, one set of rows per bar date. The final book is the rows of the run's last date. |
| `backtest.run_metadata` | one sleeve's configuration within one run | Unique (`run_id`, `strategy_id`); no primary key. Upsert on that pair (`src/data/postgres_database.cpp:2578-2583`). | End of run, per sleeve (`src/storage/backtest_results_manager.cpp:299`). | Final when written. | `backtest.results` on `run_id`. | Not one row per run: a two-sleeve run has two rows. |
| `backtest.signals` | one signal of one symbol at one bar of one run | Unique (`run_id`, `strategy_id`, `symbol`, `timestamp`); surrogate primary key `id`. | A writer exists (`src/data/postgres_database.cpp:2508`); no runner hands it signals, so a run stores no row here. | Not applicable. | `backtest.results` on `run_id`. | Not a place to look for a backtest's forecasts: the table is empty for the portfolio runners. |
| `metadata.contract_metadata` | one futures root: its contract size, tick size, margins, trading hours and fee per contract | No key constraint; the only constraint is the check that `"Fee Per Contract"` is a positive number. Rows are looked up by `"Databento Symbol"`. | No runner writes it. Migrations 014, 019 and 026 are its only writers in this repository. | Final until a migration or the data owner changes it. | A symbol such as `ZF.v.0` joins on its root, `ZF`, to `"Databento Symbol"` (the suffix is stripped before the lookup). | Not typed: every column is text and is parsed on load (`create_instrument_from_db`, `src/instruments/instrument_registry.cpp:338-361`). Not one row per traded contract month either. |

## 6. Column dictionary: `trading.positions` and `trading.live_results`

A **flow** is an amount for the row's own date and may be summed over dates. A **level** is a
value as of the row's date and must never be summed over dates.

### 6.1 `trading.positions`

| Column | Kind | Meaning |
|---|---|---|
| `portfolio_id`, `strategy_id`, `strategy_name` | key | The book and the sleeve (section 3). |
| `date` | key | The UTC date of the run that wrote the row (`src/data/postgres_database.cpp:671-672`). |
| `last_update`, `updated_at` | instant | The run instant, `now`; both are written with the same value (`src/data/postgres_database.cpp:678-679`). On a dated run, `05:00Z` on futures rows written on the New York image and `00:00Z` on equity rows (section 1). The loader filters on `DATE(last_update)`. |
| `symbol` | key | The engine symbol (`ZF.v.0`, `TMUS`). |
| `quantity` | level | The signed holding of this sleeve after the run's fills. Whole contracts on a futures row; shares, possibly fractional, on an equity row. |
| `average_price` | level | Futures: the close the row was entered at, the D-1 close (`apps/strategies/live_portfolio_conservative.cpp:2323`). Equities: the cost basis; 0 on a closed row. See `AVERAGE_PRICE_LIFECYCLE.md`. |
| `daily_realized_pnl` | **flow**, gross of transaction costs | Futures: the settled move of the row's date, 0 until the run of D+1 writes it; 0 by design on a withheld bar, on a change bar and on a date with no bar. Equities: the P&L realized by that date's own fills and corporate actions, written on the day and not revised (`include/trade_ngin/live/live_daily_cycle.hpp:630-649`). |
| `daily_unrealized_pnl` | **level**, despite its name | Futures: 0 on every row. Equities: quantity × (mark - cost basis) as of the row's date, rewritten by the run of D+1 with the D close. |
| `portfolio_type` | key | The column default `system` on every row a runner writes. |
| `instrument_id` | level | Futures: the vendor contract id held after the symbol's last consumed bar (`apps/strategies/live_portfolio_conservative.cpp:2904-2906`, `:2275-2277`); the stored id is the state the roll legs are booked from. NULL on equity rows and where unknown. |

### 6.2 `trading.live_results`: P&L, costs and value

`initial` is the book's starting capital (500,000 on the futures book).

| Column | Kind | Futures book | Equity book |
|---|---|---|---|
| `daily_realized_pnl` | flow, gross of costs | the sum of the sleeves' settled moves for the date; 0 until the run of D+1 | the day's trade-realized P&L |
| `daily_unrealized_pnl` | flow (a change) | 0 | the change in `total_unrealized_pnl` from the previous row (`apps/strategies/live_equity_mean_reversion.cpp:5515`) |
| `daily_transaction_costs` | flow, **after netting** | every fill of the date at its own cost minus its `netting_adjustment` (`transaction_cost::add_net_costs`, `apps/strategies/live_portfolio_conservative.cpp:2575-2578`); roll legs included | the same helper (`apps/strategies/live_equity_mean_reversion.cpp:4507-4508`); one strategy, so every adjustment is 0 |
| `daily_roll_costs` | flow | the part of `daily_transaction_costs` paid on `ROLL` legs, each at its own cost (a roll leg is never netted) (`apps/strategies/live_portfolio_conservative.cpp:2439`) | 0 |
| `daily_pnl` | flow, net | `daily_realized_pnl - daily_transaction_costs` | `daily_realized_pnl - daily_transaction_costs + daily_unrealized_pnl` (`:5527`) |
| `daily_return` | flow, percent | `daily_pnl` over the previous row's `current_portfolio_value`, × 100 | the same |
| `total_transaction_costs` | level (cumulative), after netting | the previous row's total plus the date's (`apps/strategies/live_portfolio_conservative.cpp:3822-3823`) | the same |
| `total_roll_costs` | level (cumulative) | the previous row's total plus the date's (`:3825-3833`); the roll part of `total_transaction_costs` | 0 |
| `total_realized_pnl` | level (cumulative), gross | `total_pnl + total_transaction_costs` (`:3837`) | the previous row's total plus `daily_realized_pnl` (`:5510`) |
| `total_unrealized_pnl` | level | 0 | the sum of the position rows' `daily_unrealized_pnl` as of the date |
| `total_pnl` | level (cumulative), net | the previous row's `total_pnl` plus `daily_pnl` (`:3819`); equivalently `total_realized_pnl - total_transaction_costs` | `total_realized_pnl - total_transaction_costs + total_unrealized_pnl` (`:5521`) |
| `current_portfolio_value` | level | the previous row's value plus `daily_pnl` (`:3820`); on an unbroken chain, `initial + total_pnl` | `initial + total_pnl` (`:5522`) |
| `total_cumulative_return` | level, percent | `(current_portfolio_value / initial - 1) × 100` | the same |
| `total_annualized_return` | level, percent | section 7 | section 7 |
| `total_dividend_income` | level (cumulative) | 0 | dividend cash received; informational, never added to any P&L total |

The identities to hand-check, per asset class:

```
futures    daily_pnl  = daily_realized_pnl - daily_transaction_costs
           total_pnl  = total_realized_pnl - total_transaction_costs
equities   daily_pnl  = daily_realized_pnl - daily_transaction_costs + daily_unrealized_pnl
           total_pnl  = total_realized_pnl - total_transaction_costs + total_unrealized_pnl
both       current_portfolio_value(D) = current_portfolio_value(D-1) + daily_pnl(D)
           trading.equity_curve.equity = current_portfolio_value, on the same UTC date
```

Costs come off exactly once, in `daily_pnl`; the realized columns are gross. Legal sums over
dates: `daily_realized_pnl`, `daily_transaction_costs`, `daily_roll_costs`, `daily_pnl`. Never
sum `positions.daily_unrealized_pnl`, any `total_*` column or `current_portfolio_value`.

**Costs after netting.** When two sleeves trade one symbol on one date the account sends one
order. Each sleeve's execution row keeps its own cost and carries its share of the difference
as `netting_adjustment` (two credited fills, not one net fill); every total charges the cost
after netting, `total_transaction_costs - netting_adjustment` per fill
(`transaction_cost::net_cost`, `include/trade_ngin/transaction_cost/netting.hpp:97`). So
`daily_transaction_costs` equals the sum over the date's execution rows of own cost minus
adjustment, not the sum of `total_transaction_costs`. A `ROLL` leg is never netted and a roll
total reads the leg's own cost. `COST_MODEL.md` defines the adjustment.

### 6.3 `trading.live_results`: the stored book, risk and statistics

| Column | Kind | Meaning |
|---|---|---|
| `active_positions` | level | The number of non-zero position rows of the stored book. On a futures book it is counted over the sleeves' rows, so a symbol two sleeves hold counts twice (`include/trade_ngin/live/book_exposure.hpp:51`). |
| `gross_notional`, `net_notional` | level | The stored book valued at the day's mark (the D-1 close, or the last consumed close for a withheld bar). On a futures book a symbol two sleeves hold on opposite sides is taken once, on the net. |
| `margin_posted`, `cash_available` | level | Initial margin over the open positions; `current_portfolio_value - margin_posted`. |
| `portfolio_leverage` | level | `gross_notional` over `current_portfolio_value`, the row's value as first written. |
| `net_leverage` | level | `net_notional` over `current_portfolio_value`, the row's value as first written, on a futures row (`apps/strategies/live_portfolio_conservative.cpp:4069-4070`). |
| `gross_leverage` | level | Written by the equity runner only; NULL on futures rows, which carry the figure in `portfolio_leverage`. |
| `equity_to_margin_ratio`, `margin_cushion` | level | `current_portfolio_value / margin_posted`; `(current_portfolio_value - maintenance margin) / current_portfolio_value`; both on the row's value as first written. `portfolio_leverage`, `net_leverage`, `equity_to_margin_ratio` and `margin_cushion` are computed when the row is first written, on the previous row's `current_portfolio_value` minus the day's costs. The run of D+1 revises `current_portfolio_value` and `cash_available` but leaves these four as written: its update fills `portfolio_leverage` and `equity_to_margin_ratio` only when they are NULL or 0 (`apps/strategies/live_portfolio_conservative.cpp:3341-3348`) and does not name `net_leverage` or `margin_cushion` at all (`:3308-3355`). So on a settled row they do not equal the ratio recomputed from the row's final value. On the 2026-04-28 row of the run of section 4.3: `portfolio_leverage` 2.0325 = 1,023,392.6875 / 503,524.8670 (the previous row's 503,612.9639 less the day's costs, 88.0969), against a final `current_portfolio_value` of 507,837.7420. |
| `portfolio_var`, `max_correlation`, `jump_risk` | level | Readings of a snapshot risk evaluation of the stored book against the capital it was sized on, taken for the report (`apps/strategies/live_portfolio_conservative.cpp:2988-2993`, `:4058-4063`). They are not the overlay's readings and they move no position. |
| `risk_scale` | level | **Futures: the delivered scale of the day's rebalance**: the stored book's gross notional over the capped target's gross notional at the same closes, held rows counted in both (`apps/strategies/live_portfolio_conservative.cpp:4052-4056`). It can exceed 1. It is 1 on a day with no sized rebalance (a carried day, a sizing hold, a refused overlay) and on a flat target. It is never the overlay's requested multiplier, which is `risk_detail.risk_requested`. Equity rows store the snapshot evaluation's recommended scale instead (`apps/strategies/live_equity_mean_reversion.cpp:5711-5720`). |
| `risk_detail` | level, JSON | Futures only, on the row of a sized rebalance the overlay answered (`apps/strategies/live_portfolio_conservative.cpp:4222-4225`); NULL on every other row. Keys below. |
| `config` | JSON | A report snapshot: the strategy id, starting capital, the exposure figures above (its `gross_leverage` is `gross_notional` over the starting capital, not the `portfolio_leverage` column, `apps/strategies/live_portfolio_conservative.cpp:4031`) and `t1_classification`, the day's per-symbol session counts (`apps/strategies/live_portfolio_conservative.cpp:4022-4034`). Its `weight`, `risk_target` and `idm` keys are literals written by the runner, not read from the book's configuration. |
| `volatility`, `downside_deviation`, `sharpe_ratio`, `sortino_ratio`, `max_drawdown`, `win_rate`, `avg_win`, `avg_loss`, `profit_factor`, `best_day`, `worst_day`, `gross_profit`, `gross_loss`, `winning_days`, `losing_days`, `total_days` | level (since inception) | Statistics of the book's stored daily history up to the row's date (`LiveHistoricalMetricsCalculator`), written with the row and rewritten by the run of D+1 once D is final (`apps/strategies/live_portfolio_conservative.cpp:3581-3606`). `total_days` is the calendar-day count of section 7. |
| `created_at` | instant | When the row was inserted. A completed run deletes and re-inserts its `live_results` row at its end, so this is the run clock the watchdog reads. |

The keys of `risk_detail` (`include/trade_ngin/risk/risk_detail.hpp:48-57`, built at `:64`);
`RISK_MODULES.md` defines the overlay they describe:

| Key | Meaning |
|---|---|
| `risk_requested` | The multiplier the overlay asked for; 1.0 with no cut. |
| `binding_term` | Which of the overlay's five readings set it: `R`, `R_jump`, `R_shock`, `L_g`, `L_n`, or `none`. |
| `over_limit_after_rounding_terms` | The readings still above their limits on the stored book after the trim, `;`-separated; null when none. |
| `over_limit_after_rounding_excess` | The size of that excess, in the unit `include/trade_ngin/risk/risk_detail.hpp:28` defines; null when none. |
| `over_limit_by_hold_terms` | The readings above their limits because of held rows, the per-name cap included; null when none. |
| `over_limit_by_hold_symbols` | The held symbols responsible, sorted and space-separated; null when none. |
| `overlay_blind` | True when the gate window had fewer than 21 complete dates: the three covariance readings were not computed and only the two leverage readings applied. |
| `sizing_capital` | The capital the book was sized on that day (section 9). |
| `account_value` | The starting capital plus the cumulative settled net P&L the sizing capital was built from. |

On the 2026-04-28 row of the run of section 4.3, `account_value` is 503,612.963982, which is
500,000 plus the `total_pnl` of the 2026-04-27 row (3,612.9639), and `sizing_capital` is
500,000.0: the account is above the starting capital, so the profit is set aside.

## 7. Trading-day count and annualisation

`trading.get_trading_days(strategy_id, target_date, portfolio_id)`
(`migrations/004_get_trading_days_portfolio_scope.sql:84`) returns

```
GREATEST(1, (target_date - live_start_date) + 1)
```

where `live_start_date` comes from `trading.strategy_trading_days_metadata` for that
`strategy_id` **and** `portfolio_id` (earliest row if several). If no metadata row exists it
falls back to `MIN(DATE(date))` over `trading.live_results` for the same strategy and
portfolio, and if that is empty too it returns 1.

This is a **calendar-day count**, not an exchange-session count: weekends and holidays are
included. All three runners call the three-argument form
(`apps/strategies/live_portfolio_conservative.cpp:3872-3874`,
`apps/strategies/live_equity_mean_reversion.cpp:5549`). The annualised return is geometric
over that count, `((1 + R)^(252 / N) - 1) × 100`
(`LiveMetricsCalculator::calculate_annualized_return`, `src/live/live_metrics_calculator.cpp:52-55`),
so it divides a calendar-day count by 252. The backtest's ratios annualise differently: Sharpe,
Sortino and Calmar use the mean daily return times 252
(`src/backtest/backtest_metrics_calculator.cpp:75`, `:98`, `:715`), so a live and a backtest
annualised figure are not comparable by construction.

Two operational rules follow:

- A metadata row must exist per (`strategy_id`, `portfolio_id`) before the first run, with
  `live_start_date` equal to the book's first day. A row dated after the book's first day is
  worse than none: the function stops at the first row it finds.
- Every runner compares the metadata anchor against `MIN(date)` in `live_results` for its book
  and logs a warning when the anchor is later
  (`apps/strategies/live_portfolio_conservative.cpp:469-513`,
  `apps/strategies/live_equity_mean_reversion.cpp:834`). The metadata row stays authoritative:
  the fix is to correct the row. The two-argument form of the function still exists, keys on
  `strategy_id` alone, and no runner calls it.

## 8. Corporate-action de-duplication

Equity-only: `trading.corp_action_applied`, its replay guards and the reset rule are described
in `CORP_ACTIONS_DATA_BOUNDARY.md`, section "De-duplication: which events are already in the
book".

## 9. How the futures runners differ

| Aspect | Equity runner | Futures runners (`live_portfolio_conservative`, `live_portfolio`) |
|---|---|---|
| Date parse | `parse_utc_date`, UTC midnight (`apps/strategies/live_equity_mean_reversion.cpp:102`) | `std::get_time` and `std::mktime`, local midnight (`apps/strategies/live_portfolio_conservative.cpp:115-117`): on the deployed New York image every stored instant is `05:00:00Z` of the run date, in winter and in summer. `now_tm` is `std::localtime` (`:313`) where the equity runner uses `gmtime_r` (`apps/strategies/live_equity_mean_reversion.cpp:250`). |
| Cadence | Every calendar date can be run; a weekend or holiday is a carry-forward (`:1064-1087`, `:3526-3530`). No job is scheduled for it by default. | **Every calendar day**: the schedule is `30 9 * * *` in New York time (`live_portfolio.cron:15`) and the wrapper has no weekday guard (`scripts/run_live_portfolio.sh:40-44`). The exact-date loader needs the unbroken chain, and a missed day is refused (section 10). |
| Previous day | `find_previous_trading_day` walk (section 2) | strictly `now - 24h` (`apps/strategies/live_portfolio_conservative.cpp:361`, `:1994`) |
| Was T-1 a session? | One answer for the book, from the calendar: `is_non_trading_day(today)` | **One answer per symbol**, from its own bars: `classify_t1` (`:1208-1211`) returns `SESSION`, `JUNK`, `NO_BAR(closure)` or `NO_BAR(feed hole)` (`include/trade_ngin/data/session_classifier.hpp:22-39`, the rule at `:140-164`). A session trades. A junk print is withheld from every consumer and the symbol is held. A closure and a feed hole are held at the last mark, and a feed hole is logged as an error. An instrument-id change that the next bar has not yet confirmed is held when its volume is below 0.10 of the symbol's norm (`kIdChangeHoldFraction`, `:54`; the rule at `:166-187`); that bar is consumed, not withheld (`:114-130`). The holds are applied to every sleeve's book by `hold_non_session_symbols` (`apps/strategies/live_portfolio_conservative.cpp:2130`). |
| A closed day | Carry-forward decided by the calendar | The whole book is carried only when **no** symbol has a T-1 price or no T-1 bar is consumed (`:1585`); the classifier names the reason (a closure is information, a feed hole an error). There is no weekday test and no abort. The holiday calendar is loaded and must cover the dates (`:1089-1134`) and feeds the classifier's closure verdict (`:1211`). |
| Saturday, Sunday, Monday rows | carry-forward on Saturday and Sunday; Monday trades at Friday's close (section 4.4) | Saturday trades at Friday's close; Sunday is a whole-book carry unless a contract printed on Saturday; Monday trades each symbol that has a Sunday bar at that bar's close and holds the others (section 4.3) |
| Accounting model | `MARK_TO_MARKET` (`apps/strategies/live_equity_mean_reversion.cpp:3758`) | `SETTLED` (`apps/strategies/live_portfolio_conservative.cpp:2254`); section 4.2 |
| Execution ids | `EXEC_<symbol>_<YYYYMMDD>`, order id `DAILY_<symbol>_<YYYYMMDD>` (`src/live/execution_manager.cpp:146`, `:250`) | The same ids on both futures books, for example `EXEC_ZF.v.0_20260424`. Three further forms: a forecast-sign close carries the suffix `_SC` on both ids and is stored ahead of the symbol's other fill of the day (`include/trade_ngin/live/session_book_gate.hpp:421`); a roll's two legs are `EXEC_<symbol>_<YYYYMMDD of the confirming bar>_RC` (closing) and `_RO` (opening), with order ids `ROLL_<symbol>_<YYYYMMDD>_RC` and `_RO` (`apps/strategies/live_portfolio_conservative.cpp:2406-2412`), so a leg's id date can be earlier than the row's `date`. A live run books no listing-date switch: it refuses to run with a predecessor contract in the universe or the stored book (`include/trade_ngin/live/live_listing_guard.hpp:14-28`). The backtest's ids are run-wide counters: `EX-<sleeve>-<n>` with order id `PM-<sleeve>-<n>` (`src/portfolio/portfolio_manager.cpp:751-752`), `RL-<sleeve>-<n>` for roll legs (`src/backtest/backtest_coordinator.cpp:1063-1064`), and `LC-<sleeve>-<n>` and `LO-<sleeve>-<n>` for the two legs of a listing-date switch (`src/portfolio/portfolio_manager.cpp:2516`). |
| Sizing capital | does not use the half-compounded capital | The **half-compounded sizing capital**: 500,000 less the drawdown of the cumulative settled net P&L from its running peak, with profits above 500,000 set aside (`half_compounded_capital`, `include/trade_ngin/portfolio/sizing_capital.hpp:36-71`). It is recomputed on every run from the stored `live_results` rows before D-1 plus D-1's net rebuilt from the D-1 book, its settlement move and its stored costs (`read_live_sizing_equity`, `include/trade_ngin/live/live_sizing_read.hpp:146`), and handed to the portfolio manager (`apps/strategies/live_portfolio_conservative.cpp:1519`). When D-1 cannot be settled (no D-1 row on a book that has history, no D-1 closes while the book holds positions, or no D-2 closes) the capital of the last settled day is kept, nothing is added for D-1 and the run logs `SIZING_CAPITAL_UNSETTLED` at warning level (`include/trade_ngin/live/live_sizing_read.hpp:232-244`, `:292-300`). `OPTIMIZER_AND_RISK_DESIGN.md` gives the worked table. |
| A day whose risk or sizing cannot be evaluated | none: the run returns 0 or 1 (section 10) | A sizing read that fails, or a refusal of the book by the risk step, **holds the book, stores the day, marks `trading.live_run_metadata` and exits 3** (`kRiskModuleFailureExitCode`, `include/trade_ngin/live/risk_module_failure.hpp:35`, returned at `apps/strategies/live_portfolio_conservative.cpp:4777`); section 10. |
| Transaction cost inputs | the equity cost feed and its 20-bar average volume (`AVERAGE_PRICE_LIFECYCLE.md`, section 8b) | A fill is priced on **the fill day's own volume**, the volume of the symbol's T-1 bar, with the weekend block merged: a weekday fill priced at a session bar adds the weekend bars right before it, and a weekday fill priced at a weekend bar uses the last session plus the block (`include/trade_ngin/live/futures_cost_feed.hpp:30-63`). Both cost managers, the execution manager's and the portfolio manager's, are fed the same bars (`apps/strategies/live_portfolio_conservative.cpp:1668`, `:1709`). `COST_MODEL.md` has the formulas. |
| Sleeves and netting | one strategy; every `netting_adjustment` is 0 (`apps/strategies/live_equity_mean_reversion.cpp:4505-4508`) | One execution row per sleeve. When two sleeves trade a symbol the account sends one order: **both rows are stored, each with its own cost and its share of the saving as `netting_adjustment`** (`:2550-2568`), and the day's cost and every total are after netting (`:2575-2578`). A roll leg is never netted. Exposure and margin take a symbol held on opposite sides once, on the net (`include/trade_ngin/live/book_exposure.hpp:51`). |
| Trading-day count | three-argument `get_trading_days` | the same (section 7) |
| `live_run_metadata` | written after its start-up checks and before its data guards (`apps/strategies/live_equity_mean_reversion.cpp:689`), never marked | written after the guards and the sizing read (`apps/strategies/live_portfolio_conservative.cpp:1554`), marked on a refused day (section 10) |
| Logs | a dated run logs under `logs/<YYYY-MM-DD>/` with the prefix `live_equity_mr` (`:136-139`) | the flat `logs/` directory, prefix `live_trend_conservative` or `live_trend` (`apps/strategies/live_portfolio_conservative.cpp:141-142`, `apps/strategies/live_portfolio.cpp:142`) |
| Email body file | not available | `TRADE_NGIN_EMAIL_BODY_DIR` (section 11) |
| Corporate-action de-duplication | `trading.corp_action_applied` | not used: futures positions carry no corporate actions |

## 10. Refusals and `trading.live_run_metadata`

A futures run that cannot proceed ends in one of two shapes, and they leave different
evidence.

| Shape | What refuses | Exit code | `live_run_metadata` row for the date | What else is stored |
|---|---|---|---|---|
| **Refused to start** | Any guard above the metadata write. The ones an operator meets: a missed previous run (`apps/strategies/live_portfolio_conservative.cpp:360-446`), a stale or incomplete feed on a run given no date (`:1004-1068`; a dated run, production's included, logs the same finding as a warning and proceeds), a held symbol with no bar for longer than the tolerance on a run for the host's date (`:1215-1258`), an uncovered or unloaded calendar (`:1089-1134`), a listing-date refusal (`:559-568`), a stored book the run does not load (`:1447-1461`), an unreadable stored book or roll state (`:1310-1418`), a sleeve book the sizing read cannot load (`:1498-1502`); and any start-up failure before them (arguments, config, database, instrument registry, margin metadata, a sleeve that cannot be built or started, the bar load, the classifier history: all exit 1 with no row) | 1 | **No row** | Nothing: no position, execution, signal or result of the date |
| **Held and stored** | The sizing read fails with every sleeve book loaded (`apps/strategies/live_portfolio_conservative.cpp:1503-1508`); the risk step refuses the book (`:1853-1862`): the one pass cannot produce its answer, or a sleeve risk module refuses or replaces its sleeve or cannot evaluate it, which refuses the whole book (`src/portfolio/portfolio_manager.cpp:2593-2594`) | 3 | **A row carrying `risk_refusal`** in `portfolio_config`, with the decision record in `risk_decisions` (`mark_risk_refusal`, `include/trade_ngin/live/run_metadata_marks.hpp:65-71`). The sizing hold is marked at the first write (`apps/strategies/live_portfolio_conservative.cpp:1548-1551`), scope `sizing`; a refusal by the risk step by writing the same row again (`apps/strategies/live_portfolio_conservative.cpp:1837-1841`) | The held book as the day's positions, no orders for the held scope, the `live_results` row and the equity point; the email subject and body are flagged (`include/trade_ngin/live/risk_module_failure.hpp:92`, `:117`) |

Details that matter when reading the table:

- A run-gap or feed refusal therefore writes **no** metadata row. Nothing in the database
  says the run was attempted, and nothing reports it the same day: the watchdog alerts only
  when the book's run clock or book date is more than one calendar day old, which is the
  morning after the refused run (`scripts/check_live_trading.py:68`, `:120-131`).
- On a held day the book is the previous day's book: every sleeve stays at its seeded
  positions, and the next run settles and sizes from it normally.
- A refusal by the risk step never ends at 0. The one pass records every refusal of the book
  with its reason as the record's error, the refusal a sleeve module decides included
  (`src/portfolio/portfolio_manager.cpp:2593-2594`, `:2689-2695`), and the runner returns 3 for a
  portfolio record that carries an error (`live_run_exit_code`,
  `include/trade_ngin/live/risk_module_failure.hpp:41-55`, `:87-89`). A row carrying
  `risk_refusal` therefore goes with exit 3, or with exit 1 when the run stops after the mark,
  as it does when the refused book has no stored previous positions to be held at
  (`apps/strategies/live_portfolio_conservative.cpp:1830-1841`, then `:1900-1903`).
- A third mark exists. When a book change is left with no T-1 price and no execution after
  the pricing rollback, the run writes `strict_assertion` into the same row, stores no book
  and exits 1 (`mark_strict_assertion`, `include/trade_ngin/live/run_metadata_marks.hpp:81-89`;
  `apps/strategies/live_portfolio_conservative.cpp:2516-2539`); the day's signals are already
  stored at that point (`:1940`). The watchdog reports both marks.
- An unmarked row is not proof of a completed run. Stops after the metadata write, for example
  a roll leg that cannot be stored (`apps/strategies/live_portfolio_conservative.cpp:2632-2644`), exit 1 and leave the row as first written,
  with the signals or a half-written day beside it depending on the step (section 10.1).
  The evidence of a completed run is the date's `trading.live_results` row.

The equity runner never marks its metadata row. It writes the row after its start-up checks
and before its data guards (`apps/strategies/live_equity_mean_reversion.cpp:689`), so its
refusals exit 1 with or without the row: without it, an uncovered or unloaded calendar
(`:184-189`, `:334-341`) and no previous trading day in fourteen days (`:348-353`); with it, a
stale or incomplete feed on a run given no date (`:896-940`), a missed previous trading day
(`:1192-1199`) and a de-duplication record it cannot trust. It also exits 1 when the previous
day cannot be settled, when an execution cannot be stored or applied, and when a
corporate-action or position write fails.

### 10.1 Exit codes

One table for every runner and for the wrapper. `performance_upkeep.md` carries the same table.

| Exit code | When it happens | What it can leave stored |
|---|---|---|
| 0 | A futures live runner (`live_portfolio`, `live_portfolio_conservative`) reaches the end of the run with the book not refused by the risk step and no sizing hold. Three cases share this code: a completed day; a carried day (no symbol has a consumed bar for the previous day, so the whole book is carried and no order is made); a run in which a store failed, was logged and was passed over. A refusal by the risk step never ends at 0: see the rows for 3 and for 1 after the `live_run_metadata` row. | A completed day: the `live_run_metadata` row, the signals, the executions, the settled rows of the previous day, the day's positions, the day's `live_results` row and its equity point. A carried day: the same without signals; the book is unchanged, so no order is made. A passed-over failure: the day without the table whose store failed, which can be the `live_run_metadata` row, the signals, the executions of a sleeve that has no roll leg, the previous day's positions, the day's positions, the previous day's `live_results` update or equity point, or the day's `live_results` row and equity point. Exit 0 is not proof that the day is stored in full: the evidence of a completed day is the date's `trading.live_results` row. |
| 0 | The wrapper `scripts/run_live_portfolio.sh` finds its lock directory present and skips. The binary is not started. | Nothing. |
| 1 | A futures live runner refuses before it writes the day's `live_run_metadata` row: a bad argument; a logger that cannot be initialised; a config, database or instrument registry failure; a missed previous run; a listing-date refusal; missing margin metadata; a sleeve that cannot be built or started; bars that cannot be loaded; a stale or incomplete feed on a run given no date; a holiday calendar that is not loaded or does not cover the run date or the day before it; a held symbol with no bar for longer than the tolerance on a run for the host's date; roll state that cannot be read or placed; a stored book the run does not load; a sleeve book the sizing read cannot load; a sizing capital the portfolio manager rejects. | Nothing. |
| 1 | A futures live runner stops after it wrote the day's `live_run_metadata` row. | It depends on where the run stops. The estimator history cannot be seeded, or the portfolio step fails: the `live_run_metadata` row (marked `risk_refusal` when a sizing hold was recorded first, when the portfolio step failed because the risk step refused a book that has no stored previous positions to be held at, or, with scope `sleeve`, when a sleeve risk module or the sleeve's risk step could not answer on a sleeve that has no stored previous positions). A roll leg has no usable close, executions cannot be generated with a roll owed, the strict assertion fails (the row is marked `strict_assertion`), the netting refuses a row, or the day's roll executions cannot be stored: the row and the day's signals. The sweep of a sleeve that has no roll leg fails: the row, the signals and, on a book of two sleeves, the executions of a sleeve already stored with its roll legs. The margin calculation fails: the row, the signals, the executions and the rewritten positions of the previous day. The previous cumulative roll cost cannot be read: a half-written day, with the executions, both days' positions and the previous day's settled `live_results` row and equity point stored, and no `live_results` row for the day. Any other error: whatever was written before it. |
| 3 | Futures live runners only. The risk step refused the book, or the sizing read failed with every sleeve book loaded. The risk step refuses the book when the one pass cannot produce its answer (the overlay sleeve is not running, a held symbol that cannot be weighed has no usable close or multiplier, an input or an overlay reading is not a finite number, the pass fails), and when a sleeve risk module refuses or replaces its sleeve or cannot evaluate it: one search on the summed book cannot hold one sleeve apart, so the whole book is held. Every such refusal is recorded with its reason as an error, whether a module decided it or a module failed. The book is held and the run goes on to the end. | A held day: the held book as the day's positions, no order, the day's `live_results` row and its equity point, and the day's signals. On a sizing hold the portfolio step is not run, so the sleeves compute no forecast and the signal stored for each symbol with seeded history is 0. The `live_run_metadata` row is marked `risk_refusal`, and the subject and the body of the report are flagged. |
| 0 or 1 | The equity live runner (`live_equity_mr`) returns only these two codes, never 3. It returns 1 on each of its refusals, and also when the previous day's positions or `live_results` row cannot be settled, when an execution cannot be stored or applied, and at the end of a run whose results could not all be saved. | On exit 1: nothing if the stop is before its `live_run_metadata` row; otherwise that row and whatever was written before the stop. Its feed checks come after that row, so a feed refusal leaves the row. On exit 0: the day; a failed store of the `live_run_metadata` row or of the previous day's equity point is logged and passed over. |
| 0 or 1 | The backtest runners (`bt_portfolio`, `bt_portfolio_conservative`, `bt_equity_mr`) take no argument. They return 1 when the config, the database, a strategy or the backtest itself fails, and 0 when the backtest ran. `bt_equity_validation` returns 0 only when every check passes. | On exit 1: no `backtest.results` row and nothing saved at the end of the run; the `backtest.final_positions` rows of the bar dates processed before the failure remain, because they are written during the run. On exit 0: the results, unless the save to the database failed; that failure is logged and passed over, so exit 0 does not prove the results are stored. |
| 127 | The wrapper cannot find the binary, or it is not executable. | Nothing. |
| 1 | The wrapper cannot change to the application directory. | Nothing. |
| any other code | No runner and no script returns any other code of its own. The wrapper passes on whatever code the binary ended with and logs it on one line. | Not defined by the code. Read the log and the tables. |

The futures runners have one return that is not 1,
`return live_run_exit_code(risk_module_failure)`
(`apps/strategies/live_portfolio_conservative.cpp:4777`), which gives 3 when a failure was
recorded and 0 otherwise (`include/trade_ngin/live/risk_module_failure.hpp:87-89`). The day's
`live_run_metadata` row is written at `apps/strategies/live_portfolio_conservative.cpp:1554`:
a `return 1` above that line stores nothing, and one below it leaves what the steps before it
stored (the signals at `:1940`, the executions at `:2632` and `:2702`, the previous day's
positions at `:2723-2729`, the day's positions at `:2961-2966`, the previous day's settled
results at `:3294-3360`, the day's results at `:4336`). A store that fails is logged and
passed over at `:1559-1561`, `:1947-1949`, `:2709-2711`, `:2731-2733`, `:2968-2970`,
`:3361-3363` and `:4337-4338`. The wrapper's own codes are at
`scripts/run_live_portfolio.sh:49-52` (lock held), `:56-60` (127), `:68` (cannot change
directory) and `:74-85` (the binary's code passed on). The equity runner's `return 0` is at
`apps/strategies/live_equity_mean_reversion.cpp:6264`, after the test of unsaved results at
`:6259-6262`.

## 11. Operating rules

- **A live run always carries a date.** A run without a date takes the wall clock as `now`
  and sends the email. This is a rule for the operator: no code refuses a run with no date
  (section 1.2).
- **Production is `<date> --send-email`**, which is what the scheduled wrapper runs
  (`scripts/run_live_portfolio.sh:71-74`). A manual or test run passes a date and omits
  `--send-email`.
- **A lone re-run of an older date is not supported.** Every date is computed from the rows
  the previous date left and settles the previous date's rows, so re-running D alone leaves
  D+1 onward computed from a book that no longer exists. A replay runs **every date from the
  first changed one forward, in order**, one runner at a time.
- **A late bar is a warning, and its remedy is that replay.** When a held symbol's bar arrives
  after the run that should have settled it, the stored row of that date books 0 and the next
  run settles against the late bar's close, so that bar's own move is on no stored row. The
  run that first consumes the bar prints one `LATE_BAR` line at warning level naming the
  symbol, the date, the unbooked amount and the remedy: re-run the date that settles the bar
  and every later date in order. Nothing stored changes, and the run is not refused
  (`include/trade_ngin/live/late_bar_warning.hpp:18-46`, `:128-150`; called at
  `apps/strategies/live_portfolio_conservative.cpp:2092-2113`).
- **The email body as a file.** `TRADE_NGIN_EMAIL_BODY_DIR` applies only to a futures run that
  does not send: with the variable set to a directory, the run builds the report body exactly
  as for a send, writes it there as `email_body_<portfolio>_<date>.html` and mails nothing.
  With `--send-email` the variable is ignored with one warning line in the log, and the report
  is mailed as usual: **`--send-email` wins**, so a variable left in a production environment
  cannot silence the daily email. A run with no date sends, so it ignores the variable in the
  same way. The equity runner does not read the variable. With neither the flag nor the
  variable, nothing is built (`include/trade_ngin/live/email_body_file.hpp:1-10`, `:34-46`,
  `:55-59`; `apps/strategies/live_portfolio_conservative.cpp:4380-4385`).

`performance_upkeep.md` covers operations: what runs where, the schedule, the wrapper's exit
codes and log locations, the watchdog and the migration order.
