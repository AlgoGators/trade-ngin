# Performance and Upkeep

How the engine is built, tested, released and operated: the build and test commands, the CI pipeline, the container
and its schedule, the wrapper and its exit codes, the rules for manual and catch-up runs, the watchdog, and the
database migrations.

What a live run does on a given date, and what each stored row means, is in [LIVE_RUN_CYCLE.md](LIVE_RUN_CYCLE.md).
Configuration is in [CONFIG_GUIDE.md](CONFIG_GUIDE.md).

## 1. Build and test

Install the dependencies with the scripts in `requirements/` (`install_ubuntu.sh`, `install_macos.sh`), then copy
the configuration template and fill in its placeholders (`config_template/README.md`):

```bash
cp -r config_template config
```

Build and test:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
ctest --test-dir build --output-on-failure
```

The build produces the library `trade_ngin`, one test executable, one benchmark executable (`trade_ngin_bench`,
`benchmarks/CMakeLists.txt:7`) and eight applications
(`apps/strategies/CMakeLists.txt:18-24`, `apps/backtest/CMakeLists.txt:27-39`):

| Binary | Source | Book |
|---|---|---|
| `live_portfolio_conservative` | `apps/strategies/live_portfolio_conservative.cpp` | CONSERVATIVE, the futures trend book (the scheduled one) |
| `live_portfolio` | `apps/strategies/live_portfolio.cpp` | BASE, the two-sleeve futures test book |
| `live_equity_mr` | `apps/strategies/live_equity_mean_reversion.cpp` | the equity mean reversion book |
| `bt_portfolio_conservative` | `apps/backtest/bt_portfolio_conservative.cpp` | CONSERVATIVE backtest |
| `bt_portfolio` | `apps/backtest/bt_portfolio.cpp` | BASE backtest |
| `bt_equity_mr` | `apps/backtest/bt_equity_mean_reversion.cpp` | equity backtest |
| `bt_equity_validation` | `apps/backtest/bt_equity_validation.cpp` | equity validation run |
| `bt_transaction_cost_report` | `apps/backtest/bt_transaction_cost_report.cpp` | cost report |

A Release build puts them in `build/bin/Release/`.

Tests are GoogleTest, compiled into `trade_ngin_tests` and discovered by CTest (`tests/CMakeLists.txt:253`, `:285`).
Two Python suites run through CTest as well: `MigrateRiskJson.ScriptSuite` and `CheckLiveTrading.ScriptSuite`
(`tests/CMakeLists.txt:298`, `:305`). Run a subset with a name pattern, for example `ctest --test-dir build -R CheckLiveTrading`.

## 2. CI

`.github/workflows/ci-cd-pipeline.yml` runs on every pull request and on a push to `main`, `develop`, `main-hd`,
`prod` or `staging` (`ci-cd-pipeline.yml:3-13`). Its jobs:

| Job | What it does |
|---|---|
| Code Linting | clang-format, clang-tidy, cppcheck and cpplint, with a report artifact |
| Build and Test | Debug and Release builds; CTest; on Debug a memory check, a coverage report and the coverage threshold (`COVERAGE_THRESHOLD`, `ci-cd-pipeline.yml:16`) |
| Security Scan | a source scan |
| Generate Summary Report | collects the artifacts |
| Schema Ownership Guard | fails if a string literal in `src/data` or `src/storage` (`.cpp`, `.hpp`) begins with `futures_data.`, `equities_data.`, `options_data.`, `synthetic.`, `auth.` or `research.` (`ci-cd-pipeline.yml:506-530`, the pattern at `:525-526`). A schema named later in a literal (for example `"FROM equities_data.corporate_action "`, `src/data/postgres_database.cpp:2922`) is not caught, and reads of those schemas exist |
| Image Generation | builds and pushes the container image (section 8) |
| Deploy to EC2 | replaces the running container (section 8) |

Four further workflows run beside it: a dependency review on pull requests into `main`
(`dependency-review.yml`), an SBOM on a push to `main` (`sbom.yml`), the OSSF scorecard on a push to `main` and
weekly (`scorecard.yml`), and a daily branch-protection job (`branch-protection.yml`). The watchdog is section 7.

## 3. What runs where

Production is one container built from `Dockerfile`. Its only job is to run cron.

| Piece | What it does | Where |
|---|---|---|
| Time zone | `TZ=America/New_York` is set in the image, so the cron line and the wrapper's `date` are New York time. The futures runners read the date argument as local midnight (`std::mktime`) and format it back through UTC, so on a New York clock the date keeps its day and every stored instant of a run carries the 05:00Z stamp on every date of the year (the parse leaves the daylight flag at zero, so local midnight is read as standard time in summer as well). On a clock ahead of UTC the same parse would land the run a day early; the pin keeps the container on the one zone the stored rows are written in. | `Dockerfile:85`; `apps/strategies/live_portfolio_conservative.cpp:117` and the comment above it |
| Entrypoint | cron gives each job a fresh, minimal environment, so variables passed into the container are not visible to a scheduled job. At start the entrypoint writes the variables the job needs (names matching `TRADING_*`, `DB_*`, `PG*`, plus `TZ` and `LD_LIBRARY_PATH`) to `/app/.cron_env`, mode 600, warns if neither the encryption key variable nor a key file is present, and then runs cron in the foreground. | `Dockerfile:128`; `scripts/docker-entrypoint.sh:22-34`, `:40-45`, `:47` |
| Cron file | installed as `/etc/cron.d/live_portfolio` and loaded as the crontab | `Dockerfile:114-117`; `live_portfolio.cron:15` |
| Healthcheck | every five minutes, `pgrep -x cron`; a container whose cron daemon has died reports unhealthy. Pair it with a restart policy in the compose file on the host. | `Dockerfile:119-124` |
| Wrapper | the script cron calls (section 5) | `scripts/run_live_portfolio.sh` |

One binary is scheduled: `live_portfolio_conservative`, the wrapper's default
(`scripts/run_live_portfolio.sh:21`). The wrapper reads `LIVE_BINARY` to run another binary, but a variable set in
the container's environment does not reach the scheduled job: the entrypoint copies only `TRADING_*`, `DB_*`, `PG*`,
`TZ` and `LD_LIBRARY_PATH` into `/app/.cron_env` (`scripts/docker-entrypoint.sh:28-34`), and the cron line sets no
variable (`live_portfolio.cron:15`). `LIVE_BINARY` takes effect only when the wrapper is started by hand with the
variable set in that shell. The equity book (`live_equity_mr`) and BASE (`live_portfolio`) have no scheduled job: they run only
when started by hand under the rules of section 6.

## 4. The schedule

```cron
30 9 * * * /app/scripts/run_live_portfolio.sh >> /proc/1/fd/1 2>&1
```

09:30 New York time, every calendar day (`live_portfolio.cron:15`). The wrapper has no weekday guard
(`scripts/run_live_portfolio.sh:40-44`).

The futures book runs seven days a week because a run dated D settles the row dated D-1, and the runner refuses to
start when the previous calendar day has no stored run (section 6). Each day of the week has work to do:

| Run on | What it does |
|---|---|
| Tuesday to Friday | books the previous day's session and trades |
| Saturday | books and trades the Friday session |
| Sunday | carries the book over the closed Saturday (a symbol that printed on Saturday is booked; the others are held) |
| Monday | books the Sunday Globex session |

A Monday-to-Friday schedule would drop a session a week and break the chain from each day to the one before it. The
schedule does not decide whether the previous day was a session: the engine decides that per symbol, and on a day no
symbol printed it carries the whole book (see "How the futures runners differ" in [LIVE_RUN_CYCLE.md](LIVE_RUN_CYCLE.md)).

## 5. The wrapper, its exit codes and the logs

`scripts/run_live_portfolio.sh` does five things in order:

1. Sources `/app/.cron_env`; if the file is missing it logs a warning and goes on (`:31-38`).
2. Takes the lock: `mkdir /tmp/live_portfolio.lock`, removed on exit. If the directory already exists it logs
   "skipping" and exits 0 (`:49-53`).
3. Checks that the binary exists and is executable (`:56-60`).
4. Changes to `/app`, because the runners resolve `./config` and `logs/` from the current directory (`:68`).
5. Runs `<binary> <today> --send-email`, where today is the container's New York date, logs the result and exits
   with the binary's own exit code (`:71-85`).

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

Exit 0 is the code of every run that reaches its end with the book not refused by the risk step and no sizing hold, so
it does not prove that the day is stored in full: a store
that fails in a futures runner is logged and the run goes on (`apps/strategies/live_portfolio_conservative.cpp:1559-1561`,
`:1947-1949`, `:2709-2711`, `:2731-2733`, `:2968-2970`, `:3361-3363`, `:4337-4338`), and the wrapper returns 0 without
starting the binary when it finds the lock held (`scripts/run_live_portfolio.sh:49-52`). Read the date's
`trading.live_results` row; the watchdog reads it (section 7).

Exit 1 before the day's `trading.live_run_metadata` row is written
(`apps/strategies/live_portfolio_conservative.cpp:1554`) stores nothing. A stop after it leaves what was already
written, as the table says step by step: that row, and depending on the step the signals, the executions and
the positions. The strict assertion (a book change with no price and no execution) stores no book and adds
`strict_assertion` to that row's `portfolio_config` (`:2516-2538`); the row can also carry `risk_refusal` at
exit 1, as the table says.

The futures runners refuse with exit 1 in two calendar cases, on every run whether or not a date is given:
the holiday file did not load, and the calendar does not cover the year of the run date or the year of the
day before it. Both are checked before the day's `live_run_metadata` row is written, so nothing is stored.
The equity runner refuses with exit 1 in three: the file did not load, the calendar does not cover the year
of the run date, and no trading day is found in the fourteen days before the run date. All three also come
before its `live_run_metadata` row. (Futures: `apps/strategies/live_portfolio_conservative.cpp:1089-1094`,
`:1117-1134`; equity: `apps/strategies/live_equity_mean_reversion.cpp:184-189`, `:328-341`, `:348-353`.)

On an exit 3 the email subject opens with `[RISK MODULE FAILED - BOOK HELD]` and the body with a banner. Code 3 is
`kRiskModuleFailureExitCode` (`include/trade_ngin/live/risk_module_failure.hpp:35`); `main` returns it
through `live_run_exit_code` (`risk_module_failure.hpp:87`, `apps/strategies/live_portfolio_conservative.cpp:4777`).
The causes are set at `live_portfolio_conservative.cpp:1504` and `:1988-1990` (the sizing hold) and at `:1853` (the
risk step refused the book, whichever module caused it); the marks are written by `mark_risk_refusal` and `mark_strict_assertion`
(`include/trade_ngin/live/run_metadata_marks.hpp:65`, `:81`). The design comment at the top of
`risk_module_failure.hpp` is the specification. Code 3 is a futures runner code: the equity runner returns 0 or 1.

A sleeve risk module that does answer, and answers REFUSE, refuses the whole book in the same way: the row is marked
with `risk_refusal` (`live_portfolio_conservative.cpp:1830`), the refusal is recorded with its reason as an error and
the run exits 3. A refusal of a book that has no stored previous positions to be held at fails the portfolio call
instead: the row is marked the same way and the run exits 1 (`:1900-1903`). No refusal by the risk step ends at 0. The
watchdog reports the mark either way (section 7). The modules and the refusal contract are in [RISK_MODULES.md](RISK_MODULES.md).

Logs:

| Log | Where |
|---|---|
| The wrapper's lines and everything the runner prints | the container's standard output (the cron line redirects to `/proc/1/fd/1`), read with `docker logs trade-ngin`. Wrapper lines are timestamped and tagged `[live-portfolio]`. |
| The runner's log file | `/app/logs/<prefix>_YYYYMMDD_HHMMSS_partN.log`. Prefixes: `live_trend_conservative` (CONSERVATIVE), `live_trend` (BASE), `live_equity_mr` (equity) (`live_portfolio_conservative.cpp:141-142`, `live_portfolio.cpp:142`, `live_equity_mean_reversion.cpp:139`). The logger keeps ten files per prefix per directory (`include/trade_ngin/core/logger.hpp:109`). |
| A dated equity run | `/app/logs/<YYYY-MM-DD>/`, one directory per replayed date, so a replay does not evict another date's logs (`live_equity_mean_reversion.cpp:137`) |

## 6. Manual and catch-up runs

| Rule | Why |
|---|---|
| A run always carries an explicit date: `<binary> YYYY-MM-DD`. | A run without a date takes the wall clock and turns the email on (`live_portfolio_conservative.cpp:129-131`). Do not start one: nothing in the code refuses it, and `scripts/dev_build_run.sh` starts the binary it builds with no argument (`:59-60`). The three run modes (no date, a date equal to the host's date, a past date) and what differs between them are in [LIVE_RUN_CYCLE.md](LIVE_RUN_CYCLE.md), section 1.2. |
| Production is `<date> --send-email`. A test or a replay passes the date and omits `--send-email`. | Without the flag nothing is mailed. To read the report of such a run, set `TRADE_NGIN_EMAIL_BODY_DIR` to an existing directory. `TRADE_NGIN_EMAIL_BODY_DIR` applies only to a futures run that does not send: with the variable set to a directory, the run builds the report body exactly as for a send, writes it there as `email_body_<portfolio>_<date>.html` and mails nothing. With `--send-email` the variable is ignored with one warning line in the log, and the report is mailed as usual. A run with no date sends, so it ignores the variable in the same way. The equity runner does not read the variable (`include/trade_ngin/live/email_body_file.hpp:34-46`, `live_portfolio_conservative.cpp:129-131`, `:4380-4384`). |
| One runner at a time. Never start two live or backtest runners together, whatever the books. | The runners share tables and each assumes it is the only writer for the length of its run; inside a process the market data bus is a single instance (`include/trade_ngin/data/market_data_bus.hpp:94`) that a runner switches off and on around its own steps. |
| The lock protects wrapper runs only. | `/tmp/live_portfolio.lock` is taken by `run_live_portfolio.sh`. A binary started directly takes no lock and is not stopped by it, so before a manual run check that no scheduled run is in progress and that none will start while it runs. |
| A replay runs every date forward, in order. A lone re-run of an older date is not supported. | The run dated D settles the row dated D-1 and seeds from it, so every later row is built on the earlier ones. After a missed day, or after correcting data for a past date, run that date and then each later date in turn up to today. The runner does not refuse a lone re-run: its missed-run check asks only whether the previous day is stored (`live_portfolio_conservative.cpp:360-446`). |
| A missed day stops the next run. | When the previous calendar day has no results row and no stored positions while an earlier run exists, the runner logs "A run was missed. Replay every date from ... forward, in order" and exits 1 before any write (`live_portfolio_conservative.cpp:429-440`). |
| A held symbol with no bar for longer than `live.data_staleness_tolerance_days` stops a futures run for the host's date and only warns on a replay of a past date. | The rule is exactly that: the symbol is held (a non-zero stored quantity), it has had no bar for more days than the tolerance, and the run's date is the host's date, which a run with no date always is (`live_portfolio_conservative.cpp:1221-1258`, `include/trade_ngin/live/session_book_gate.hpp:143-156`, `:176-178`). On a replay of a past date the same finding is a warning. A stale or missing symbol the book does not hold never stops a dated run; the three feed checks refuse only a run given no date (`live_portfolio_conservative.cpp:1016-1060`). The equity runner has no held-symbol check. The remedy is to refresh the feed and run again. |
| A late bar is a warning, and its remedy is the replay. | When a bar of a held symbol arrives after the run that should have consumed it, the first run that consumes it prints one WARNING line naming the symbol, the bar's date, the amount no stored row carries and the remedy: re-run the date that settles that bar and every later date in order. No stored value changes on its own (`include/trade_ngin/live/late_bar_warning.hpp:18-46`, `live_portfolio_conservative.cpp:2092-2112`). |

## 7. The watchdog

`.github/workflows/live-trading-watchdog.yml` runs `scripts/check_live_trading.py` every day at 15:00 UTC, after the
09:30 New York run (`live-trading-watchdog.yml:23`), and on demand. It asks whether the engine's output reached the
database, which covers a stopped container, a dead cron, a missing binary, missing credentials and a run that
failed part way. It reads only; it connects with the repository's database secrets.

Per book (`LIVE_PORTFOLIOS`, default `CONSERVATIVE_PORTFOLIO`, `check_live_trading.py:70`) it reads four things
(`_fetch` at `:141`, `_fetch_run_marks` at `:159`) and `evaluate` (`:97`) decides:

| Reading | Source | Reported when |
|---|---|---|
| The run clock | `max(created_at)` of `trading.live_results` for the book, read in UTC. A completed run writes its results row at the end, with only the equity point after it. | there is no row, or it is more than `MAX_CALENDAR_DAYS_SILENT` calendar days old (default 1, `:68`) |
| The book stamp | the latest `trading.live_results.date` not after today | it is more than the same number of calendar days old |
| The positions stamp | the latest `trading.positions.date` with `portfolio_type = 'system'`, not after today | the latest results row says the book holds positions and the positions are older than it: a partial write. A flat book writes no position rows and is healthy. |
| The run marks | the latest `trading.live_run_metadata` row not after today: whether its `portfolio_config` carries `risk_refusal` or `strict_assertion` | either mark is present. A held day stores its results and positions, so the first three readings pass it; the mark is what says the book was held. |

Thresholds are calendar days, because the book runs every day. The default of 1 allows today's run to be still in
progress and does not allow yesterday's to be missing.

How it fails: any reported reason makes the script exit 1, so the workflow run fails, and it opens a GitHub issue
labelled `live-trading-down`, or adds a comment to the open one (`_file_issue` at `:174`). A run that refused to
start writes no row at all, so it shows as silence on the run clock on the following day. If the database cannot be
reached the script stops on the connection error: the workflow run fails and no issue is filed. On a pull request
that touches the script or the workflow only the self-test runs (`live-trading-watchdog.yml:47-54`), with no
database.

## 8. The release path

| Step | Trigger | What happens |
|---|---|---|
| Image | a pull request, or a push to `prod` or `staging`, after lint, build and the security scan pass (`ci-cd-pipeline.yml:438-450`) | the image is built from `Dockerfile`, tagged `<branch>-<date>-<time>` and pushed to the GitHub container registry; a push to `prod` also tags it `latest` (`:477-481`); the image is scanned |
| Deploy | a push to `prod`, after the image job (`ci-cd-pipeline.yml:532-551`) | the trading host pulls `latest`, stops and removes the container `trade-ngin`, and brings it up again from its compose file |

The container is removed and recreated on each deploy, so a change made by hand inside it does not survive: every
change to code, the cron file or the wrapper reaches production through a push to `prod`. The deploy applies no
database migration (section 9).

## 9. Migrations

The files are in `migrations/`. The numbers present are 001 to 006, 012 to 020 and 026. They are applied to the
database in number order, by hand, before the binaries that need them are deployed: a binary that writes a
column its migration adds fails against a database without it (the one column read with a fallback is the fee of
014: without it the registry warns and prices every fill at the code default,
`src/instruments/instrument_registry.cpp:157`). Nothing in the release path applies them and no
table records which are applied.

| Number | File | What it does |
|---|---|---|
| 001 | `001_add_portfolio_type.sql` | the `portfolio_type` column that separates the system's own rows from the desk's |
| 002 | `002_corp_action_applied.sql` | `trading.corp_action_applied`, the record that makes corporate actions apply once |
| 003 | `003_equity_query_indexes.sql` | indexes for the equity read paths of a live run |
| 004 | `004_get_trading_days_portfolio_scope.sql` | `trading.get_trading_days()` under version control, with the portfolio-scoped overload |
| 005 | `005_corp_action_applied_run_date.sql` | `run_date` on `trading.corp_action_applied` |
| 006 | `006_corp_action_applied_basis_ratio.sql` | `basis_ratio` on `trading.corp_action_applied` (see [BROKER_BASIS_RECONCILIATION.md](BROKER_BASIS_RECONCILIATION.md)) |
| 012 | `012_strategy_id_width.sql` | `strategy_id` widened to 100 characters on positions, live results and signals |
| 013 | `013_executions_netting_adjustment.sql` | `netting_adjustment` on both executions tables (see [COST_MODEL.md](COST_MODEL.md)) |
| 014 | `014_contract_metadata_fee_per_contract.sql` | `"Fee Per Contract"` on `metadata.contract_metadata` |
| 015 | `015_executions_execution_type.sql` | `execution_type` (STRATEGY, ROLL, BORROW) and `instrument_id` on both executions tables (see [FUTURES_ROLLS.md](FUTURES_ROLLS.md)) |
| 016 | `016_positions_instrument_id.sql` | `instrument_id` on `trading.positions` and `backtest.final_positions` |
| 017 | `017_live_results_roll_costs.sql` | `daily_roll_costs` and `total_roll_costs` on `trading.live_results` |
| 018 | `018_backtest_results_costs.sql` | `transaction_costs`, `roll_costs` and `total_roll_fills` on `backtest.results` |
| 019 | `019_contract_metadata_fixes.sql` | corrections to nine contract metadata rows (data only) |
| 020 | `020_risk_detail.sql` | `risk_detail` on `trading.live_results` and `backtest.equity_curve` (see [RISK_MODULES.md](RISK_MODULES.md)) |
| 026 | `026_contract_metadata_ibkr_fees.sql` | the per-contract IBKR fee in each `"Fee Per Contract"` cell, with a CHECK that the fee is a positive number; needs 014 |

Each file's header is its specification: what it adds, why, what it leaves untouched, its guards (for example
whether a second apply is a no-op, or which earlier migration it needs) and how it is undone. Read the header before
applying a file.

Every migration has a rollback beside it, `<number>_<name>_rollback.sql`.

Shell tests in the same folder apply a migration and its rollback to a real PostgreSQL and check the result:

| Test | Covers |
|---|---|
| `test_001_migration.sh` | 001 |
| `test_012_strategy_id_width.sh` | 012 |
| `test_013_executions_netting_adjustment.sh` | 013 |
| `test_014_contract_metadata_fee_per_contract.sh` | 014 |
| `test_015_017_018_roll_columns.sh` | 015, 017 and 018 |
| `test_016_positions_instrument_id.sh` | 016 |
| `test_019_contract_metadata_fixes.sh` | 019 |
| `test_020_risk_detail.sh` | 020 |
| `test_026_contract_metadata_ibkr_fees.sh` | 026 |

Migrations 002 to 006 have no shell test. A test's own header says how to run it; the full form is destructive and is
pointed at a throwaway database made for the purpose, never at a database that holds data.

## 10. Troubleshooting

| Symptom | Look at |
|---|---|
| No run today | `docker ps` (is `trade-ngin` up and healthy), then `docker logs trade-ngin` for the wrapper's lines |
| "skipping: another run already holds" | a run is in progress, or a killed run left `/tmp/live_portfolio.lock` behind inside the container; remove the directory only after checking that no runner process is alive |
| "binary not found or not executable" (127) | the image, or a wrong `LIVE_BINARY` on a wrapper started by hand |
| "WARNING: /app/.cron_env missing" followed by a database failure | the container was not started through `scripts/docker-entrypoint.sh` |
| Exit 1 with "A run was missed" | replay the missing dates in order (section 6) |
| Exit 3 | the day is stored and the book is held; read the `RISK_MODULE_FAILURE` or `SIZING_HOLD` line in the log for the cause, fix it, and the next run trades from the held book |
| A watchdog issue | the reasons listed in the issue map to the four readings of section 7 |
| A hung or failing test | `ctest --test-dir build -R <name> -VV` |
