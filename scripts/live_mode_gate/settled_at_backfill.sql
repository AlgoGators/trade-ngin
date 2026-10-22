-- The backfill of migration 035 (live_results_settled_at; applied to new_algo_data as 029), copied verbatim from
-- between its BACKFILL markers. The live mode gate runs it on a book it restores from a snapshot, scoped to that
-- portfolio by the session setting migration.settled_at_backfill = 'portfolio:<id>', so the restored rows are in the
-- state the migration leaves them. The migration is applied and never changes, so this copy cannot drift from it.
UPDATE trading.live_results r
   SET settled_at = TIMESTAMPTZ '1970-01-01 00:00:00+00'
 WHERE r.settled_at IS NULL
   AND (current_setting('migration.settled_at_backfill', true) = 'all'
        OR 'portfolio:' || r.portfolio_id = current_setting('migration.settled_at_backfill', true))
   AND EXISTS (SELECT 1 FROM trading.live_results l
                WHERE l.portfolio_id IS NOT DISTINCT FROM r.portfolio_id
                  AND l.strategy_id = r.strategy_id
                  AND l.date > r.date);
