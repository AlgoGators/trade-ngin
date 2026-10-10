-- 029_live_results_settled_at.sql
--
-- Add settled_at (timestamptz, NULL) to trading.live_results: how a reader knows a row is settled
-- (T-8D-2 R43 and section 5.6; LOOP_SPEC v6.2 sections 3.1 and 7.2; T-8a commit (12)). The run of
-- a date writes its row with settled_at NULL; the next run, once its finalize of the previous day
-- finished without error, stamps now() on every earlier row of its book that is still NULL.
--
-- BACKFILL (R43 as written; LOOP_SPEC section 15 erratum 1): when this migration adds the column,
-- every existing row that has a later row of its key (portfolio_id, strategy_id; the table's
-- UNIQUE key is (portfolio_id, strategy_id, date)) is stamped with the fixed marker
-- 1970-01-01 00:00:00+00: backfilled at migration, settlement not observed. A row an earlier
-- no-prices day or finalize warning left unfinalised is stamped too and counts as settled from
-- then on. The last row of each key stays NULL: the next run settles it. portfolio_id is matched
-- with IS NOT DISTINCT FROM, so the rows stored with a NULL portfolio_id form one book per
-- strategy_id.
--
-- The backfill statement runs only where the session setting migration.settled_at_backfill says:
-- 'all' (set below for this transaction, and only when the column is being added, so a second
-- apply stamps nothing), or 'portfolio:<portfolio_id>' (one book: a harness that restores a book
-- from a pre-029 snapshot sets it and runs the statement between the two marker lines). With the
-- setting absent the statement touches no row.
--
-- SAFETY: ADD COLUMN of a NULL-able column with no default is metadata-only; no other column, key,
-- index or view is touched; type-guarded; transactional and idempotent.

BEGIN;
DO $$
BEGIN
    IF to_regclass('trading.live_results') IS NULL THEN RAISE EXCEPTION 'trading.live_results does not exist'; END IF;
    IF EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = 'trading' AND table_name = 'live_results'
               AND column_name = 'settled_at' AND data_type <> 'timestamp with time zone') THEN
        RAISE EXCEPTION 'trading.live_results.settled_at exists with another type';
    END IF;
    PERFORM set_config('migration.settled_at_backfill',
        CASE WHEN EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = 'trading'
                          AND table_name = 'live_results' AND column_name = 'settled_at')
             THEN 'none' ELSE 'all' END, true);
END $$;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS settled_at timestamptz;
-- 029 BACKFILL BEGIN
UPDATE trading.live_results r
   SET settled_at = TIMESTAMPTZ '1970-01-01 00:00:00+00'
 WHERE r.settled_at IS NULL
   AND (current_setting('migration.settled_at_backfill', true) = 'all'
        OR 'portfolio:' || r.portfolio_id = current_setting('migration.settled_at_backfill', true))
   AND EXISTS (SELECT 1 FROM trading.live_results l
                WHERE l.portfolio_id IS NOT DISTINCT FROM r.portfolio_id
                  AND l.strategy_id = r.strategy_id
                  AND l.date > r.date);
-- 029 BACKFILL END
COMMENT ON COLUMN trading.live_results.settled_at IS
    'The instant the row was settled, UTC; NULL until then. The run of a date writes its row with settled_at NULL: its levels are as written (a futures row carries the day''s costs only, an equity row the mark at the previous close). The next run stamps now() on every earlier row of the book (portfolio_id, strategy_id) that is still NULL, in one UPDATE, once its finalize of the previous day (the level rewrite, the equity-curve point and the statistics refresh) finished without error, whether the levels were rewritten or were already final (a flat book, a day with no move). A run with no Day T-1 or Day T-2 closes, or whose finalize warned, stamps nothing, and so does a run that finds no Day T-1 row (the first run after a missed day); the next clean run stamps the rows it left. The stamp is written by the run that finalises the row; a held day on which no bar printed (a Saturday) is counted by the sizing capital from the first run that loads a later bar, which can be one run before its stamp. A re-run of a date deletes and re-inserts its row, so its stamp is cleared and written again by the following run. Readers take the statistics from the newest row with settled_at set and draw trading.equity_curve through that date. THE BACKFILL MARKER: the value 1970-01-01 00:00:00+00 is not an instant. Migration 029 wrote it on every row that then had a later row of its book: backfilled at migration, settlement not observed (it covers a row an earlier no-prices day or finalize warning had left unfinalised; such a row counts as settled). Every other value is the wall-clock instant of the run that settled the row. Written on every book, the equity book included. T-8D-2 R43; LOOP_SPEC v6.2 sections 3.1 and 7.2; migration 029.';
COMMIT;
