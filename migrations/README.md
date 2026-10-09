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
| 001 | `001_add_portfolio_type.sql` | `portfolio_type` (`system`/`qt`) on `trading.positions` and `trading.equity_curve`, and in their keys | `test_001_migration.sh` | | |
| 002 | `002_corp_action_applied.sql` | `trading.corp_action_applied`: durable dedup of corporate actions applied to live positions | | | |
| 003 | `003_equity_query_indexes.sql` | Indexes for the equity runner's read paths | | | |
| 004 | `004_get_trading_days_portfolio_scope.sql` | `trading.get_trading_days()` under version control, with a portfolio-scoped overload | | | |
| 005 | `005_corp_action_applied_run_date.sql` | Nullable `run_date` on `trading.corp_action_applied` | | | |
| 006 | `006_corp_action_applied_basis_ratio.sql` | Nullable `basis_ratio` on `trading.corp_action_applied` | | | |
| 012 | `012_strategy_id_width.sql` | Widen `strategy_id` on positions, live_results and signals to varchar(100) | `test_012_strategy_id_width.sh` | | |
| 013 | `013_executions_netting_adjustment.sql` | `netting_adjustment` on `trading.executions` and `backtest.executions` | `test_013_executions_netting_adjustment.sh` | | |
| 014 | `014_contract_metadata_fee_per_contract.sql` | `"Fee Per Contract"` on `metadata.contract_metadata` | `test_014_contract_metadata_fee_per_contract.sh` | | |
| 015 | `015_executions_execution_type.sql` | `execution_type` (STRATEGY/ROLL/BORROW) and `instrument_id` on both executions tables | `test_015_017_018_roll_columns.sh` | | |
| 016 | `016_positions_instrument_id.sql` | `instrument_id` on `trading.positions` and `backtest.final_positions` | `test_016_positions_instrument_id.sh` | | |
| 017 | `017_live_results_roll_costs.sql` | `daily_roll_costs` and `total_roll_costs` on `trading.live_results` | `test_015_017_018_roll_columns.sh` | | |
| 018 | `018_backtest_results_costs.sql` | `transaction_costs`, `roll_costs`, `total_roll_fills` on `backtest.results` | `test_015_017_018_roll_columns.sh` | | |
| 019 | `019_contract_metadata_fixes.sql` | Data fix of nine contract metadata rows | `test_019_contract_metadata_fixes.sh` | | |
| 020 | `020_risk_detail.sql` | `risk_detail` jsonb on `trading.live_results` and `backtest.equity_curve` | `test_020_risk_detail.sh` | | |
| 021 | `021_qt_books.sql` | Three books: `qt_proposal` allowed in positions and equity_curve; `portfolio_type` on executions and live_results and in their keys; `positions.moved_by` | `test_021_qt_books.sh` | | |
| 022 | `022_strategy_config.sql` | `trading.strategy_config` (desk settings, versioned, one active row per portfolio); `settings_used`, `published_by`, `published_at` on `trading.live_run_metadata` | | applied | |
| 023 | `023_qt_command_log.sql` | Rebuild `trading.position_overrides` as the QT command log; `live_results.book_source`; `strategy_registry.portfolio_group`/`desk_editable` | | applied | |
| 024 | `024_drop_retired_qt_tables.sql` | Ruling-28 drops (risk_limits, portfolios, strategy_book_memberships, portfolio_assignments) | | not applied | |

## Deploy notes

- **001** and **021** change keys the engine names, so each ships with the binary of its commit.
  The binary from 021 on names `portfolio_type` in every read, write and delete of positions,
  executions, live_results and equity_curve, and fails against a database without 021.
- A rollback refuses while it would lose rows a later book wrote; read its header first.
