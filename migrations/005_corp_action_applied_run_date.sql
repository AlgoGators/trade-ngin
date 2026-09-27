-- 005_corp_action_applied_run_date.sql
--
-- Add a nullable `run_date` to trading.corp_action_applied so a run can tell whether the
-- dedup rows it is about to trust were written by an EARLIER pass or a LATER one
-- (E2-F23, audit option C-prime).
--
-- WHY THIS COLUMN EXISTS
--
-- A live equity run is path-dependent. It loads the T-1 book, applies the corporate actions
-- the dedup table says are not yet applied, and finalizes T-1 from the resulting frame. That
-- is correct exactly when the dedup rows and the T-1 position row come from the SAME pass.
--
-- Reset the book but not the dedup table and it is not. Measured: re-seed the equity book at
-- 2026-04-02 and re-run 2026-04-07 while the dedup row (BKNG, SPLIT, 2026-04-06) survives
-- from the previous chain. The 25:1 split is skipped as "already applied" against a T-1 row
-- that is still PRE-split, the position is marked at the post-split close, and the run stores
-- an unrealized P&L of -4,824 on 1.2 shares. Nothing catches it: the G3 basis/mark guard only
-- inspects events that were APPLIED, and a deduped event never enters that list.
--
-- The existing detector -- refuse when a dedup row has ex_date >= the run date -- catches the
-- same-day case only. BKNG's ex-date is 2026-04-06 and the run is 2026-04-07, so it passes
-- the ex_date test while being exactly the failure. `applied_at` cannot substitute: an
-- earlier chain always has an earlier wall-clock, and so does legitimate history. The run
-- date of the pass that wrote the row is the only value that separates them.
--
-- WHAT USES IT
--
-- Detection only. `store_applied_corp_actions_in` stamps the runner's own `now` date on every
-- row it writes; `load_applied_corp_actions` returns it; and CorporateActionsAuditLog::load()
-- REFUSES the run (the runner exits 1, naming the offending rows) when any row for this
-- portfolio/strategy/name carries a run_date on or after today.
--
-- It deliberately does NOT self-heal. Deleting such rows automatically -- audit option C --
-- is only correct when the book was also reset, which the runner cannot verify, and it turns
-- today's accidentally-correct plain re-run of a deferred-apply day into a silent
-- double-apply for any event under 5x. The remedy is the protocol that was always required:
-- reset trading.corp_action_applied together with positions/live_results/equity_curve/
-- executions from the replay start date, then replay in order.
--
-- Legacy rows written before this migration have run_date NULL and are accepted. That is the
-- only safe reading -- NULL means "unknown", and refusing every pre-existing row would make
-- the next run unstartable.
--
-- FUTURES REACHABILITY, AND WHAT A REBUILD COSTS
--
-- trading.corp_action_applied is written and read by exactly two files:
-- src/live/corporate_actions_audit_log.cpp and apps/strategies/live_equity_mean_reversion.cpp.
-- live_portfolio.cpp and live_portfolio_conservative.cpp contain no reference to
-- corp_action, CorporateAction, or dividend. So no futures runner touches this table and no
-- futures number can move because of this migration.
--
-- The rebuild risk is the mirror of 004's. `trade_ngin` is a SHARED library, so the futures
-- binaries relink against the new code even though their call graph never reaches it -- their
-- sha256 changes and a checksum-only gate would report a difference that is not a behaviour
-- difference. Argue preservation from the call graph, not from the hash.
--
-- The equity-side rebuild risk is the one to respect: on a database rebuilt or restored
-- WITHOUT this migration, every INSERT into trading.corp_action_applied names a column that
-- does not exist. That is not silent -- store_applied_corp_actions_in returns an error and
-- the transaction that carries the adjusted positions rolls back with it, so the run fails
-- loudly rather than storing positions with no dedup record. Apply 005 before running the
-- equity book against a fresh database.
--
-- SAFETY
--   * Additive and idempotent: ADD COLUMN IF NOT EXISTS, nullable, no default, no rewrite of
--     existing rows and no table rewrite on PostgreSQL 11+.
--   * Touches no other table, no index, no constraint. The primary key is unchanged --
--     run_date is NOT part of the natural key (audit option D1 showed keying on it is unsafe:
--     the old row is ignored, ON CONFLICT DO NOTHING keeps the later run_date, and the event
--     re-applies every day).
--   * Transactional.

BEGIN;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.tables
        WHERE table_schema = 'trading' AND table_name = 'corp_action_applied'
    ) THEN
        RAISE EXCEPTION 'migration 002 has not been applied: trading.corp_action_applied does not exist';
    END IF;
END $$;

ALTER TABLE trading.corp_action_applied
    ADD COLUMN IF NOT EXISTS run_date DATE;

COMMENT ON COLUMN trading.corp_action_applied.run_date IS
    'Run date of the pass that wrote this row (the runner''s own as-of date, not a '
    'wall clock). NULL on rows written before migration 005 and accepted as unknown. '
    'A row whose run_date is on or after the current run date was written by a LATER '
    'pass over this book: the run refuses to start, because trusting it would skip an '
    'event whose effect is not in the T-1 position row (E2-F23 / E2-F16). Detection '
    'only -- nothing deletes these rows; reset the dedup table and the book together.';

COMMIT;

-- ---------------------------------------------------------------------------
-- VERIFICATION -- run after applying.
--
--   SELECT column_name, data_type, is_nullable
--     FROM information_schema.columns
--    WHERE table_schema = 'trading' AND table_name = 'corp_action_applied'
--      AND column_name = 'run_date';
--   -- expect: run_date | date | YES
--
--   SELECT count(*) AS total, count(run_date) AS stamped
--     FROM trading.corp_action_applied;
--   -- expect stamped = 0 immediately after applying; every row written from now on
--   -- carries a date.
-- ---------------------------------------------------------------------------
