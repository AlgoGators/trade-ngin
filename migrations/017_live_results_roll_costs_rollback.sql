-- 017_live_results_roll_costs_rollback.sql: drops the two columns. Refuses while any non-zero value exists unless
-- migration.force_rollback = 'yes'.
BEGIN;
DO $$
DECLARE n bigint;
BEGIN
    SELECT count(*) INTO n FROM trading.live_results WHERE daily_roll_costs <> 0 OR total_roll_costs <> 0;
    IF n > 0 AND coalesce(current_setting('migration.force_rollback', true), '') <> 'yes' THEN
        RAISE EXCEPTION '017 rollback refused: % row(s) carry a roll cost', n;
    END IF;
END $$;
ALTER TABLE trading.live_results DROP COLUMN IF EXISTS daily_roll_costs;
ALTER TABLE trading.live_results DROP COLUMN IF EXISTS total_roll_costs;
COMMIT;
