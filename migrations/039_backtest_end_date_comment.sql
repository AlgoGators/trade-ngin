-- 039_backtest_end_date_comment.sql
-- Numbered 033 until 2026-10-21; renumbered 039 when main-nonQT joined main (main has its own 028 and 029).
-- The migration was applied to new_algo_data as 033 on 2026-10-13: do not re-apply it there. The text below is
-- exactly as applied, so its comments and stored COMMENT texts still call it 033.
--
-- COMMENT ON only: the two end_date columns of a backtest run. Migration 028 said each holds the
-- end of the configured window. Since the run stores the data boundary instead (the last bar it
-- consumed), both texts were false:
--   backtest.results.end_date       a timestamp: the stamp of the run's last equity-curve row
--   backtest.run_metadata.end_date  a date: the date of that same stamp
-- start_date keeps 028's comment on both tables: it is still the start of the configured window.
--
-- SAFETY: comments only, no value and no column moves; refuses, changing nothing, when either
-- column is missing; transactional and idempotent.

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
            RAISE EXCEPTION 'migration 033: column % is missing; nothing was changed', o;
        END IF;
    END LOOP;
END $$;
COMMENT ON COLUMN backtest.results."end_date" IS $c033$The last bar the run consumed, not the end of the configured window: the latest timestamp of the run's backtest.equity_curve rows, with the same stamp as that row. The stamp is the engine's reading of the bar's time and depends on the host's time zone: on a UTC host it is the bar's own instant. A run that consumed no bar stores start_date. Rows written before migration 033 hold the end of the configured window, stored as the host-local midnight of the configured date.$c033$;
COMMENT ON COLUMN backtest.run_metadata."end_date" IS $c033$The date of the last bar the run consumed (the date of backtest.results.end_date of the same run), not the end of the configured window. A run that consumed no bar stores start_date. Rows written before migration 033 hold the end of the configured window.$c033$;
COMMIT;
