# Trading schema migration order

Apply these migrations in numeric order before deploying a binary that uses the
dual-portfolio workflow:

1. `001_add_portfolio_type.sql` — already merged on `main`; adds the `system`
   and `qt` discriminator and rebuilds affected keys.
2. `002_backfill_qt_from_system.sql` — seeds historical `qt` rows so the first
   upgraded run does not treat the live book as flat.
3. `003_add_benchmark_stream.sql` — enables the independent counterfactual
   stream.
4. `004_position_overrides_audit.sql` — creates the append-only manual-change
   audit log.
5. `005_risk_limits.sql` — delivered by PR #56; publishes the limits consumed
   by the QT operator validation gate.
6. `006_run_inputs_and_rebench_stream.sql` — records replay inputs and the
   research re-benchmark stream.
7. `007_benchmark_frozen_shadow_stream.sql` — records frozen-engine parity
   results without mixing them into production attribution.
8. `008_strategy_config.sql` — delivered by PR #60; stores versioned strategy
   overrides and run manifests without colliding with the portfolio migrations.
9. `009_books_and_membership.sql` — delivered on the `qt-platform-preview`
   branch; declares books as first-class rows, lets a strategy belong to
   several, and adds the append-only book-change audit. Numbered 009 because
   PR #60 already holds 008 on its own branch; both are meant for `main`.
10. `010_clear_profit_factor_sentinel.sql` — delivered on the `qt-platform-preview`
   branch; clears the 999.99 placeholder the engine used to write for a book
   with no losing days. Apply it together with the binary that stops writing it;
   applying it early is harmless, because the next run would put the sentinel
   back and a later re-run of the migration clears it again.
11. `011_live_results_and_executions_columns.sql` — delivered on the
   `qt-platform-preview` branch; declares, with `ADD COLUMN IF NOT EXISTS`,
   every column the live runner writes to `trading.live_results` and the
   execution writer writes to `trading.executions`. Nothing in this directory
   had ever created those two tables or added a column to either: their shape
   existed only as whatever was done by hand on the box running the engine.
   That became a deployment blocker when AlgoLens started reading ten published
   metrics out of `live_results` rather than recomputing them. Additive and
   idempotent — a no-op on a database that already has them, which a working
   production box does. Apply it BEFORE 010, which UPDATEs `profit_factor`.

The safe release sequence is: merge PR #55 and PR #56; apply 002–004 from PR
#55, then 005 from PR #56, then 006–007 from PR #55; deploy the combined binary
only after the schema is current. Apply 008 from PR #60 before enabling
database-backed strategy overrides, and 009 before deploying an AlgoLens that
has the Books tab (it no longer creates these tables itself). Apply 011 before 010 and before deploying an
AlgoLens that reads the engine's published metrics; apply 010 with the binary
that stops writing the profit-factor sentinel.

Building a database from this directory and checking it against AlgoLens's
schema contract (`algolens-api/scripts/check_schema.py`) reported fourteen
mismatches before 011 and reports two after: `futures_data.ohlcv_1d` and
`metadata.contract_metadata`, which belong to data-ngin and are correctly not
created here. That check is worth running against production before any
deploy. Run each `test_*.sh` migration test against
disposable PostgreSQL before production rollout. Rollback scripts intentionally
refuse operations that would discard populated attribution streams.

Manual position writes must target only `portfolio_type = 'qt'`, must create a
`trading.position_overrides` row in the same transaction, and must never mutate
the `system` or benchmark streams.
