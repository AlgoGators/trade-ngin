-- 005_corp_action_applied_run_date_rollback.sql
--
-- Rollback for 005. Drops trading.corp_action_applied.run_date.
--
-- READ THIS BEFORE RUNNING IT.
--
-- Unlike 004's rollback, this one is safe to run under live code but NOT safe to run and
-- forget. Order matters and is the reverse of the forward migration:
--
--   1. Revert the CODE first. With 005's code in place and the column gone, every INSERT
--      into trading.corp_action_applied names a column that does not exist, and because the
--      dedup write shares a transaction with the adjusted positions, the whole unit rolls
--      back -- the equity run fails rather than half-applying. Loud, but a hard stop.
--   2. Rebuild and confirm nothing references the column:
--        grep -rn "run_date" src/ apps/ include/ | grep corp_action     -> expect 0
--   3. Only then run this file.
--
-- What is lost: the ability to tell an earlier pass's dedup rows from a later pass's. The
-- ex_date >= today detector remains and still catches the same-day case, but the measured
-- E2-F23 failure (BKNG, ex-date 2026-04-06, re-run of 2026-04-07 over an un-reset dedup
-- table, -4,824 of phantom unrealized P&L) passes that detector and would go back to being
-- undetectable. The replay protocol -- reset trading.corp_action_applied together with the
-- book -- becomes the only defence again, and forgetting it is silent.
--
-- Dropping the column destroys no dedup record: run_date is detection metadata, never part
-- of the natural key, so idempotency is unaffected by its absence.

BEGIN;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.columns
        WHERE table_schema = 'trading' AND table_name = 'corp_action_applied'
          AND column_name = 'run_date'
    ) THEN
        RAISE EXCEPTION 'migration 005 was never applied: trading.corp_action_applied.run_date does not exist';
    END IF;
END $$;

ALTER TABLE trading.corp_action_applied DROP COLUMN run_date;

COMMIT;

-- Verify the column is gone and the dedup rows survive:
--   SELECT count(*) FROM information_schema.columns
--    WHERE table_schema='trading' AND table_name='corp_action_applied'
--      AND column_name='run_date';                       -- expect 0
--   SELECT count(*) FROM trading.corp_action_applied;    -- expect the pre-rollback count
