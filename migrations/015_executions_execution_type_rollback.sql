-- 015_executions_execution_type_rollback.sql: drops the two columns and the CHECK on both tables.
-- Refuses while any ROLL or BORROW row exists unless migration.force_rollback = 'yes'.
BEGIN;
DO $$
DECLARE n bigint;
BEGIN
    SELECT (SELECT count(*) FROM trading.executions WHERE execution_type <> 'STRATEGY')
         + (SELECT count(*) FROM backtest.executions WHERE execution_type <> 'STRATEGY') INTO n;
    IF n > 0 AND coalesce(current_setting('migration.force_rollback', true), '') <> 'yes' THEN
        RAISE EXCEPTION '015 rollback refused: % non-STRATEGY row(s); SET migration.force_rollback = ''yes'' to discard the class', n;
    END IF;
END $$;
ALTER TABLE trading.executions DROP CONSTRAINT IF EXISTS chk_executions_execution_type;
ALTER TABLE backtest.executions DROP CONSTRAINT IF EXISTS chk_backtest_executions_execution_type;
ALTER TABLE trading.executions DROP COLUMN IF EXISTS execution_type;
ALTER TABLE trading.executions DROP COLUMN IF EXISTS instrument_id;
ALTER TABLE backtest.executions DROP COLUMN IF EXISTS execution_type;
ALTER TABLE backtest.executions DROP COLUMN IF EXISTS instrument_id;
COMMIT;
