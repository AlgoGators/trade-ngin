-- Rollback for 010_clear_profit_factor_sentinel.sql
--
-- Puts the sentinel back where the forward migration removed it. It can do this
-- exactly, because the condition that identified those rows -- a book with gross
-- profit and no gross loss -- is still recorded in the same row.
--
-- This restores 999.99 to rows that also satisfy the condition and were already
-- NULL before 010 ran, which is a set of rows the forward migration did not
-- touch. Those are rows written by a post-fix binary, where NULL is correct and
-- deliberate. Reverting the schema change without also reverting the binary
-- therefore reintroduces the sentinel to rows that never had it -- which is the
-- point of a rollback: it restores the shape the old binary expects.
--
-- Idempotent and transactional.

BEGIN;

DO $$
BEGIN
    IF EXISTS (
        SELECT 1 FROM information_schema.columns
         WHERE table_schema = 'trading'
           AND table_name = 'live_results'
           AND column_name = 'profit_factor'
    ) THEN
        EXECUTE 'UPDATE trading.live_results
                    SET profit_factor = 999.99
                  WHERE profit_factor IS NULL
                    AND coalesce(gross_loss, 0) = 0
                    AND coalesce(gross_profit, 0) > 0';
    END IF;
END
$$;

DO $$
BEGIN
    IF EXISTS (
        SELECT 1 FROM information_schema.columns
         WHERE table_schema = 'trading'
           AND table_name = 'backtest_results'
           AND column_name = 'profit_factor'
    ) THEN
        -- The backtest table does not carry gross_profit / gross_loss, so the
        -- rows 010 cleared cannot be identified again. Nothing is restored here
        -- rather than guessing which NULLs were once 999.0.
        RAISE NOTICE 'trading.backtest_results.profit_factor sentinels are not restored: the row does not record gross profit and loss, so the cleared rows cannot be identified.';
    END IF;
END
$$;

COMMIT;
