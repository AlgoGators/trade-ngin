-- 015_executions_execution_type.sql
--
-- Add trading.executions.execution_type and backtest.executions.execution_type (text, NOT NULL,
-- DEFAULT 'STRATEGY', CHECK in STRATEGY / ROLL / BORROW) and instrument_id (text, NULL) to both
-- executions tables (LOOP_SPEC v6.1 sections 6.5 and 7; T-ROLLX commit 3).
--
-- WHY
--
-- A contract roll is booked as two fills on the confirming bar (the closing leg of the outgoing
-- contract at its last consumed close, the opening leg of the incoming contract at the change
-- bar's close), each with its cost. They are mechanical: inside the costs and the equity curve,
-- outside every strategy trade measure (trade counts, win rates, holding periods, reversal pairs,
-- churn, netting). Every reader needs to tell them from the day's STRATEGY fills, and the
-- backtest's synthetic overnight borrow-fee rows (quantity 0) are a third class that was untyped.
-- instrument_id names the contract a ROLL leg traded (the outgoing on the closing leg, the
-- incoming on the opening leg); NULL on every other row.
--
-- SAFETY
--
--   * ADD COLUMN with a constant DEFAULT is metadata-only on PostgreSQL 11 and later: every
--     existing row reads 'STRATEGY' (the only class that existed), instrument_id NULL.
--   * The CHECK constraint is added NOT VALID then validated: existing rows all read the default.
--   * No key, index or view is touched; no other column changes; number-neutral when applied.
--   * Type-guarded: an existing column of another type makes this refuse.
--   * Transactional and idempotent (ADD COLUMN IF NOT EXISTS; the constraint added only when
--     absent; COMMENT repeats).
--
-- ORDER AND NUMBERING
--
-- The T-ROLLX ids are assigned by table (LOOP_SPEC v6.1 section 7): 015 the executions tables
-- (this file, commit 3), 016 positions / final_positions (commit 2), 017 live_results roll
-- costs, 018 backtest.results costs. Applied to the stage-3 session clones in-session and to
-- production only at the end-of-stage merge on HD's go.

BEGIN;

DO $$
DECLARE
    t text;
BEGIN
    IF to_regclass('trading.executions') IS NULL OR to_regclass('backtest.executions') IS NULL THEN
        RAISE EXCEPTION 'trading.executions or backtest.executions does not exist';
    END IF;
    FOR t IN SELECT data_type FROM information_schema.columns
              WHERE table_name = 'executions' AND table_schema IN ('trading', 'backtest')
                AND column_name IN ('execution_type', 'instrument_id') AND data_type <> 'text' LOOP
        RAISE EXCEPTION 'an executions execution_type / instrument_id column exists with type %, not text', t;
    END LOOP;
END $$;

ALTER TABLE trading.executions ADD COLUMN IF NOT EXISTS execution_type text NOT NULL DEFAULT 'STRATEGY';
ALTER TABLE trading.executions ADD COLUMN IF NOT EXISTS instrument_id text;
ALTER TABLE backtest.executions ADD COLUMN IF NOT EXISTS execution_type text NOT NULL DEFAULT 'STRATEGY';
ALTER TABLE backtest.executions ADD COLUMN IF NOT EXISTS instrument_id text;

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'chk_executions_execution_type'
                      AND conrelid = 'trading.executions'::regclass) THEN
        ALTER TABLE trading.executions ADD CONSTRAINT chk_executions_execution_type
            CHECK (execution_type IN ('STRATEGY', 'ROLL', 'BORROW')) NOT VALID;
        ALTER TABLE trading.executions VALIDATE CONSTRAINT chk_executions_execution_type;
    END IF;
    IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'chk_backtest_executions_execution_type'
                      AND conrelid = 'backtest.executions'::regclass) THEN
        ALTER TABLE backtest.executions ADD CONSTRAINT chk_backtest_executions_execution_type
            CHECK (execution_type IN ('STRATEGY', 'ROLL', 'BORROW')) NOT VALID;
        ALTER TABLE backtest.executions VALIDATE CONSTRAINT chk_backtest_executions_execution_type;
    END IF;
END $$;

COMMENT ON COLUMN trading.executions.execution_type IS
    'STRATEGY: the day''s fill against the target. ROLL: one leg of a contract roll, booked on the confirming bar (closing leg at the last pre-change close, opening leg at the change bar''s close), mechanical: inside the costs and the equity curve, outside every strategy trade measure, excluded from netting. BORROW: the backtest''s synthetic overnight borrow-fee row (quantity 0). LOOP_SPEC v6.1 sections 6.5 and 7; migration 015.';
COMMENT ON COLUMN backtest.executions.execution_type IS
    'As trading.executions.execution_type (migration 015).';
COMMENT ON COLUMN trading.executions.instrument_id IS
    'The vendor contract id a ROLL leg traded (the outgoing contract on the closing leg, the incoming on the opening leg); NULL on every STRATEGY and BORROW row. Migration 015.';
COMMENT ON COLUMN backtest.executions.instrument_id IS
    'As trading.executions.instrument_id (migration 015).';

COMMIT;
