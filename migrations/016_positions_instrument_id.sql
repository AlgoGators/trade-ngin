-- 016_positions_instrument_id.sql
--
-- Add trading.positions.instrument_id and backtest.final_positions.instrument_id: the vendor's
-- contract id the futures position is held in on the row's date (LOOP_SPEC v6.1 section 7;
-- T-ROLLX commit 2).
--
-- WHY
--
-- The continuous series `.v.0` splices the vendor's volume-ranked contracts, and the stored
-- position never said which contract it was in. With the roll rules of LOOP_SPEC v6.1 (a change
-- bar is held, no P&L is recorded for it, the roll's two fills are booked on the confirming bar)
-- the row carries the confirmed contract id: still the outgoing contract on a pending change bar,
-- the new one from the confirming bar on. The id is the raw table's futures_data.ohlcv_1d_raw
-- .instrument_id of the kept print (text, as there).
--
-- NULL-able, no default: NULL means "unknown" (a kept print the raw table lacks, a row written
-- before this migration) and is the value of every equity row. AlgoLens's schema contract marks
-- positions as an insert table and refuses a NOT NULL column without a default on deploy
-- (reports/ROLL_COLUMN_CENSUS.md section 2.3), so the column ships nullable.
--
-- SAFETY
--
--   * ADD COLUMN without a default is metadata-only: no table rewrite, every existing row reads
--     NULL.
--   * No key, index, constraint or view is touched; no other column changes; number-neutral when
--     applied (the engine writes the column from this commit on; nothing reads it for a number).
--   * Type-guarded: an existing instrument_id of another type makes this refuse.
--   * Transactional and idempotent (ADD COLUMN IF NOT EXISTS; COMMENT repeats).
--
-- ORDER AND NUMBERING
--
-- 015 (executions.execution_type and instrument_id) lands with T-ROLLX commit 3; the ids are
-- assigned by table, not by commit (LOOP_SPEC v6.1 section 7): 016 is this table pair. Applied to
-- the stage-3 session clones in-session and to production only at the end-of-stage merge on HD's go.

BEGIN;

DO $$
DECLARE
    t text;
BEGIN
    IF to_regclass('trading.positions') IS NULL THEN
        RAISE EXCEPTION 'trading.positions does not exist; this migration adds a column to it';
    END IF;
    IF to_regclass('backtest.final_positions') IS NULL THEN
        RAISE EXCEPTION 'backtest.final_positions does not exist; this migration adds a column to it';
    END IF;
    SELECT data_type INTO t FROM information_schema.columns
     WHERE table_schema = 'trading' AND table_name = 'positions' AND column_name = 'instrument_id';
    IF t IS NOT NULL AND t <> 'text' THEN
        RAISE EXCEPTION 'trading.positions.instrument_id exists with type %, not text', t;
    END IF;
    SELECT data_type INTO t FROM information_schema.columns
     WHERE table_schema = 'backtest' AND table_name = 'final_positions' AND column_name = 'instrument_id';
    IF t IS NOT NULL AND t <> 'text' THEN
        RAISE EXCEPTION 'backtest.final_positions.instrument_id exists with type %, not text', t;
    END IF;
END $$;

ALTER TABLE trading.positions ADD COLUMN IF NOT EXISTS instrument_id text;
ALTER TABLE backtest.final_positions ADD COLUMN IF NOT EXISTS instrument_id text;

COMMENT ON COLUMN trading.positions.instrument_id IS
    'The vendor contract id (futures_data.ohlcv_1d_raw.instrument_id) the futures position is held in on this date: the confirmed id of the symbol''s consumed bar sequence, still the outgoing contract on a pending change bar (LOOP_SPEC v6.1 sections 2.2, 6.5, 7; migration 016). NULL: unknown, a row written before 016, or not a futures row.';
COMMENT ON COLUMN backtest.final_positions.instrument_id IS
    'The vendor contract id (futures_data.ohlcv_1d_raw.instrument_id) the futures position is held in on this date: the confirmed id of the symbol''s consumed bar sequence, still the outgoing contract on a pending change bar (LOOP_SPEC v6.1 sections 2.2, 6.5, 7; migration 016). NULL: unknown, a row written before 016, or not a futures row.';

COMMIT;
