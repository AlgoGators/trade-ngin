# Migrations

Apply in the order below. Each `NNN_name.sql` has a `NNN_name_rollback.sql` beside it, and most
have a `test_NNN_*.sh` that runs the migration and its rollback against a throwaway database
(each script says how to call it and refuses production by name).

There is no migrations table (master document ruling 26). **Applied state is tracked by hand in
this file**: when you apply a migration to a database, fill in the date in its column in the
same PR or straight after. A blank cell means "not recorded", not "not applied".

007 to 011 are unused on main: they were taken on unmerged branches (#55, config-from-database,
qt-platform-preview) and are retired, so the numbers are never reused (see the header of 012).

| # | File | What it does | Test | new_algo_data | algo_data |
|---|---|---|---|---|---|
| 001 | `001_add_portfolio_type.sql` | `portfolio_type` (`system`/`qt`) on `trading.positions` and `trading.equity_curve`, and in their keys | `test_001_migration.sh` | before 2026-10-08 | |
| 002 | `002_corp_action_applied.sql` | `trading.corp_action_applied`: durable dedup of corporate actions applied to live positions | | | |
| 003 | `003_equity_query_indexes.sql` | Indexes for the equity runner's read paths | | | |
| 004 | `004_get_trading_days_portfolio_scope.sql` | `trading.get_trading_days()` under version control, with a portfolio-scoped overload | | | |
| 005 | `005_corp_action_applied_run_date.sql` | Nullable `run_date` on `trading.corp_action_applied` | | | |
| 006 | `006_corp_action_applied_basis_ratio.sql` | Nullable `basis_ratio` on `trading.corp_action_applied` | | | |
| 012 | `012_strategy_id_width.sql` | Widen `strategy_id` on positions, live_results and signals to varchar(100) | `test_012_strategy_id_width.sh` | 2026-10-09 | |
| 013 | `013_executions_netting_adjustment.sql` | `netting_adjustment` on `trading.executions` and `backtest.executions` | `test_013_executions_netting_adjustment.sh` | 2026-10-09 | |
| 014 | `014_contract_metadata_fee_per_contract.sql` | `"Fee Per Contract"` on `metadata.contract_metadata` | `test_014_contract_metadata_fee_per_contract.sh` | 2026-10-09 | |
| 015 | `015_executions_execution_type.sql` | `execution_type` (STRATEGY/ROLL/BORROW) and `instrument_id` on both executions tables | `test_015_017_018_roll_columns.sh` | 2026-10-09 | |
| 016 | `016_positions_instrument_id.sql` | `instrument_id` on `trading.positions` and `backtest.final_positions` | `test_016_positions_instrument_id.sh` | 2026-10-09 | |
| 017 | `017_live_results_roll_costs.sql` | `daily_roll_costs` and `total_roll_costs` on `trading.live_results` | `test_015_017_018_roll_columns.sh` | 2026-10-09 | |
| 018 | `018_backtest_results_costs.sql` | `transaction_costs`, `roll_costs`, `total_roll_fills` on `backtest.results` | `test_015_017_018_roll_columns.sh` | 2026-10-09 | |
| 019 | `019_contract_metadata_fixes.sql` | Data fix of nine contract metadata rows | `test_019_contract_metadata_fixes.sh` | 2026-10-09 | |
| 020 | `020_risk_detail.sql` | `risk_detail` jsonb on `trading.live_results` and `backtest.equity_curve` | `test_020_risk_detail.sh` | 2026-10-09 | |
| 021 | `021_qt_books.sql` | Three books: `qt_proposal` allowed in positions and equity_curve; `portfolio_type` on executions and live_results and in their keys; `positions.moved_by` | `test_021_qt_books.sh` | 2026-10-09 | |
| 022 | `022_strategy_config.sql` | `trading.strategy_config` (desk settings: versioned, one active row per portfolio); `settings_used`, `published_by`, `published_at` on `trading.live_run_metadata` | `test_022_strategy_config.sh` | 2026-10-09 | |
| 023 | `023_qt_command_log.sql` | `trading.position_overrides` rebuilt as the desk command log; `live_results.book_source`; `strategy_registry.portfolio_group` and `desk_editable`, and the two QT portfolios | `test_023_qt_command_log.sh` | 2026-10-09 | |
| 024 | `024_drop_retired_qt_tables.sql` | Drops `risk_limits`, `portfolios`, `strategy_book_memberships` and `portfolio_assignments` (ruling 28). Irreversible; no rollback file | `test_023_qt_command_log.sh` | | |
| 025 | `025_qt_command_log_hardening.sql` | QT contract C2 and C4 on `trading.position_overrides`: one live decision per request, one open publish and one open override request per day (unique partial indexes); inserts must be `pending` with the engine columns NULL; status moves pending -> running -> done/refused/failed, running -> pending only under `algogators.recovery = 'on'`; terminal rows final; TRUNCATE refused. Grants: PUBLIC loses everything on `trading.live_run_metadata` and its sequence (explicit grants kept or added first); `svc_algolens` loses UPDATE, DELETE, TRUNCATE on `position_overrides` | `test_025_qt_command_log_hardening.sh` (postgres:16, throwaway server only; CI `rpc.yml`) | | |

## Deploy notes

- **001** and **021** change keys the engine names, so each ships with the binary of its commit.
  The binary from 021 on names `portfolio_type` in every read, write and delete of positions,
  executions, live_results and equity_curve, and fails against a database without 021.
- A rollback refuses while it would lose rows a later book wrote; read its header first.
- **025** ships with the desk service of its commit: the service's recovery sets
  `algogators.recovery = 'on'` to put a stale `running` row back to `pending`, which 025 refuses
  otherwise. An older service only loses that recovery (its startup reset is refused and
  logged); nothing else it does is refused. Apply 025 before or with that image. Grants, on
  new_algo_data as checked 2026-10-09: on `trading.live_run_metadata`, PUBLIC held `arwdDxt`
  (and `rU` on the sequence). After 025, `fund_member`, `quant_dev`, `quant_research_ro`,
  `leadership_ro` and `investor_relations_ro` keep SELECT; `quant_dev_rw`, `quant_trading_rw`,
  `leadership_rw`, `svc_algolens` and (newly explicit) `svc_trade_ngin` keep SELECT, INSERT,
  UPDATE, DELETE and the sequence; `qt_algolens_app` and `qt_engine_app` inherit theirs;
  `svc_airflow` and `svc_data_ngin` have no USAGE on schema `trading` and lose nothing. On
  `trading.position_overrides`, `svc_algolens` keeps SELECT, INSERT and the sequence, and
  `svc_trade_ngin` gets SELECT, INSERT, UPDATE, DELETE explicitly. Today both services connect
  as `postgres`, so the grants take effect when they move to `qt_algolens_app` /
  `qt_engine_app`.
- **Not done, on purpose:** SET NOT NULL on columns that hold NULLs today. The spec left it;
  `live_run_metadata.portfolio_id` is already NOT NULL on new_algo_data (0 NULL rows of 514 on
  2026-10-09), and no NOT NULL was added to `position_overrides`.
