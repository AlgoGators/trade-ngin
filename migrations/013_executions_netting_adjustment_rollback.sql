-- 013_executions_netting_adjustment_rollback.sql
--
-- Rollback for 013. Drops trading.executions.netting_adjustment and
-- backtest.executions.netting_adjustment and restores the equity_to_margin_ratio comment the
-- database carried before 013.
--
-- READ THIS BEFORE RUNNING IT.
--
--   1. Roll back the CODE first. A binary built with 013 names netting_adjustment in every
--      executions INSERT (live and backtest) and fails on a database without the column.
--   2. Dropping the column destroys the adjustments. They can be recomputed only with each
--      day's cost-model state (the fill day's volume and volatility feed), which is not stored.
--      So this file REFUSES while any row carries a non-zero value, with a count per table.
--      Dump those rows first, then, in the same psql session:
--          SET migration.allow_netting_drop = 'yes';
--          \i 013_executions_netting_adjustment_rollback.sql
--   3. DROP COLUMN is metadata-only (no rewrite); every other column keeps its values.
--
-- The comment restored on trading.live_results.equity_to_margin_ratio is the text the
-- stage-3 scratch (a copy of production) carried before 013, verbatim.

BEGIN;

DO $$
DECLARE
    n_tr bigint := 0;
    n_bt bigint := 0;
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.columns
                WHERE table_schema = 'trading' AND table_name = 'executions'
                  AND column_name = 'netting_adjustment') THEN
        SELECT count(*) INTO n_tr FROM trading.executions
         WHERE netting_adjustment IS NOT NULL AND netting_adjustment <> 0;
    END IF;
    IF EXISTS (SELECT 1 FROM information_schema.columns
                WHERE table_schema = 'backtest' AND table_name = 'executions'
                  AND column_name = 'netting_adjustment') THEN
        SELECT count(*) INTO n_bt FROM backtest.executions
         WHERE netting_adjustment IS NOT NULL AND netting_adjustment <> 0;
    END IF;
    IF n_tr + n_bt > 0
       AND coalesce(current_setting('migration.allow_netting_drop', true), '') <> 'yes' THEN
        RAISE EXCEPTION 'Refusing: % trading and % backtest executions row(s) carry a non-zero '
                        'netting_adjustment, which cannot be recomputed. Dump them, then SET '
                        'migration.allow_netting_drop = ''yes'' in this session and re-run.',
                        n_tr, n_bt;
    END IF;
END $$;

ALTER TABLE trading.executions  DROP COLUMN IF EXISTS netting_adjustment;
ALTER TABLE backtest.executions DROP COLUMN IF EXISTS netting_adjustment;

COMMENT ON COLUMN trading.live_results.equity_to_margin_ratio IS
    'Gross notional divided by total margin posted (implied margin leverage).';

COMMIT;

-- Verify:
--   SELECT count(*) FROM information_schema.columns
--    WHERE table_name = 'executions' AND column_name = 'netting_adjustment';   -- expect 0
--   SELECT col_description('trading.live_results'::regclass,
--          (SELECT ordinal_position FROM information_schema.columns WHERE table_schema='trading'
--            AND table_name='live_results' AND column_name='equity_to_margin_ratio')::int);
--   -- expect the gross-notional text above
