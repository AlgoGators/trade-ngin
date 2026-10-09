-- 027_qt_daily_cutoff_rollback.sql
--
-- Reverses 027: svc_algolens gets its table-wide INSERT and UPDATE on trading.live_run_metadata
-- back (in place of the column grants 027 made), and the CHECK and the columns publish_source
-- and sent_at are dropped. svc_trade_ngin keeps the table grants 025 gave it. Refuses while any
-- row carries publish_source or sent_at, unless the session sets migration.force_rollback =
-- 'yes': the record of who published each day and when its e-mail went out is then lost, and a
-- service that still runs the 027 engine would e-mail an already sent day again. Roll the image
-- back first.

BEGIN;

DO $$
DECLARE
    n bigint := 0;
BEGIN
    IF to_regclass('trading.live_run_metadata') IS NULL THEN
        RETURN;
    END IF;
    IF (SELECT count(*) FROM information_schema.columns WHERE table_schema = 'trading'
          AND table_name = 'live_run_metadata'
          AND column_name IN ('publish_source', 'sent_at')) = 2 THEN
        EXECUTE 'SELECT count(*) FROM trading.live_run_metadata WHERE publish_source IS NOT NULL '
                'OR sent_at IS NOT NULL' INTO n;
    END IF;
    IF n > 0 AND coalesce(current_setting('migration.force_rollback', true), '') <> 'yes' THEN
        RAISE EXCEPTION '027 rollback refused: % live_run_metadata row(s) carry publish_source or sent_at', n;
    END IF;
END $$;

DO $$
DECLARE
    cols text;
    priv text;
BEGIN
    IF to_regclass('trading.live_run_metadata') IS NULL
       OR NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'svc_algolens') THEN
        RETURN;
    END IF;
    SELECT string_agg(quote_ident(column_name), ', ' ORDER BY ordinal_position) INTO cols
    FROM information_schema.columns
    WHERE table_schema = 'trading' AND table_name = 'live_run_metadata';
    FOREACH priv IN ARRAY ARRAY['INSERT', 'UPDATE'] LOOP
        -- 027 turned a table-wide grant into column grants: turn them back.
        IF NOT has_table_privilege('svc_algolens', 'trading.live_run_metadata', priv)
           AND has_any_column_privilege('svc_algolens', 'trading.live_run_metadata', priv) THEN
            EXECUTE format('GRANT %s ON trading.live_run_metadata TO svc_algolens', priv);
            EXECUTE format('REVOKE %s (%s) ON trading.live_run_metadata FROM svc_algolens', priv, cols);
        END IF;
    END LOOP;
END $$;

ALTER TABLE IF EXISTS trading.live_run_metadata
    DROP CONSTRAINT IF EXISTS live_run_metadata_publish_source_check;
ALTER TABLE IF EXISTS trading.live_run_metadata DROP COLUMN IF EXISTS publish_source;
ALTER TABLE IF EXISTS trading.live_run_metadata DROP COLUMN IF EXISTS sent_at;

COMMIT;
