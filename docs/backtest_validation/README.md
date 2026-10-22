# Issue #113: conservative futures validation

This directory is the run protocol for [issue #113](https://github.com/AlgoGators/trade-ngin/issues/113). It evaluates stored backtest output. It does not change engine code, configuration templates, or production tables.

## State and hard gates

The evaluation has **no result** until HD supplies a dated UTC scratch database snapshot. At that handoff, record the latest merged `main` commit SHA, the snapshot timestamp, migration level, data vintage, runner binary hash, complete resolved config hash, costs, and all attempted configurations. Never put credentials in the manifest. Run one backtest at a time because the runners share `backtest` tables.

The currently pinned source snapshot (`52c17b6be39b9a98ff3f9f793f238c33236ec1fd`, inspected 2026-10-22) uses `backtest.frozen_end_date` and `lookback_years`. Its `BacktestMetricsCalculator` hardcodes 252 for return and Sharpe annualization. Check the source **again at scratch handoff** and record any change. The date-window protocol must be identical across every run in one evaluation.

1. Freeze the conservative book's configuration and choose an end date `E` and `lookback_years=N`. Run once with `(E,N)` and once with `(E+1 calendar year,N+1)` so the start date is the same. Save each run's curve before the next run resets shared tables. If explicit start/end dates have landed, use the same start and ends `E`/`E+1 year` instead.
2. Compare every curve row through `E` by `(portfolio_id,timestamp,equity)` at stored precision, ignoring `run_id`. Missing, duplicate, or differing rows fail the causality gate. Stop all numerical interpretation and report the first difference if it fails.
3. Identify and document the warm-up boundary from the runner and executions. Before a stored `phase` flag exists, never infer warm-up from flat equity alone. Carry the last warm-up equity as the base for the first scored daily return; exclude all warm-up returns.
4. Check input history for duplicate bars, the `.v.0` roll splice, and contract coverage. Choose the longest defensible common scored window. Record actual dates and row counts, not just configured lookback years.
5. Export the fixed run and every compared configuration, including calibration arms and scalar sweeps. Complete the trial ledger **before** computing PBO or DSR. Every export has a SHA-256 checksum and a manifest entry.
6. Run the fixed-book precision, calendar-year, selection-risk, and offline next-year selection analyses. Write a conservative report with methods, numbers, limitations, and exact reproduction commands. Give it to HD for review before starting equity mean reversion.

## Export contract

Use `curve_export.sql` on the scratch copy with a role that has `SELECT` only. Export one run at a time to CSV with header `run_id,portfolio_id,timestamp,equity`. The SQL's `timestamp AT TIME ZONE 'UTC'` must be checked against the stored column type at handoff; preserve the UTC instant in the CSV. If a `phase` column has landed, export it too and update the validator before using that schema. The current writer inserts `run_id`, `portfolio_id`, `timestamp`, `equity` in `src/data/postgres_database_extensions.cpp`; the older schema example in `src/backtest/README.md` is stale.

Manifest fields per run: `trial_id`, `run_id`, `portfolio_id`, `engine_sha`, `snapshot_utc`, `database`, `config_sha256`, `config_path`, `runner_sha256`, `window_protocol`, `window_start`, `window_end`, `scored_start`, `scored_end`, `warmup_last_timestamp`, `costs`, `curve_csv`, `curve_sha256`, `curve_rows`, `seed`, and `notes`. Keep local database addresses and credentials outside the tracked file. A trial with no comparable export remains in the ledger with a `missing_reason`; it still counts in the search count.

## Predeclared analysis settings

- Daily simple net returns; compounding for annual return and drawdown. Report the source engine's 252 convention and label any alternate convention separately.
- 95% percentile intervals using circular moving blocks of **20 scored days**; show block lengths 10 and 40 and iid resampling. Use 10,000 resamples and seed 42 per distinct analysis. Report the block choice as a fixed sensitivity protocol, not a data-driven optimal length.
- Sharpe uses sample standard deviation (`ddof=1`), 252 periods per year, zero risk-free rate. A series with fewer than two returns or zero variance has undefined Sharpe and no PSR/DSR claim.
- PSR tests per-period Sharpe against zero. DSR uses the full raw trial count; any effective independent count is sensitivity only. A 0.95 PSR/DSR is a descriptive threshold, not a trading release rule.
- CSCV PBO uses 10 contiguous equal-sized groups and all 252 five-group training combinations when common history permits. Selection metric is training Sharpe; ties break by lexical `trial_id`. Missing dates are a hard error unless a predeclared common-date intersection is documented with dropped dates and row counts. Two-variant PBO is a machinery demonstration only.
- Offline selection: choose by trailing scored calendar-year Sharpe, apply the chosen configuration's already stored returns to the next year, compare with the fixed configuration. A year is called out of sample only if its selection rule was frozen before it.

The final report must separate engine-stored metrics from recalculated statistics. It must show the causality table first, then warm-up and data checks, fixed-run intervals and drawdown distribution, calendar years, all-trial PBO/DSR, offline selection, and limitations. No definitive PBO/DSR is quoted if the trial ledger is incomplete.
