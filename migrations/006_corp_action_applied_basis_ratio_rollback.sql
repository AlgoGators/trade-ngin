-- 006_corp_action_applied_basis_ratio_rollback.sql
--
-- Rollback for 006. Drops trading.corp_action_applied.basis_ratio.
--
-- READ THIS BEFORE RUNNING IT.
--
-- Same ordering hazard as 005's rollback. With 006's code in place and the column gone,
-- every INSERT into trading.corp_action_applied names a column that does not exist, and
-- because the dedup write shares a transaction with the adjusted positions, the whole unit
-- rolls back -- the equity run fails rather than half-applying. Loud, but a hard stop.
--
--   1. Revert the CODE first.
--   2. Rebuild and confirm nothing references the column:
--        grep -rn "basis_ratio" src/ apps/ include/       -> expect 0
--   3. Only then run this file.
--
-- What is lost: the ability to invert an adjusted basis back to the broker's frame from the
-- ledger. The chain can still be RECOMPUTED by joining every applied event back to
-- equities_data.ohlcv_1d for its raw ex-date close -- except for a position that has since
-- closed, where the basis is gone (average_price = 0 on a closed row) and the dedup row was
-- the only surviving record of the chain. F-8's identity 2 becomes unverifiable for those.
--
-- Dropping the column destroys no dedup record: basis_ratio is reconciliation metadata,
-- never part of the natural key, so idempotency is unaffected by its absence.

BEGIN;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.columns
        WHERE table_schema = 'trading' AND table_name = 'corp_action_applied'
          AND column_name = 'basis_ratio'
    ) THEN
        RAISE EXCEPTION 'migration 006 was never applied: trading.corp_action_applied.basis_ratio does not exist';
    END IF;
END $$;

ALTER TABLE trading.corp_action_applied DROP COLUMN basis_ratio;

COMMIT;

-- Verify the column is gone and the dedup rows survive:
--   SELECT count(*) FROM information_schema.columns
--    WHERE table_schema='trading' AND table_name='corp_action_applied'
--      AND column_name='basis_ratio';                    -- expect 0
--   SELECT count(*) FROM trading.corp_action_applied;    -- expect the pre-rollback count
