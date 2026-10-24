-- 040_roll_split_rollback.sql: drops the five roll split columns and their two CHECKs. Refuses while any
-- cell holds a value unless migration.force_rollback = 'yes' (the values are lost: a switch-on run would
-- have to be re-run to write them again). Idempotent.
BEGIN;
DO $$
DECLARE n bigint := 0; m bigint;
BEGIN
    IF (SELECT count(*) FROM information_schema.columns WHERE table_schema = 'trading' AND table_name = 'executions'
          AND column_name = 'roll_bar_move') = 1 THEN
        EXECUTE 'SELECT count(*) FROM trading.executions WHERE roll_bar_move IS NOT NULL OR roll_gap_points IS NOT NULL
                   OR roll_gap_value IS NOT NULL' INTO m;
        n := n + m;
    END IF;
    IF (SELECT count(*) FROM information_schema.columns WHERE table_schema = 'backtest' AND table_name = 'executions'
          AND column_name = 'roll_bar_move') = 1 THEN
        EXECUTE 'SELECT count(*) FROM backtest.executions WHERE roll_bar_move IS NOT NULL OR roll_gap_points IS NOT NULL
                   OR roll_gap_value IS NOT NULL' INTO m;
        n := n + m;
    END IF;
    IF (SELECT count(*) FROM information_schema.columns WHERE table_schema = 'backtest' AND table_name = 'results'
          AND column_name = 'roll_bar_moves') = 1 THEN
        EXECUTE 'SELECT count(*) FROM backtest.results WHERE roll_bar_moves IS NOT NULL OR roll_gap_values IS NOT NULL' INTO m;
        n := n + m;
    END IF;
    IF n > 0 AND coalesce(current_setting('migration.force_rollback', true), '') <> 'yes' THEN
        RAISE EXCEPTION '040 rollback refused: % row(s) carry a roll split value', n;
    END IF;
END $$;
ALTER TABLE trading.executions DROP CONSTRAINT IF EXISTS chk_executions_roll_split;
ALTER TABLE backtest.executions DROP CONSTRAINT IF EXISTS chk_backtest_executions_roll_split;
ALTER TABLE trading.executions DROP COLUMN IF EXISTS roll_bar_move;
ALTER TABLE trading.executions DROP COLUMN IF EXISTS roll_gap_points;
ALTER TABLE trading.executions DROP COLUMN IF EXISTS roll_gap_value;
ALTER TABLE backtest.executions DROP COLUMN IF EXISTS roll_bar_move;
ALTER TABLE backtest.executions DROP COLUMN IF EXISTS roll_gap_points;
ALTER TABLE backtest.executions DROP COLUMN IF EXISTS roll_gap_value;
ALTER TABLE backtest.results DROP COLUMN IF EXISTS roll_bar_moves;
ALTER TABLE backtest.results DROP COLUMN IF EXISTS roll_gap_values;
COMMIT;
