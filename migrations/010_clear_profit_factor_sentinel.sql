-- Clear the 999.99 profit-factor sentinel from historical rows.
--
-- WHAT THE SENTINEL WAS
-- ---------------------
-- Profit factor is gross profit over gross loss. A book that has not yet had a
-- losing day has no gross loss, so the ratio has no denominator and no value.
-- The engine wrote 999.99 for that case (999.0 on the backtest path), described
-- in the source as a "convention: very large profit factor if there are no
-- losses".
--
-- It is not a large profit factor. It is a placeholder for an absent one, and
-- once it is sitting in a numeric column nothing downstream can tell the two
-- apart. It averages into a fund-level figure. It sorts to the top of any
-- leaderboard. It renders on a dashboard as "999.99x", which is the number a
-- reader will remember. AlgoLens now treats it as undefined on read, but every
-- other consumer of trading.live_results -- a notebook, a report, a query
-- someone writes next month -- inherits the confusion.
--
-- The engine stopped writing it in the same change that adds this file
-- (src/live/live_historical_metrics.cpp, src/live/live_metrics_calculator.cpp,
-- src/backtest/backtest_metrics_calculator.cpp). New rows leave the column NULL
-- when the ratio is undefined. This clears the rows already written.
--
--
-- WHY THIS IS SAFE, AND WHY IT IS REVERSIBLE
-- ------------------------------------------
-- gross_profit and gross_loss are stored in the same row. The sentinel is
-- therefore not carrying any information that is not still present: it is
-- exactly the rows where gross_loss = 0, and the rollback reconstructs it from
-- that condition. Nothing is lost either way.
--
-- The predicate is deliberately narrow. It clears a value at or above 999 only
-- where gross_loss is zero or absent -- that is, only where the sentinel is the
-- only thing it could be. A genuine profit factor of 999 would require a book
-- that made a thousand dollars for every one it lost, and this leaves such a
-- row alone if gross_loss says it is real.
--
-- Idempotent and transactional. Running it twice changes nothing the second
-- time.

BEGIN;

UPDATE trading.live_results
   SET profit_factor = NULL
 WHERE profit_factor >= 999.0
   AND coalesce(gross_loss, 0) = 0;

-- The backtest path wrote 999.0 into whichever results table it was pointed at.
-- trading.backtest_results is the usual one and may not exist in every
-- deployment, so this is guarded rather than assumed.
DO $$
BEGIN
    IF EXISTS (
        SELECT 1 FROM information_schema.columns
         WHERE table_schema = 'trading'
           AND table_name = 'backtest_results'
           AND column_name = 'profit_factor'
    ) THEN
        EXECUTE 'UPDATE trading.backtest_results
                    SET profit_factor = NULL
                  WHERE profit_factor >= 999.0';
    END IF;
END
$$;

COMMIT;
