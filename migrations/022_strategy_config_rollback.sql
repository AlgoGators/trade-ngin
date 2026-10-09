-- 022_strategy_config_rollback.sql
--
-- Drops trading.strategy_config and live_run_metadata's settings_used, published_by and
-- published_at. Refuses while any strategy_config row exists or any live_run_metadata row carries
-- one of the three values, unless the session sets migration.force_rollback = 'yes' (the desk's
-- settings history and the record of the settings each run used are then lost; neither can be
-- recomputed). Every other column of live_run_metadata is untouched.

BEGIN;
DO $$
DECLARE n bigint := 0; m bigint := 0;
BEGIN
    IF to_regclass('trading.strategy_config') IS NOT NULL THEN
        EXECUTE 'SELECT count(*) FROM trading.strategy_config' INTO n;
    END IF;
    IF (SELECT count(*) FROM information_schema.columns WHERE table_schema = 'trading'
          AND table_name = 'live_run_metadata'
          AND column_name IN ('settings_used', 'published_by', 'published_at')) = 3 THEN
        EXECUTE 'SELECT count(*) FROM trading.live_run_metadata WHERE settings_used IS NOT NULL '
                'OR published_by IS NOT NULL OR published_at IS NOT NULL' INTO m;
    END IF;
    IF n + m > 0 AND coalesce(current_setting('migration.force_rollback', true), '') <> 'yes' THEN
        RAISE EXCEPTION '022 rollback refused: % strategy_config row(s) and % live_run_metadata row(s) carry 022 data', n, m;
    END IF;
END $$;
DROP TABLE IF EXISTS trading.strategy_config;
ALTER TABLE IF EXISTS trading.live_run_metadata DROP COLUMN IF EXISTS settings_used;
ALTER TABLE IF EXISTS trading.live_run_metadata DROP COLUMN IF EXISTS published_by;
ALTER TABLE IF EXISTS trading.live_run_metadata DROP COLUMN IF EXISTS published_at;
COMMIT;
