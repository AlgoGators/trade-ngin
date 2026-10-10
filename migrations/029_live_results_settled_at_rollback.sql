-- 029_live_results_settled_at_rollback.sql
--
-- Drops settled_at from trading.live_results (its comment goes with it). Refuses while any row
-- carries an OBSERVED stamp (a value other than the backfill marker 1970-01-01 00:00:00+00)
-- unless the session sets migration.force_rollback = 'yes': the instants are then lost, and a
-- second apply of 029 marks those rows as backfilled. Rows that hold only the marker or NULL are
-- restored exactly by applying 029 again. No other column, value, key or index is touched.

BEGIN;
DO $$
DECLARE n bigint := 0;
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = 'trading' AND table_name = 'live_results' AND column_name = 'settled_at') THEN
        EXECUTE 'SELECT count(*) FROM trading.live_results WHERE settled_at IS NOT NULL AND settled_at <> TIMESTAMPTZ ''1970-01-01 00:00:00+00''' INTO n;
    END IF;
    IF n > 0 AND coalesce(current_setting('migration.force_rollback', true), '') <> 'yes' THEN
        RAISE EXCEPTION '029 rollback refused: % live_results row(s) carry an observed settled_at', n;
    END IF;
END $$;
ALTER TABLE trading.live_results DROP COLUMN IF EXISTS settled_at;
COMMIT;
