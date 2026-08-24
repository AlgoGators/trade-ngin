-- Rollback for 006_run_inputs_and_rebench_stream.sql
--
-- Reverses the schema changes:
--   * Drops the run_inputs and benchmark_replays tables
--   * Narrows the portfolio_type CHECK constraints back to their 005 state
--
-- Idempotent and transactional.

BEGIN;

-- Drop the new tables (idempotent via IF EXISTS).
DROP TABLE IF EXISTS trading.benchmark_replays;
DROP TABLE IF EXISTS trading.run_inputs;

-- Narrow portfolio_type back to the pre-006 set. This is safe because we are
-- rolling back, so no 'benchmark_rebench' rows should exist to become invalid.
ALTER TABLE trading.positions
    DROP CONSTRAINT IF EXISTS positions_portfolio_type_check;
ALTER TABLE trading.positions
    ADD CONSTRAINT positions_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt', 'benchmark'));

ALTER TABLE trading.equity_curve
    DROP CONSTRAINT IF EXISTS equity_curve_portfolio_type_check;
ALTER TABLE trading.equity_curve
    ADD CONSTRAINT equity_curve_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt', 'benchmark'));

COMMIT;
