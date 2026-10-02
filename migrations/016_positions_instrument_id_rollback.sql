-- 016_positions_instrument_id_rollback.sql
--
-- Drops trading.positions.instrument_id and backtest.final_positions.instrument_id (016). The
-- column carries no number any reader sums; the rows keep every other value. Refuses while any row
-- carries a non-NULL id unless the session sets migration.force_rollback = 'yes' (a value written by
-- the engine from T-ROLLX commit 2 on is history the operator must choose to discard).

BEGIN;

DO $$
DECLARE
    n bigint;
BEGIN
    SELECT (SELECT count(*) FROM trading.positions WHERE instrument_id IS NOT NULL)
         + (SELECT count(*) FROM backtest.final_positions WHERE instrument_id IS NOT NULL) INTO n;
    IF n > 0 AND coalesce(current_setting('migration.force_rollback', true), '') <> 'yes' THEN
        RAISE EXCEPTION '016 rollback refused: % row(s) carry an instrument_id; SET migration.force_rollback = ''yes'' to discard them', n;
    END IF;
END $$;

ALTER TABLE trading.positions DROP COLUMN IF EXISTS instrument_id;
ALTER TABLE backtest.final_positions DROP COLUMN IF EXISTS instrument_id;

COMMIT;
