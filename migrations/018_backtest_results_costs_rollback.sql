-- 018_backtest_results_costs_rollback.sql: drops the three columns. Refuses while any non-zero value exists unless
-- migration.force_rollback = 'yes'.
BEGIN;
DO $$
DECLARE n bigint;
BEGIN
    SELECT count(*) INTO n FROM backtest.results WHERE transaction_costs <> 0 OR roll_costs <> 0 OR total_roll_fills <> 0;
    IF n > 0 AND coalesce(current_setting('migration.force_rollback', true), '') <> 'yes' THEN
        RAISE EXCEPTION '018 rollback refused: % row(s) carry a cost total', n;
    END IF;
END $$;
ALTER TABLE backtest.results DROP COLUMN IF EXISTS transaction_costs;
ALTER TABLE backtest.results DROP COLUMN IF EXISTS roll_costs;
ALTER TABLE backtest.results DROP COLUMN IF EXISTS total_roll_fills;
COMMIT;
