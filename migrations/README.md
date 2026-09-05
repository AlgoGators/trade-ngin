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

The safe release sequence is: merge PR #55 and PR #56; apply 002–004 from PR
#55, then 005 from PR #56, then 006–007 from PR #55; deploy the combined binary
only after the schema is current. Apply 008 from PR #60 before enabling
database-backed strategy overrides, and 009 before deploying an AlgoLens that
has the Books tab (it no longer creates these tables itself). Apply 010 with the binary that
stops writing the profit-factor sentinel. Run each `test_*.sh` migration test against
disposable PostgreSQL before production rollout. Rollback scripts intentionally
refuse operations that would discard populated attribution streams.

Manual position writes must target only `portfolio_type = 'qt'`, must create a
`trading.position_overrides` row in the same transaction, and must never mutate
the `system` or benchmark streams.
