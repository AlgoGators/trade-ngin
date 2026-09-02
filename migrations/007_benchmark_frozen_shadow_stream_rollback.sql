-- Rollback for 007_benchmark_frozen_shadow_stream.sql
--
-- Narrows the portfolio_type CHECK constraints back to their 006 state.
-- Idempotent and transactional.

BEGIN;

ALTER TABLE trading.positions
    DROP CONSTRAINT IF EXISTS positions_portfolio_type_check;
ALTER TABLE trading.positions
    ADD CONSTRAINT positions_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt', 'benchmark', 'benchmark_rebench'));

ALTER TABLE trading.equity_curve
    DROP CONSTRAINT IF EXISTS equity_curve_portfolio_type_check;
ALTER TABLE trading.equity_curve
    ADD CONSTRAINT equity_curve_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt', 'benchmark', 'benchmark_rebench'));

COMMIT;
