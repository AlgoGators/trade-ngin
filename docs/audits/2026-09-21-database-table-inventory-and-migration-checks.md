# Database inventory, migration 012 checks, and backend routing

Audit date: 2026-09-21. Scope: the approved production database connection, checked-in Trade Ngin/AlgoLens code, and the recorded disposable migration-test run. **Production remains read-only.**

## Plain-English findings

- Nginx is the request router, not the Python application backend. The checked-in routing sends API traffic to port 5000, but the checked-in deployment workflow restarts a legacy backend service on port 5001. This is a deployment-path mismatch, not evidence of two different databases.
- The **21 checks are 21 assertions in a disposable migration test**, including two fixture-setup checks. They are not checks of 21 production tables and do not establish that every production pipeline is healthy.
- This database contains **106 application/data/research/snapshot tables in 12 schemas**, plus **3 application views**. The complete table-and-column inventory is below.
- Exact reporting-table reads found **3,803 position rows, all tagged `system`**, **zero QT position rows**, **zero manual override-history rows**, and **zero risk-limit rows**. The latest system position date is **2026-08-05**.
- Migration 012's companion table is absent. Applying 012 alone would not create QT edits, publish risk limits, refresh stale positions, deploy the correct API, or prove investor-email delivery.

## 1. What “a different backend” means

Think of Nginx as the front desk: it forwards a request to the application worker that actually reads and writes the database.

The similarly named **Trade Ngin** is the C++ trading engine. **Nginx** is the web traffic router. The **AlgoLens backend** is the Python API application. The issue described here is two configured execution paths for that API, not a proposal to replace Nginx or the trading engine.

```text
Browser → Nginx → configured Python API on port 5000
Deployment workflow → restarts legacy Python API on port 5001
```

| Checked-in component | What it does | Why it matters |
|---|---|---|
| `deployment/algolens.conf` | Routes `/auth` and `/portfolio` to port 5000; the frontend route goes to port 3000. | Port 5000 is the configured API request destination. |
| `deployment/algolens-backend.service` | Runs Gunicorn on `127.0.0.1:5001`. | This is a separate service/process path, not Nginx itself. |
| `.github/workflows/deploy.yml` | Updates the server checkout and restarts the systemd backend service. | Restarting 5001 does not establish that the API serving 5000 received the code. |
| `docker-compose.prod.yml` and `DEPLOYMENT.md` | Describe the Docker API on 5000 as the authoritative backend. The production container uses an image rather than a source-code bind mount. | A changed checkout or a restart of an old image is not proof of a newly deployed API artifact. |

These findings come from the local AlgoLens checkout. **The active server's Nginx configuration, serving container, image identity, and connected database have not been verified by this inventory.** The `/health` endpoint checks database connectivity but does not identify a tested commit/build.

For a future coordinated rollout, verify the actual Nginx upstream, identify the process/container serving it, deploy the approved compatible artifact to that process, and verify its database target and build identity. No such deployment or restart was performed in this audit.

AlgoLens source references (sibling checkout):

- [Nginx configuration](../../../algolens-qt/deployment/algolens.conf)
- [Legacy backend service](../../../algolens-qt/deployment/algolens-backend.service)
- [Deploy workflow](../../../algolens-qt/.github/workflows/deploy.yml)
- [Production compose](../../../algolens-qt/docker-compose.prod.yml)
- [Deployment documentation](../../../algolens-qt/DEPLOYMENT.md)

## 2. Exactly what the 21 migration checks cover

The recorded disposable PostgreSQL 16 harness log ends with `RESULT: 21 passed, 0 failed`. This audit inspected the test source and that recorded output; it did **not** rerun the migration against production.

| # | Area | Actual assertion |
|---:|---|---|
| 1 | Setup | Create the synthetic pre-012 schema and tables. |
| 2 | Setup | Seed exactly two legacy override records. |
| 3 | Forward migration | Apply migration 012 successfully. |
| 4 | Forward migration | Keep the two existing override records unscoped (portfolio_id remains NULL). |
| 5 | Historical attribution | Map the uniquely matched legacy override to BOOK_UNIQUE. |
| 6 | Historical attribution | Leave the multi-book ambiguous legacy override without a mapping. |
| 7 | Rollback protection | Refuse rollback while inferred legacy attribution exists. |
| 8 | Future-write enforcement | Reject a new override record without portfolio_id. |
| 9 | Future-write enforcement | Accept a new override record with portfolio_id. |
| 10 | Original audit protection | Ignore an attempted UPDATE of an original audit record. |
| 11 | Original audit protection | Ignore an attempted DELETE of an original audit record. |
| 12 | Companion protection | Ignore an attempted UPDATE of a legacy-scope mapping. |
| 13 | Companion protection | Ignore an attempted DELETE of a legacy-scope mapping. |
| 14 | Repeat application | Apply the migration a second time successfully. |
| 15 | Repeat application | Create no duplicate inferred mappings on the second application. |
| 16 | Repeat application | Preserve the existing new portfolio-scoped audit record. |
| 17 | Rollback protection | Refuse rollback while portfolio-attributed audit data exists. |
| 18 | Rollback protection | Preserve companion attribution after refused rollback. |
| 19 | Rollback locking | Hold ACCESS EXCLUSIVE locks on both audit tables before rollback safety checks. |
| 20 | Empty rollback | Allow repeated safe rollback after the synthetic attribution data is cleared. |
| 21 | Empty rollback | Remove the new portfolio_id column on a safe empty rollback. |

### Tables used by those tests

| Synthetic table | Before / after 012 | Information represented |
|---|---|---|
| `trading.positions` | Existing minimal 5-column fixture; unchanged by 012. | ID, portfolio, strategy, symbol, date. Used to infer whether a historical override can be attributed to exactly one portfolio. |
| `trading.position_overrides` | 11 columns before; 12 after 012 adds `portfolio_id`. | Actor/application, strategy, symbol, before/after JSON, reason, risk-check JSON, risk-override flag, time, and then portfolio attribution. |
| `trading.position_override_legacy_scopes` | Created by 012; 4 columns. | `override_id`, inferred `portfolio_id`, `inference_basis`, `created_at`. Preserves attribution separately instead of rewriting historical audit records. |

Migration 012 adds a future-write check requiring portfolio scope, a scoped audit index, and append-only protection on the new companion table. Existing legacy override rows retain NULL in their new column; uniquely attributable history is recorded in the companion table. Ambiguous history is deliberately not guessed.

### What those checks do not prove

- The fixture is not a full copy of production. Its positions table omits quantities, prices, P&L, stream type, strategy name, and the production composite key. Its override table omits some production check constraints and indexes.
- Attribution examples cover one uniquely matched case and one multi-book ambiguous case, not every unmatched, historical-date, or anomalous-data scenario.
- The harness does not explicitly assert every new index definition or independently exercise every foreign-key/type property. Reading their SQL definitions is separate evidence.
- The rollback lock check verifies the required lock posture, not every adversarial concurrent workload.
- These are not full-schema migration-order tests, role/permission tests, live API tests, C++ tests, or investor-email delivery tests.
- No SMTP message, investor email, production override, production migration, or production rollback was executed.

Sources:

- [21-check harness](../../migrations/test_012_position_overrides_portfolio_scope.sh)
- [Migration 012](../../migrations/012_position_overrides_portfolio_scope.sql)
- [Guarded rollback](../../migrations/012_position_overrides_portfolio_scope_rollback.sql)
- Recorded run: `.cache/migration012-preparation-test.log` (local, ignored evidence).

## 3. Production inventory scope and counting rules

Catalog checkpoint: **2026-09-21T21:48:21.347876Z**. Exact reporting-count checkpoint: **2026-09-21T21:51:22.062458Z**. These are separate point-in-time observations, not a permanent guarantee or a single cross-query snapshot.

- A **schema** is a named group of tables, similar to a folder.
- A **table** stores records; a **view** is a saved query over records and is counted separately here.
- The 106-table count includes application, research, market-data, and snapshot/validation tables in this one connected database. It is not a count of every database on the server.
- PostgreSQL system catalogs (`pg_*` and `information_schema`) are excluded.
- Timescale contributes **873 internal tables**: **845 inherited storage chunks** for `futures_data.ohlcv_1d` and **28 other internal tables**. There are also **14 extension/internal views**. These are not 873 separate business datasets.
- Including those extension objects gives **979 tables and 17 views** outside PostgreSQL's standard system schemas.
- **43 of the 106 tables** are in `e2_snapshot` or `validation_backup`; their existence/names do not demonstrate a tested backup or successful restore.
- Catalog row estimates were not used as exact counts. Large market-data and people/auth tables were not scanned for row counts; an uncounted table must not be assumed empty.

| Schema | Tables | Views | Information category |
|---|---:|---:|---|
| `auth` | 1 | 0 | Application login accounts, roles, and authentication metadata. |
| `backtest` | 6 | 0 | Simulated strategy runs, performance, executions, signals, and final positions. |
| `e2_snapshot` | 26 | 0 | Snapshot/test copies of backtest and trading datasets; names do not establish a verified recovery backup. |
| `eia` | 6 | 0 | Energy data series, including imports and natural-gas/petroleum production. |
| `equities_data` | 8 | 0 | Equity prices, corporate actions, membership, ticker mapping, and data-quality records. |
| `futures_data` | 4 | 0 | Futures daily price bars and raw/provider staging data. |
| `jonah_nissan` | 2 | 0 | Research-schema EOD price datasets; ownership/purpose beyond the columns was not verified. |
| `macro_data` | 7 | 0 | Economic indicators, rates, credit spreads, liquidity, and research ETF prices. |
| `metadata` | 1 | 1 | Instrument identifiers, contract sizes, margins, tick sizes, and trading specifications. |
| `people` | 13 | 2 | People, membership, applications, teams, student/investor records, attachments, and change/import logs. |
| `trading` | 15 | 0 | Live portfolio/strategy state, positions, execution records, results, risk settings, and audit history. |
| `validation_backup` | 17 | 0 | Validation/baseline copies of live/backtest records; restore suitability was not verified. |
| **Total** | **106** | **3** | Excludes extension internals and PostgreSQL system catalogs. |

## 4. Exact counts for the reporting-related tables

These are numbers of stored rows, **not contract quantities, investor allocations, or current open positions**. Position snapshots can contain repeated symbols across dates, strategies, and portfolios.

| Table | Exact rows | What the records hold |
|---|---:|---|
| `trading.corp_action_applied` | 0 | Applied corporate-action dedup/audit records. |
| `trading.equity_curve` | 292 | Live equity curve time series. |
| `trading.executions` | 662 | Live execution/fill records. |
| `trading.live_results` | 289 | Daily live portfolio metrics and PnL. |
| `trading.live_run_metadata` | 384 | Live-run configuration metadata. |
| `trading.portfolio_assignments` | 0 | Strategy portfolio-move audit records. |
| `trading.portfolios` | 0 | Portfolio definitions. |
| `trading.position_overrides` | 0 | Manual position override audit records. |
| `trading.positions` | 3,803 | Live/historical position snapshots. |
| `trading.risk_limits` | 0 | Strategy/portfolio risk-limit policy. |
| `trading.signals` | 11,931 | Generated strategy signals. |
| `trading.strategy_book_memberships` | 4 | Strategy-to-portfolio membership links. |
| `trading.strategy_lifecycle_log` | 0 | Strategy lifecycle transition audit log. |
| `trading.strategy_registry` | 4 | Strategy display/configuration registry. |
| `trading.strategy_trading_days_metadata` | 4 | Strategy live-start/annualization metadata. |
| `metadata.contract_metadata` | 40 | Instrument identifiers, contract sizes, margins, tick sizes, and related specifications. |

### Implications for QT position numbers in investor reports

- All 3,803 position rows have `portfolio_type = 'system'`; no QT stream rows were found in this connection.
- `position_overrides` is empty, so this database presently has no stored manual-edit audit history to validate end-to-end.
- `risk_limits` and the `portfolios` catalog are empty even though strategy/book membership rows exist. These are readiness gaps to resolve/understand against the actual deployed API and intended book configuration; row counts alone do not establish how all application routes behave.
- The latest system position date (2026-08-05) does not demonstrate current daily ingestion. Confirm whether this is the intended live database/feed before claiming a current daily pipeline.
- The migration-012 companion table is absent. If applied without unrelated schema changes, 012 would raise `trading` from 15 to 16 tables and this application inventory from 106 to 107. It also adds one column to `position_overrides`; it does not change held quantities in `positions`.
- The `people.investor` table's existence does not prove the report mailer's recipient source is wired to it. This inventory did not trace or exercise delivery.
- The user's requirement remains unchanged: preserve the daily trading email's layout and other content, changing only displayed position numbers to the intended QT-adjusted values. No email code or template was changed in this audit.

## 5. Every application/data table and the information it can hold

Columns below are actual live catalog column names. Descriptions marked **source-backed** were mapped to checked-in code/migrations; **inferred** descriptions are interpretations of table/column names and do not establish business ownership or active usage. Listing columns describes the schema, not the presence or correctness of values in every row. No personal records, password hashes, investor details, or position-level data samples are reproduced.

### auth — 1 tables

Application login accounts, roles, and authentication metadata.

#### auth.users

Application identity and authentication users. Purpose basis: **source-backed**.

Columns (9): `id`, `email`, `password_hash`, `created_at`, `role`, `first_name`, `last_name`, `team`, `force_password_change`.

Primary key: `id`.

### backtest — 6 tables

Simulated strategy runs, performance, executions, signals, and final positions.

#### backtest.equity_curve

Backtest equity time series. Purpose basis: **source-backed**.

Columns (4): `run_id`, `timestamp`, `equity`, `portfolio_id`.

Primary key: `run_id`, `timestamp`.

#### backtest.executions

Backtest execution/fill records. Purpose basis: **source-backed**.

Columns (16): `id`, `run_id`, `execution_id`, `order_id`, `timestamp`, `symbol`, `side`, `quantity`, `price`, `commissions_fees`, `is_partial`, `strategy_id`, `portfolio_id`, `implicit_price_impact`, `slippage_market_impact`, `total_transaction_costs`.

Primary key: `run_id`, `strategy_id`, `execution_id`.

#### backtest.final_positions

Backtest ending position snapshot. Purpose basis: **source-backed**.

Columns (11): `run_id`, `symbol`, `quantity`, `average_price`, `unrealized_pnl`, `realized_pnl`, `strategy_id`, `last_update`, `updated_at`, `date`, `portfolio_id`.

Primary key: `run_id`, `strategy_id`, `date`, `symbol`.

#### backtest.results

Backtest aggregate performance and risk metrics. Purpose basis: **source-backed**.

Columns (23): `run_id`, `start_date`, `end_date`, `total_return`, `sharpe_ratio`, `sortino_ratio`, `max_drawdown`, `calmar_ratio`, `volatility`, `total_trades`, `win_rate`, `profit_factor`, `avg_win`, `avg_loss`, `max_win`, `max_loss`, `avg_holding_period`, `var_95`, `cvar_95`, `beta`, `correlation`, `downside_volatility`, `portfolio_id`.

Primary key: `run_id`.

#### backtest.run_metadata

Backtest run configuration and identifiers. Purpose basis: **source-backed**.

Columns (12): `run_id`, `name`, `description`, `date_inserted`, `start_date`, `end_date`, `hyperparameters`, `portfolio_run_id`, `strategy_allocation`, `portfolio_config`, `strategy_id`, `portfolio_id`.

Primary key: none declared in the catalog.

#### backtest.signals

Backtest strategy signals. Purpose basis: **source-backed**.

Columns (9): `id`, `run_id`, `strategy_id`, `symbol`, `signal_value`, `timestamp`, `signal_strength`, `portfolio_run_id`, `portfolio_id`.

Primary key: `id`.

### e2_snapshot — 26 tables

Snapshot/test copies of backtest and trading datasets; names do not establish a verified recovery backup.

#### e2_snapshot.ab10_old_eq

Historical snapshot of equity-value time series. Purpose basis: **inferred**.

Columns (6): `id`, `strategy_id`, `timestamp`, `equity`, `portfolio_id`, `portfolio_type`.

Primary key: none declared in the catalog.

#### e2_snapshot.ab10_old_exec

Historical snapshot of execution/fill records. Purpose basis: **inferred**.

Columns (17): `exec_id`, `order_id`, `symbol`, `side`, `quantity`, `price`, `execution_time`, `commissions_fees`, `is_partial`, `created_at`, `strategy_id`, `strategy_name`, `date`, `portfolio_id`, `implicit_price_impact`, `slippage_market_impact`, `total_transaction_costs`.

Primary key: none declared in the catalog.

#### e2_snapshot.ab10_old_pos

Historical snapshot of position snapshots. Purpose basis: **inferred**.

Columns (12): `symbol`, `quantity`, `average_price`, `daily_unrealized_pnl`, `daily_realized_pnl`, `last_update`, `updated_at`, `strategy_id`, `strategy_name`, `date`, `portfolio_id`, `portfolio_type`.

Primary key: none declared in the catalog.

#### e2_snapshot.ab10_old_res

Historical snapshot of performance/result metrics. Purpose basis: **inferred**.

Columns (49): `id`, `strategy_id`, `date`, `total_annualized_return`, `volatility`, `total_pnl`, `total_unrealized_pnl`, `total_realized_pnl`, `current_portfolio_value`, `portfolio_var`, `gross_leverage`, `net_leverage`, `portfolio_leverage`, `max_correlation`, `jump_risk`, `risk_scale`, `active_positions`, `config`, `created_at`, `daily_pnl`, `total_transaction_costs`, `daily_realized_pnl`, `daily_unrealized_pnl`, `gross_notional`, `net_notional`, `daily_transaction_costs`, `daily_return`, `equity_to_margin_ratio`, `margin_cushion`, `margin_posted`, `cash_available`, `total_cumulative_return`, `portfolio_id`, `win_rate`, `avg_win`, `avg_loss`, `profit_factor`, `best_day`, `worst_day`, `downside_deviation`, `winning_days`, `losing_days`, `total_days`, `gross_profit`, `gross_loss`, `sharpe_ratio`, `sortino_ratio`, `max_drawdown`, `total_dividend_income`.

Primary key: none declared in the catalog.

#### e2_snapshot.ab_old_equity

Historical snapshot of equity-value time series. Purpose basis: **inferred**.

Columns (6): `id`, `strategy_id`, `timestamp`, `equity`, `portfolio_id`, `portfolio_type`.

Primary key: none declared in the catalog.

#### e2_snapshot.ab_old_execs

Historical snapshot of execution/fill records. Purpose basis: **inferred**.

Columns (17): `exec_id`, `order_id`, `symbol`, `side`, `quantity`, `price`, `execution_time`, `commissions_fees`, `is_partial`, `created_at`, `strategy_id`, `strategy_name`, `date`, `portfolio_id`, `implicit_price_impact`, `slippage_market_impact`, `total_transaction_costs`.

Primary key: none declared in the catalog.

#### e2_snapshot.ab_old_positions

Historical snapshot of position snapshots. Purpose basis: **inferred**.

Columns (12): `symbol`, `quantity`, `average_price`, `daily_unrealized_pnl`, `daily_realized_pnl`, `last_update`, `updated_at`, `strategy_id`, `strategy_name`, `date`, `portfolio_id`, `portfolio_type`.

Primary key: none declared in the catalog.

#### e2_snapshot.ab_old_results

Historical snapshot of performance/result metrics. Purpose basis: **inferred**.

Columns (49): `id`, `strategy_id`, `date`, `total_annualized_return`, `volatility`, `total_pnl`, `total_unrealized_pnl`, `total_realized_pnl`, `current_portfolio_value`, `portfolio_var`, `gross_leverage`, `net_leverage`, `portfolio_leverage`, `max_correlation`, `jump_risk`, `risk_scale`, `active_positions`, `config`, `created_at`, `daily_pnl`, `total_transaction_costs`, `daily_realized_pnl`, `daily_unrealized_pnl`, `gross_notional`, `net_notional`, `daily_transaction_costs`, `daily_return`, `equity_to_margin_ratio`, `margin_cushion`, `margin_posted`, `cash_available`, `total_cumulative_return`, `portfolio_id`, `win_rate`, `avg_win`, `avg_loss`, `profit_factor`, `best_day`, `worst_day`, `downside_deviation`, `winning_days`, `losing_days`, `total_days`, `gross_profit`, `gross_loss`, `sharpe_ratio`, `sortino_ratio`, `max_drawdown`, `total_dividend_income`.

Primary key: none declared in the catalog.

#### e2_snapshot.backtest_equity_curve

Historical snapshot of equity-value time series. Purpose basis: **inferred**.

Columns (4): `run_id`, `timestamp`, `equity`, `portfolio_id`.

Primary key: none declared in the catalog.

#### e2_snapshot.backtest_executions

Historical snapshot of execution/fill records. Purpose basis: **inferred**.

Columns (16): `id`, `run_id`, `execution_id`, `order_id`, `timestamp`, `symbol`, `side`, `quantity`, `price`, `commissions_fees`, `is_partial`, `strategy_id`, `portfolio_id`, `implicit_price_impact`, `slippage_market_impact`, `total_transaction_costs`.

Primary key: none declared in the catalog.

#### e2_snapshot.backtest_final_positions

Historical snapshot of position snapshots. Purpose basis: **inferred**.

Columns (11): `run_id`, `symbol`, `quantity`, `average_price`, `unrealized_pnl`, `realized_pnl`, `strategy_id`, `last_update`, `updated_at`, `date`, `portfolio_id`.

Primary key: none declared in the catalog.

#### e2_snapshot.backtest_results

Historical snapshot of performance/result metrics. Purpose basis: **inferred**.

Columns (23): `run_id`, `start_date`, `end_date`, `total_return`, `sharpe_ratio`, `sortino_ratio`, `max_drawdown`, `calmar_ratio`, `volatility`, `total_trades`, `win_rate`, `profit_factor`, `avg_win`, `avg_loss`, `max_win`, `max_loss`, `avg_holding_period`, `var_95`, `cvar_95`, `beta`, `correlation`, `downside_volatility`, `portfolio_id`.

Primary key: none declared in the catalog.

#### e2_snapshot.backtest_run_metadata

Historical snapshot of run/strategy configuration metadata. Purpose basis: **inferred**.

Columns (12): `run_id`, `name`, `description`, `date_inserted`, `start_date`, `end_date`, `hyperparameters`, `portfolio_run_id`, `strategy_allocation`, `portfolio_config`, `strategy_id`, `portfolio_id`.

Primary key: none declared in the catalog.

#### e2_snapshot.backtest_signals

Historical snapshot of strategy signal records. Purpose basis: **inferred**.

Columns (9): `id`, `run_id`, `strategy_id`, `symbol`, `signal_value`, `timestamp`, `signal_strength`, `portfolio_run_id`, `portfolio_id`.

Primary key: none declared in the catalog.

#### e2_snapshot.preb5_live_results

Historical snapshot of performance/result metrics. Purpose basis: **inferred**.

Columns (49): `id`, `strategy_id`, `date`, `total_annualized_return`, `volatility`, `total_pnl`, `total_unrealized_pnl`, `total_realized_pnl`, `current_portfolio_value`, `portfolio_var`, `gross_leverage`, `net_leverage`, `portfolio_leverage`, `max_correlation`, `jump_risk`, `risk_scale`, `active_positions`, `config`, `created_at`, `daily_pnl`, `total_transaction_costs`, `daily_realized_pnl`, `daily_unrealized_pnl`, `gross_notional`, `net_notional`, `daily_transaction_costs`, `daily_return`, `equity_to_margin_ratio`, `margin_cushion`, `margin_posted`, `cash_available`, `total_cumulative_return`, `portfolio_id`, `win_rate`, `avg_win`, `avg_loss`, `profit_factor`, `best_day`, `worst_day`, `downside_deviation`, `winning_days`, `losing_days`, `total_days`, `gross_profit`, `gross_loss`, `sharpe_ratio`, `sortino_ratio`, `max_drawdown`, `total_dividend_income`.

Primary key: none declared in the catalog.

#### e2_snapshot.preb5_positions

Historical snapshot of position snapshots. Purpose basis: **inferred**.

Columns (12): `symbol`, `quantity`, `average_price`, `daily_unrealized_pnl`, `daily_realized_pnl`, `last_update`, `updated_at`, `strategy_id`, `strategy_name`, `date`, `portfolio_id`, `portfolio_type`.

Primary key: none declared in the catalog.

#### e2_snapshot.trading_corp_action_applied

Historical snapshot of applied corporate-action records. Purpose basis: **inferred**.

Columns (10): `portfolio_id`, `strategy_id`, `strategy_name`, `symbol`, `action_type`, `ex_date`, `applied_at`, `qty_held`, `dividend_per_share`, `total_cash`.

Primary key: none declared in the catalog.

#### e2_snapshot.trading_equity_curve

Historical snapshot of equity-value time series. Purpose basis: **inferred**.

Columns (6): `id`, `strategy_id`, `timestamp`, `equity`, `portfolio_id`, `portfolio_type`.

Primary key: none declared in the catalog.

#### e2_snapshot.trading_executions

Historical snapshot of execution/fill records. Purpose basis: **inferred**.

Columns (17): `exec_id`, `order_id`, `symbol`, `side`, `quantity`, `price`, `execution_time`, `commissions_fees`, `is_partial`, `created_at`, `strategy_id`, `strategy_name`, `date`, `portfolio_id`, `implicit_price_impact`, `slippage_market_impact`, `total_transaction_costs`.

Primary key: none declared in the catalog.

#### e2_snapshot.trading_live_results

Historical snapshot of performance/result metrics. Purpose basis: **inferred**.

Columns (49): `id`, `strategy_id`, `date`, `total_annualized_return`, `volatility`, `total_pnl`, `total_unrealized_pnl`, `total_realized_pnl`, `current_portfolio_value`, `portfolio_var`, `gross_leverage`, `net_leverage`, `portfolio_leverage`, `max_correlation`, `jump_risk`, `risk_scale`, `active_positions`, `config`, `created_at`, `daily_pnl`, `total_transaction_costs`, `daily_realized_pnl`, `daily_unrealized_pnl`, `gross_notional`, `net_notional`, `daily_transaction_costs`, `daily_return`, `equity_to_margin_ratio`, `margin_cushion`, `margin_posted`, `cash_available`, `total_cumulative_return`, `portfolio_id`, `win_rate`, `avg_win`, `avg_loss`, `profit_factor`, `best_day`, `worst_day`, `downside_deviation`, `winning_days`, `losing_days`, `total_days`, `gross_profit`, `gross_loss`, `sharpe_ratio`, `sortino_ratio`, `max_drawdown`, `total_dividend_income`.

Primary key: none declared in the catalog.

#### e2_snapshot.trading_live_run_metadata

Historical snapshot of run/strategy configuration metadata. Purpose basis: **inferred**.

Columns (8): `id`, `date`, `strategy_id`, `portfolio_id`, `strategy_allocations`, `portfolio_config`, `strategy_configs`, `created_at`.

Primary key: none declared in the catalog.

#### e2_snapshot.trading_positions

Historical snapshot of position snapshots. Purpose basis: **inferred**.

Columns (12): `symbol`, `quantity`, `average_price`, `daily_unrealized_pnl`, `daily_realized_pnl`, `last_update`, `updated_at`, `strategy_id`, `strategy_name`, `date`, `portfolio_id`, `portfolio_type`.

Primary key: none declared in the catalog.

#### e2_snapshot.trading_signals

Historical snapshot of strategy signal records. Purpose basis: **inferred**.

Columns (8): `id`, `strategy_id`, `symbol`, `signal_value`, `timestamp`, `created_at`, `portfolio_id`, `strategy_name`.

Primary key: none declared in the catalog.

#### e2_snapshot.trading_strategy_lifecycle_log

Historical snapshot of strategy lifecycle history. Purpose basis: **inferred**.

Columns (8): `id`, `strategy_id`, `before_state`, `after_state`, `reason`, `user_id`, `created_at`, `updated_at`.

Primary key: none declared in the catalog.

#### e2_snapshot.trading_strategy_registry

Historical snapshot of strategy configuration records. Purpose basis: **inferred**.

Columns (14): `id`, `strategy_type`, `portfolio_id`, `name`, `description`, `initial_equity`, `managers`, `is_active`, `sort_order`, `created_at`, `updated_at`, `lifecycle`, `incubation_started_at`, `mock_capital`.

Primary key: none declared in the catalog.

#### e2_snapshot.trading_strategy_trading_days_metadata

Historical snapshot of run/strategy configuration metadata. Purpose basis: **inferred**.

Columns (6): `strategy_id`, `live_start_date`, `description`, `created_at`, `updated_at`, `portfolio_id`.

Primary key: none declared in the catalog.

### eia — 6 tables

Energy data series, including imports and natural-gas/petroleum production.

#### eia.imports

Normalized EIA energy observations. Purpose basis: **inferred**.

Columns (13): `time`, `originId`, `originName`, `originType`, `originTypeName`, `destinationId`, `destinationName`, `destinationType`, `destinationTypeName`, `gradeId`, `gradeName`, `value`, `series_id`.

Primary key: `time`, `originId`, `originName`, `originType`, `originTypeName`, `destinationId`, `destinationName`, `destinationType`, `destinationTypeName`, `gradeId`, `gradeName`, `series_id`.

#### eia.imports_raw

Raw EIA energy observations. Purpose basis: **inferred**.

Columns (13): `time`, `originId`, `originName`, `originType`, `originTypeName`, `destinationId`, `destinationName`, `destinationType`, `destinationTypeName`, `gradeId`, `gradeName`, `value`, `series_id`.

Primary key: `time`, `originId`, `originName`, `originType`, `originTypeName`, `destinationId`, `destinationName`, `destinationType`, `destinationTypeName`, `gradeId`, `gradeName`, `series_id`.

#### eia.ng_prod

Normalized EIA energy observations. Purpose basis: **inferred**.

Columns (11): `time`, `duoarea`, `area-name`, `product`, `product-name`, `process`, `process-name`, `series`, `series-description`, `value`, `series_id`.

Primary key: `time`, `duoarea`, `area-name`, `product`, `product-name`, `process`, `process-name`, `series`, `series-description`, `series_id`.

#### eia.ng_prod_raw

Raw EIA energy observations. Purpose basis: **inferred**.

Columns (11): `time`, `duoarea`, `area-name`, `product`, `product-name`, `process`, `process-name`, `series`, `series-description`, `value`, `series_id`.

Primary key: `time`, `duoarea`, `area-name`, `product`, `product-name`, `process`, `process-name`, `series`, `series-description`, `series_id`.

#### eia.petro_prod

Normalized EIA energy observations. Purpose basis: **inferred**.

Columns (11): `time`, `duoarea`, `area-name`, `product`, `product-name`, `process`, `process-name`, `series`, `series-description`, `value`, `series_id`.

Primary key: `time`, `duoarea`, `area-name`, `product`, `product-name`, `process`, `process-name`, `series`, `series-description`, `series_id`.

#### eia.petro_prod_raw

Raw EIA energy observations. Purpose basis: **inferred**.

Columns (11): `time`, `duoarea`, `area-name`, `product`, `product-name`, `process`, `process-name`, `series`, `series-description`, `value`, `series_id`.

Primary key: `time`, `duoarea`, `area-name`, `product`, `product-name`, `process`, `process-name`, `series`, `series-description`, `series_id`.

### equities_data — 8 tables

Equity prices, corporate actions, membership, ticker mapping, and data-quality records.

#### equities_data.corporate_action

Equity corporate-action reference events. Purpose basis: **inferred**.

Columns (7): `date`, `action`, `ticker`, `name`, `value`, `contraticker`, `contraname`.

Primary key: none declared in the catalog.

#### equities_data.coverage_gaps

Equity market-data coverage gap ledger. Purpose basis: **inferred**.

Columns (9): `ticker`, `reason`, `membership_from`, `membership_to`, `tiingo_name`, `tiingo_start`, `tiingo_end`, `first_attempted`, `note`.

Primary key: `ticker`.

#### equities_data.ohlcv_1d

Adjusted daily equity OHLCV bars. Purpose basis: **inferred**.

Columns (15): `time`, `symbol`, `open`, `high`, `low`, `close`, `volume`, `adj_open`, `adj_high`, `adj_low`, `adjusted_close`, `adj_volume`, `div_cash`, `split_factor`, `delisting_date`.

Primary key: `symbol`, `time`.

#### equities_data.ohlcv_1d_raw

Raw daily equity OHLCV bars. Purpose basis: **inferred**.

Columns (14): `time`, `open`, `high`, `low`, `close`, `volume`, `adj_open`, `adj_high`, `adj_low`, `adjusted_close`, `adj_volume`, `div_cash`, `split_factor`, `symbol`.

Primary key: none declared in the catalog.

#### equities_data.sharadar_ohlcv_1d

Sharadar daily equity OHLCV bars. Purpose basis: **inferred**.

Columns (10): `ticker`, `date`, `open`, `high`, `low`, `close`, `volume`, `closeadj`, `closeunadj`, `lastupdated`.

Primary key: none declared in the catalog.

#### equities_data.sp500_membership

S&P 500 membership intervals. Purpose basis: **inferred**.

Columns (3): `ticker`, `start_date`, `end_date`.

Primary key: none declared in the catalog.

#### equities_data.ticker_aliases

Historical-to-current ticker aliases. Purpose basis: **inferred**.

Columns (4): `historical_ticker`, `current_symbol`, `effective_until`, `note`.

Primary key: `historical_ticker`, `current_symbol`.

#### equities_data.verified_absent_bars

Verified missing equity bars. Purpose basis: **inferred**.

Columns (4): `symbol`, `bar_date`, `checked_at`, `note`.

Primary key: `symbol`, `bar_date`.

### futures_data — 4 tables

Futures daily price bars and raw/provider staging data.

#### futures_data.new_data_ohlcv_1d

Staged/new futures daily OHLCV data. Purpose basis: **source-backed**.

Columns (7): `time`, `symbol`, `volume`, `open`, `high`, `low`, `close`.

Primary key: `time`, `symbol`.

#### futures_data.new_data_ohlcv_1d_raw

Raw futures daily OHLCV ingestion. Purpose basis: **source-backed**.

Columns (10): `ts_event`, `symbol`, `open`, `high`, `low`, `close`, `volume`, `rtype`, `publisher_id`, `instrument_id`.

Primary key: `ts_event`, `symbol`.

#### futures_data.ohlcv_1d

Canonical futures daily OHLCV bars. Purpose basis: **source-backed**.

Columns (7): `time`, `symbol`, `volume`, `open`, `high`, `low`, `close`.

Primary key: none declared in the catalog.

#### futures_data.ohlcv_1d_raw

Raw futures daily OHLCV ingestion. Purpose basis: **source-backed**.

Columns (10): `ts_event`, `symbol`, `open`, `high`, `low`, `close`, `volume`, `rtype`, `publisher_id`, `instrument_id`.

Primary key: `ts_event`, `symbol`.

### jonah_nissan — 2 tables

Research-schema EOD price datasets; ownership/purpose beyond the columns was not verified.

#### jonah_nissan.eodhd_data

Normalized EODHD daily market data. Purpose basis: **inferred**.

Columns (8): `time`, `symbol`, `open`, `high`, `low`, `close`, `adjusted_close`, `volume`.

Primary key: `time`, `symbol`.

#### jonah_nissan.eodhd_raw

Raw EODHD market-data observations. Purpose basis: **inferred**.

Columns (8): `time`, `symbol`, `open`, `high`, `low`, `close`, `adjusted_close`, `volume`.

Primary key: `time`, `symbol`.

### macro_data — 7 tables

Economic indicators, rates, credit spreads, liquidity, and research ETF prices.

#### macro_data.bsts_etf_prices

Daily macro/ETF price series. Purpose basis: **inferred**.

Columns (8): `date`, `symbol`, `open`, `high`, `low`, `close`, `adjusted_close`, `volume`.

Primary key: `date`, `symbol`.

#### macro_data.credit_spreads

Credit-spread indicators. Purpose basis: **inferred**.

Columns (3): `date`, `ig_credit_spread`, `high_yield_spread`.

Primary key: `date`.

#### macro_data.growth

Growth and labor indicators. Purpose basis: **inferred**.

Columns (11): `date`, `nonfarm_payrolls`, `unemployment_rate`, `manufacturing_capacity_util`, `industrial_production`, `retail_sales`, `gdp`, `consumer_sentiment`, `manufacturing_employment`, `cfnai`, `init_claims`.

Primary key: `date`.

#### macro_data.inflation

Inflation indicators. Purpose basis: **inferred**.

Columns (6): `date`, `cpi`, `core_cpi`, `core_pce`, `breakeven_5y`, `breakeven_10y`.

Primary key: `date`.

#### macro_data.liquidity

Liquidity and money-supply indicators. Purpose basis: **inferred**.

Columns (4): `date`, `m2_money_supply`, `ted_spread`, `fed_balance_sheet`.

Primary key: `date`.

#### macro_data.market

Market regime indicators. Purpose basis: **inferred**.

Columns (6): `date`, `vix`, `dxy`, `tips_10y`, `gdp_nowcast`, `wti_crude`.

Primary key: `date`.

#### macro_data.yield_curve

Interest-rate and yield-curve indicators. Purpose basis: **inferred**.

Columns (8): `date`, `treasury_2y`, `treasury_10y`, `yield_spread_10y_2y`, `fed_funds_rate`, `treasury_30y`, `sofr`, `butterfly_spread`.

Primary key: `date`.

### metadata — 1 tables

Instrument identifiers, contract sizes, margins, tick sizes, and trading specifications.

#### metadata.contract_metadata

Instrument and contract specifications. Purpose basis: **source-backed**.

Columns (22): `Databento Symbol`, `IB Symbol`, `Name`, `Exchange`, `Intraday Initial Margin`, `Intraday Maintenance Margin`, `Overnight Initial Margin`, `Overnight Maintenance Margin`, `Asset Type`, `Sector`, `Contract Size`, `Units`, `Minimum Price Fluctuation`, `Tick Size`, `Settlement Type`, `Trading Hours (EST)`, `Data Provider`, `Dataset`, `Newest Month Additions`, `Contract Months`, `Time of Expiry`, `Additional Notes`.

Primary key: none declared in the catalog.

### people — 13 tables

People, membership, applications, teams, student/investor records, attachments, and change/import logs.

#### people.application

Applicant submissions. Purpose basis: **inferred**.

Columns (16): `id`, `person_id`, `cycle_term`, `cycle_year`, `form_id`, `external_response_id`, `raw_response`, `stage`, `stage_rank`, `outcome`, `resume_attachment_id`, `submitted_at`, `deleted_at`, `row_version`, `created_at`, `updated_at`.

Primary key: `id`.

#### people.application_form

Application form definitions. Purpose basis: **inferred**.

Columns (13): `id`, `cycle_term`, `cycle_year`, `track`, `form_url`, `external_form_id`, `question_map`, `opened_at`, `closed_at`, `deleted_at`, `row_version`, `created_at`, `updated_at`.

Primary key: `id`.

#### people.application_team_preference

Applicant team preferences. Purpose basis: **inferred**.

Columns (4): `application_id`, `team_id`, `rank`, `created_at`.

Primary key: `application_id`, `team_id`.

#### people.attachment

Applicant attachments and file metadata. Purpose basis: **inferred**.

Columns (14): `id`, `person_id`, `kind`, `source_url`, `original_filename`, `content`, `content_type`, `size_bytes`, `sha256`, `fetched_at`, `deleted_at`, `row_version`, `uploaded_at`, `updated_at`.

Primary key: `id`.

#### people.change_log

People-data row change audit log. Purpose basis: **inferred**.

Columns (9): `id`, `batch_id`, `table_name`, `row_id`, `operation`, `before`, `after`, `actor`, `occurred_at`.

Primary key: `id`.

#### people.import_batch

People-data import batches and status. Purpose basis: **inferred**.

Columns (12): `id`, `source`, `filename`, `file_sha256`, `actor`, `note`, `row_count`, `status`, `started_at`, `finished_at`, `reverted_at`, `reverted_by`.

Primary key: `id`.

#### people.investor

Investor records linked to people. Purpose basis: **inferred**.

Columns (5): `person_id`, `deleted_at`, `row_version`, `created_at`, `updated_at`.

Primary key: `person_id`.

#### people.member

Program member records. Purpose basis: **inferred**.

Columns (10): `id`, `person_id`, `student_id`, `is_leadership`, `joined_on`, `left_on`, `deleted_at`, `row_version`, `created_at`, `updated_at`.

Primary key: `id`.

#### people.member_team

Member-to-team assignments. Purpose basis: **inferred**.

Columns (9): `id`, `member_id`, `team_id`, `started_on`, `ended_on`, `deleted_at`, `row_version`, `created_at`, `updated_at`.

Primary key: `id`.

#### people.person

People master records. Purpose basis: **inferred**.

Columns (10): `id`, `first_name`, `last_name`, `email`, `person_type`, `name_key`, `deleted_at`, `row_version`, `created_at`, `updated_at`.

Primary key: `id`.

#### people.student

Student records. Purpose basis: **inferred**.

Columns (11): `id`, `person_id`, `gpa`, `class_standing`, `grad_term`, `grad_year`, `grad_sort`, `deleted_at`, `row_version`, `created_at`, `updated_at`.

Primary key: `id`.

#### people.student_major

Student majors. Purpose basis: **inferred**.

Columns (8): `id`, `student_id`, `field`, `kind`, `deleted_at`, `row_version`, `created_at`, `updated_at`.

Primary key: `id`.

#### people.team

Team definitions. Purpose basis: **inferred**.

Columns (8): `id`, `name`, `slug`, `active`, `deleted_at`, `row_version`, `created_at`, `updated_at`.

Primary key: `id`.

### trading — 15 tables

Live portfolio/strategy state, positions, execution records, results, risk settings, and audit history.

#### trading.corp_action_applied

Applied corporate-action dedup/audit records. Purpose basis: **source-backed**.

Columns (12): `portfolio_id`, `strategy_id`, `strategy_name`, `symbol`, `action_type`, `ex_date`, `applied_at`, `qty_held`, `dividend_per_share`, `total_cash`, `run_date`, `basis_ratio`.

Primary key: `portfolio_id`, `strategy_id`, `strategy_name`, `symbol`, `action_type`, `ex_date`.

#### trading.equity_curve

Live equity curve time series. Purpose basis: **source-backed**.

Columns (6): `id`, `strategy_id`, `timestamp`, `equity`, `portfolio_id`, `portfolio_type`.

Primary key: `id`.

#### trading.executions

Live execution/fill records. Purpose basis: **source-backed**.

Columns (17): `exec_id`, `order_id`, `symbol`, `side`, `quantity`, `price`, `execution_time`, `commissions_fees`, `is_partial`, `created_at`, `strategy_id`, `strategy_name`, `date`, `portfolio_id`, `implicit_price_impact`, `slippage_market_impact`, `total_transaction_costs`.

Primary key: `portfolio_id`, `strategy_id`, `strategy_name`, `date`, `exec_id`.

#### trading.live_results

Daily live portfolio metrics and PnL. Purpose basis: **source-backed**.

Columns (49): `id`, `strategy_id`, `date`, `total_annualized_return`, `volatility`, `total_pnl`, `total_unrealized_pnl`, `total_realized_pnl`, `current_portfolio_value`, `portfolio_var`, `gross_leverage`, `net_leverage`, `portfolio_leverage`, `max_correlation`, `jump_risk`, `risk_scale`, `active_positions`, `config`, `created_at`, `daily_pnl`, `total_transaction_costs`, `daily_realized_pnl`, `daily_unrealized_pnl`, `gross_notional`, `net_notional`, `daily_transaction_costs`, `daily_return`, `equity_to_margin_ratio`, `margin_cushion`, `margin_posted`, `cash_available`, `total_cumulative_return`, `portfolio_id`, `win_rate`, `avg_win`, `avg_loss`, `profit_factor`, `best_day`, `worst_day`, `downside_deviation`, `winning_days`, `losing_days`, `total_days`, `gross_profit`, `gross_loss`, `sharpe_ratio`, `sortino_ratio`, `max_drawdown`, `total_dividend_income`.

Primary key: `id`.

#### trading.live_run_metadata

Live-run configuration metadata. Purpose basis: **inferred**.

Columns (8): `id`, `date`, `strategy_id`, `portfolio_id`, `strategy_allocations`, `portfolio_config`, `strategy_configs`, `created_at`.

Primary key: `id`.

#### trading.portfolio_assignments

Strategy portfolio-move audit records. Purpose basis: **source-backed**.

Columns (10): `id`, `strategy_id`, `user_id`, `from_portfolio_id`, `to_portfolio_id`, `lifecycle_at_move`, `reason`, `consequences`, `acknowledged`, `created_at`.

Primary key: `id`.

#### trading.portfolios

Portfolio definitions. Purpose basis: **source-backed**.

Columns (5): `portfolio_id`, `name`, `description`, `created_by`, `created_at`.

Primary key: `portfolio_id`.

#### trading.position_overrides

Manual position override audit records. Purpose basis: **source-backed**.

Columns (11): `id`, `user_id`, `source_app`, `strategy_id`, `symbol`, `before_state`, `after_state`, `reason`, `risk_check_result`, `overrode_risk`, `created_at`.

Primary key: `id`.

#### trading.positions

Live/historical position snapshots. Purpose basis: **source-backed**.

Columns (12): `symbol`, `quantity`, `average_price`, `daily_unrealized_pnl`, `daily_realized_pnl`, `last_update`, `updated_at`, `strategy_id`, `strategy_name`, `date`, `portfolio_id`, `portfolio_type`.

Primary key: `portfolio_id`, `strategy_id`, `strategy_name`, `date`, `symbol`, `portfolio_type`.

#### trading.risk_limits

Strategy/portfolio risk-limit policy. Purpose basis: **source-backed**.

Columns (5): `id`, `strategy_id`, `portfolio_id`, `limits`, `published_at`.

Primary key: `id`.

#### trading.signals

Generated strategy signals. Purpose basis: **inferred**.

Columns (8): `id`, `strategy_id`, `symbol`, `signal_value`, `timestamp`, `created_at`, `portfolio_id`, `strategy_name`.

Primary key: `id`.

#### trading.strategy_book_memberships

Strategy-to-portfolio membership links. Purpose basis: **source-backed**.

Columns (4): `strategy_id`, `portfolio_id`, `added_by`, `added_at`.

Primary key: `strategy_id`, `portfolio_id`.

#### trading.strategy_lifecycle_log

Strategy lifecycle transition audit log. Purpose basis: **source-backed**.

Columns (8): `id`, `strategy_id`, `before_state`, `after_state`, `reason`, `user_id`, `created_at`, `updated_at`.

Primary key: `id`.

#### trading.strategy_registry

Strategy display/configuration registry. Purpose basis: **source-backed**.

Columns (14): `id`, `strategy_type`, `portfolio_id`, `name`, `description`, `initial_equity`, `managers`, `is_active`, `sort_order`, `created_at`, `updated_at`, `lifecycle`, `incubation_started_at`, `mock_capital`.

Primary key: `id`.

#### trading.strategy_trading_days_metadata

Strategy live-start/annualization metadata. Purpose basis: **inferred**.

Columns (6): `strategy_id`, `live_start_date`, `description`, `created_at`, `updated_at`, `portfolio_id`.

Primary key: `portfolio_id`, `strategy_id`.

### validation_backup — 17 tables

Validation/baseline copies of live/backtest records; restore suitability was not verified.

#### validation_backup.bt_baseline_equity

Validation/comparison copy of equity-value time series. Purpose basis: **inferred**.

Columns (4): `run_id`, `timestamp`, `equity`, `portfolio_id`.

Primary key: none declared in the catalog.

#### validation_backup.bt_baseline_positions

Validation/comparison copy of position snapshots. Purpose basis: **inferred**.

Columns (11): `run_id`, `symbol`, `quantity`, `average_price`, `unrealized_pnl`, `realized_pnl`, `strategy_id`, `last_update`, `updated_at`, `date`, `portfolio_id`.

Primary key: none declared in the catalog.

#### validation_backup.head_run_0502

Validation comparison copy of head_run_0502. Purpose basis: **inferred**.

Columns (12): `symbol`, `quantity`, `average_price`, `daily_unrealized_pnl`, `daily_realized_pnl`, `last_update`, `updated_at`, `strategy_id`, `strategy_name`, `date`, `portfolio_id`, `portfolio_type`.

Primary key: none declared in the catalog.

#### validation_backup.live_equity_curve_20260830

Validation/comparison copy of equity-value time series. Purpose basis: **inferred**.

Columns (6): `id`, `strategy_id`, `timestamp`, `equity`, `portfolio_id`, `portfolio_type`.

Primary key: none declared in the catalog.

#### validation_backup.live_equity_curve_pre_e1verify

Validation/comparison copy of equity-value time series. Purpose basis: **inferred**.

Columns (6): `id`, `strategy_id`, `timestamp`, `equity`, `portfolio_id`, `portfolio_type`.

Primary key: none declared in the catalog.

#### validation_backup.live_executions_20260830

Validation/comparison copy of execution/fill records. Purpose basis: **inferred**.

Columns (17): `exec_id`, `order_id`, `symbol`, `side`, `quantity`, `price`, `execution_time`, `commissions_fees`, `is_partial`, `created_at`, `strategy_id`, `strategy_name`, `date`, `portfolio_id`, `implicit_price_impact`, `slippage_market_impact`, `total_transaction_costs`.

Primary key: none declared in the catalog.

#### validation_backup.live_executions_pre_e1verify

Validation/comparison copy of execution/fill records. Purpose basis: **inferred**.

Columns (17): `exec_id`, `order_id`, `symbol`, `side`, `quantity`, `price`, `execution_time`, `commissions_fees`, `is_partial`, `created_at`, `strategy_id`, `strategy_name`, `date`, `portfolio_id`, `implicit_price_impact`, `slippage_market_impact`, `total_transaction_costs`.

Primary key: none declared in the catalog.

#### validation_backup.live_positions_20260830

Validation/comparison copy of position snapshots. Purpose basis: **inferred**.

Columns (12): `symbol`, `quantity`, `average_price`, `daily_unrealized_pnl`, `daily_realized_pnl`, `last_update`, `updated_at`, `strategy_id`, `strategy_name`, `date`, `portfolio_id`, `portfolio_type`.

Primary key: none declared in the catalog.

#### validation_backup.live_positions_pre_e1verify

Validation/comparison copy of position snapshots. Purpose basis: **inferred**.

Columns (12): `symbol`, `quantity`, `average_price`, `daily_unrealized_pnl`, `daily_realized_pnl`, `last_update`, `updated_at`, `strategy_id`, `strategy_name`, `date`, `portfolio_id`, `portfolio_type`.

Primary key: none declared in the catalog.

#### validation_backup.live_results_20260830

Validation/comparison copy of performance/result metrics. Purpose basis: **inferred**.

Columns (49): `id`, `strategy_id`, `date`, `total_annualized_return`, `volatility`, `total_pnl`, `total_unrealized_pnl`, `total_realized_pnl`, `current_portfolio_value`, `portfolio_var`, `gross_leverage`, `net_leverage`, `portfolio_leverage`, `max_correlation`, `jump_risk`, `risk_scale`, `active_positions`, `config`, `created_at`, `daily_pnl`, `total_transaction_costs`, `daily_realized_pnl`, `daily_unrealized_pnl`, `gross_notional`, `net_notional`, `daily_transaction_costs`, `daily_return`, `equity_to_margin_ratio`, `margin_cushion`, `margin_posted`, `cash_available`, `total_cumulative_return`, `portfolio_id`, `win_rate`, `avg_win`, `avg_loss`, `profit_factor`, `best_day`, `worst_day`, `downside_deviation`, `winning_days`, `losing_days`, `total_days`, `gross_profit`, `gross_loss`, `sharpe_ratio`, `sortino_ratio`, `max_drawdown`, `total_dividend_income`.

Primary key: none declared in the catalog.

#### validation_backup.live_results_pre_e1verify

Validation/comparison copy of performance/result metrics. Purpose basis: **inferred**.

Columns (49): `id`, `strategy_id`, `date`, `total_annualized_return`, `volatility`, `total_pnl`, `total_unrealized_pnl`, `total_realized_pnl`, `current_portfolio_value`, `portfolio_var`, `gross_leverage`, `net_leverage`, `portfolio_leverage`, `max_correlation`, `jump_risk`, `risk_scale`, `active_positions`, `config`, `created_at`, `daily_pnl`, `total_transaction_costs`, `daily_realized_pnl`, `daily_unrealized_pnl`, `gross_notional`, `net_notional`, `daily_transaction_costs`, `daily_return`, `equity_to_margin_ratio`, `margin_cushion`, `margin_posted`, `cash_available`, `total_cumulative_return`, `portfolio_id`, `win_rate`, `avg_win`, `avg_loss`, `profit_factor`, `best_day`, `worst_day`, `downside_deviation`, `winning_days`, `losing_days`, `total_days`, `gross_profit`, `gross_loss`, `sharpe_ratio`, `sortino_ratio`, `max_drawdown`, `total_dividend_income`.

Primary key: none declared in the catalog.

#### validation_backup.live_signals_20260830

Validation/comparison copy of strategy signal records. Purpose basis: **inferred**.

Columns (8): `id`, `strategy_id`, `symbol`, `signal_value`, `timestamp`, `created_at`, `portfolio_id`, `strategy_name`.

Primary key: none declared in the catalog.

#### validation_backup.live_signals_pre_e1verify

Validation/comparison copy of strategy signal records. Purpose basis: **inferred**.

Columns (8): `id`, `strategy_id`, `symbol`, `signal_value`, `timestamp`, `created_at`, `portfolio_id`, `strategy_name`.

Primary key: none declared in the catalog.

#### validation_backup.toggle_live_pos_0424

Validation/comparison copy of position snapshots. Purpose basis: **inferred**.

Columns (12): `symbol`, `quantity`, `average_price`, `daily_unrealized_pnl`, `daily_realized_pnl`, `last_update`, `updated_at`, `strategy_id`, `strategy_name`, `date`, `portfolio_id`, `portfolio_type`.

Primary key: none declared in the catalog.

#### validation_backup.toggle_live_res_0424

Validation/comparison copy of performance/result metrics. Purpose basis: **inferred**.

Columns (49): `id`, `strategy_id`, `date`, `total_annualized_return`, `volatility`, `total_pnl`, `total_unrealized_pnl`, `total_realized_pnl`, `current_portfolio_value`, `portfolio_var`, `gross_leverage`, `net_leverage`, `portfolio_leverage`, `max_correlation`, `jump_risk`, `risk_scale`, `active_positions`, `config`, `created_at`, `daily_pnl`, `total_transaction_costs`, `daily_realized_pnl`, `daily_unrealized_pnl`, `gross_notional`, `net_notional`, `daily_transaction_costs`, `daily_return`, `equity_to_margin_ratio`, `margin_cushion`, `margin_posted`, `cash_available`, `total_cumulative_return`, `portfolio_id`, `win_rate`, `avg_win`, `avg_loss`, `profit_factor`, `best_day`, `worst_day`, `downside_deviation`, `winning_days`, `losing_days`, `total_days`, `gross_profit`, `gross_loss`, `sharpe_ratio`, `sortino_ratio`, `max_drawdown`, `total_dividend_income`.

Primary key: none declared in the catalog.

#### validation_backup.togglebin_live_positions

Validation/comparison copy of position snapshots. Purpose basis: **inferred**.

Columns (12): `symbol`, `quantity`, `average_price`, `daily_unrealized_pnl`, `daily_realized_pnl`, `last_update`, `updated_at`, `strategy_id`, `strategy_name`, `date`, `portfolio_id`, `portfolio_type`.

Primary key: none declared in the catalog.

#### validation_backup.togglebin_live_results

Validation/comparison copy of performance/result metrics. Purpose basis: **inferred**.

Columns (49): `id`, `strategy_id`, `date`, `total_annualized_return`, `volatility`, `total_pnl`, `total_unrealized_pnl`, `total_realized_pnl`, `current_portfolio_value`, `portfolio_var`, `gross_leverage`, `net_leverage`, `portfolio_leverage`, `max_correlation`, `jump_risk`, `risk_scale`, `active_positions`, `config`, `created_at`, `daily_pnl`, `total_transaction_costs`, `daily_realized_pnl`, `daily_unrealized_pnl`, `gross_notional`, `net_notional`, `daily_transaction_costs`, `daily_return`, `equity_to_margin_ratio`, `margin_cushion`, `margin_posted`, `cash_available`, `total_cumulative_return`, `portfolio_id`, `win_rate`, `avg_win`, `avg_loss`, `profit_factor`, `best_day`, `worst_day`, `downside_deviation`, `winning_days`, `losing_days`, `total_days`, `gross_profit`, `gross_loss`, `sharpe_ratio`, `sortino_ratio`, `max_drawdown`, `total_dividend_income`.

Primary key: none declared in the catalog.

## 6. The three application views

Views are separate from the 106 stored tables. Their outputs may contain personal information; no view rows were selected.

### metadata.symbols

Contract metadata convenience view. Purpose basis: **source-backed**.

Columns (4): `Databento Symbol`, `IB Symbol`, `Name`, `Contract Months`.

### people.applicant_history

Applicant history summary view. Purpose basis: **inferred**.

Columns (10): `person_id`, `first_name`, `last_name`, `email`, `times_applied`, `cycles`, `first_applied_at`, `last_applied_at`, `latest_outcome`, `is_current_member`.

### people.roster_public

Public roster view. Purpose basis: **inferred**.

Columns (7): `id`, `first_name`, `last_name`, `team`, `is_leadership`, `joined_on`, `active`.

## 7. Safety, evidence, and remaining limits

- Connections used `default_transaction_read_only=on` at startup and explicit `BEGIN READ ONLY`, verified the approved database/user fingerprints and read-only settings, then ended with `ROLLBACK`.
- Queries were catalog reads and bounded reporting-table aggregates. No `INSERT`, `UPDATE`, `DELETE`, DDL, migration, `ANALYZE`, application-write endpoint, scheduler, or mailer was run against production.
- Statement timeout was 30 seconds and lock timeout was 5 seconds. Ordinary read queries still consume database resources and acquire ordinary read locks; read-only does not mean zero operational load.
- Local ignored evidence: `.cache/prod-table-inventory.json`, `.cache/prod-reporting-table-counts.json`, `.cache/table-purpose-map.json`; query helpers: `.cache/prod_table_inventory.sh`, `.cache/prod_reporting_table_counts.sh`. Connection secrets were not copied into this report.
- This is a catalog/coverage/readiness audit, not an exhaustive correctness audit of every table's contents, permissions, retention, referential consistency, or every downstream pipeline.
- No production data/schema changes, deployment changes, container/service restarts, or investor emails were made.

Related preparation documents:

- [Migration 012 production rollout plan](../superpowers/plans/2026-09-21-migration-012-production-rollout.md)
- [QT pipeline revalidation](2026-09-21-qt-pipeline-revalidation.md)

**Conclusion:** The 21 recorded tests provide targeted migration/rollback evidence. The live inventory establishes what tables/columns exist and selected exact counts. Neither is sufficient to claim the investor-email workflow is production-ready; the configured API deployment mismatch, database identity/freshness, missing QT/risk data, and approved coordinated rollout still require resolution under the existing read-only boundary.
