-- 028_ops_data_ready_rollback.sql
--
-- Reverses 028: drops ops.data_ready, and the schema ops when nothing else is in it. Refuses
-- while the table holds rows unless the session sets migration.force_rollback = 'yes' (the
-- readiness history is then lost). Set QT_EVENING_RUN=0 on engine-rpc first: with the table
-- gone the evening scheduler alerts on every pass and the 06:45 backup waits for data.

BEGIN;

DO $$
DECLARE
    n bigint;
BEGIN
    IF to_regclass('ops.data_ready') IS NULL THEN
        RETURN;
    END IF;
    SELECT count(*) INTO n FROM ops.data_ready;
    IF n > 0 AND coalesce(current_setting('migration.force_rollback', true), '') <> 'yes' THEN
        RAISE EXCEPTION 'ops.data_ready holds % row(s); SET migration.force_rollback = ''yes'' to drop them', n;
    END IF;
END $$;

DROP TABLE IF EXISTS ops.data_ready;

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_namespace WHERE nspname = 'ops')
       AND NOT EXISTS (SELECT 1 FROM pg_class c JOIN pg_namespace s ON s.oid = c.relnamespace
                       WHERE s.nspname = 'ops') THEN
        DROP SCHEMA ops;
    END IF;
END $$;

COMMIT;
