-- 033_backtest_end_date_comment_rollback.sql
--
-- Restores, byte for byte, the comments migration 028 set on backtest.results.end_date and
-- backtest.run_metadata.end_date. COMMENT ON only: no value moves. Transactional and idempotent.

BEGIN;
DO $$
DECLARE o text; p text[];
BEGIN
    FOREACH o IN ARRAY ARRAY[
        'backtest.results.end_date',
        'backtest.run_metadata.end_date'
    ] LOOP
        p := string_to_array(o, '.');
        IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                       WHERE table_schema = p[1] AND table_name = p[2] AND column_name = p[3]) THEN
            RAISE EXCEPTION 'rollback of migration 033: column % is missing; nothing was changed', o;
        END IF;
    END LOOP;
END $$;
COMMENT ON COLUMN backtest.results."end_date" IS $c028$The end of the configured window of the run, stored as the host-local midnight of the configured date.$c028$;
COMMENT ON COLUMN backtest.run_metadata."end_date" IS $c028$The end of the configured window of the run (a date).$c028$;
COMMIT;
