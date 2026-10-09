-- 014_contract_metadata_fee_per_contract.sql
--
-- Add metadata.contract_metadata."Fee Per Contract": the dollars per contract per side the
-- transaction cost model charges for a futures fill, as a column of the contract's own metadata
-- row (T-7b-2 CM1).
--
-- WHY
--
-- The cost model priced every futures fill's explicit fee with a code default of 1.50 per
-- contract (TransactionCostManager::Config, BacktestExecutionConfig, ExecutionConfig), while the
-- contract size the strategy sizes with comes from this table. HD 2026-09-25 (rulings 11 and 14):
-- one source of truth for contract specs, nothing hard-coded, costs from the metadata. After this
-- column exists the instrument registry reads it into each futures contract and the cost model
-- charges it; the code default remains only for a database without the column.
--
-- Every existing row gets the configured default fee per contract, 1.50, the reference until
-- per-row fees land (HD ruling 33, 2026-09-26), so every fill is priced exactly as before: the
-- column is number-neutral when applied. A micro or mini contract whose broker fee differs is
-- updated row by row by the migration that changes that row's contract, with its source.
--
-- The column is text, like every other column of the table; the loader converts it to a double
-- by name (PostgresDatabase::convert_metadata_to_arrow), so the loader's positional indices of
-- the older columns do not move.
--
-- SAFETY
--
--   * ADD COLUMN with a constant DEFAULT is metadata-only on PostgreSQL 11 and later: no table
--     rewrite, every existing row reads the default.
--   * NOT NULL holds from the start (every row reads '1.50'); a later INSERT that omits the
--     column gets '1.50'.
--   * No key, index, constraint or view is touched; no other column changes.
--   * Type-guarded: an existing "Fee Per Contract" of another type makes this refuse.
--   * Transactional and idempotent (ADD COLUMN IF NOT EXISTS; COMMENT repeats).
--
-- ORDER AND NUMBERING
--
-- Independent of 001-013; 014 is the next free number after 013 (T-7b-2 8b). Like 012 and 013,
-- applied to the stage-3 session clones in-session and to production only at the end-of-stage
-- merge on HD's go.
--
-- REACHABILITY
--
-- Every futures runner and backtest loads this table through InstrumentRegistry::load_instruments
-- (SELECT *). With the column present each futures contract carries its fee, and all three cost
-- managers (the backtest fills, the live fills, the PortfolioManager's optimizer cost vector)
-- charge it. The equity cost configs charge per share and never read it.

BEGIN;

DO $$
DECLARE
    t text;
BEGIN
    IF to_regclass('metadata.contract_metadata') IS NULL THEN
        RAISE EXCEPTION 'metadata.contract_metadata does not exist; this migration adds a column '
                        'to it, it does not create the table';
    END IF;
    SELECT data_type INTO t
      FROM information_schema.columns
     WHERE table_schema = 'metadata' AND table_name = 'contract_metadata'
       AND column_name = 'Fee Per Contract';
    IF t IS NOT NULL AND t <> 'text' THEN
        RAISE EXCEPTION 'metadata.contract_metadata."Fee Per Contract" already exists as %, not '
                        'text: refusing', t;
    END IF;
END $$;

ALTER TABLE metadata.contract_metadata
    ADD COLUMN IF NOT EXISTS "Fee Per Contract" TEXT NOT NULL DEFAULT '1.50';

COMMENT ON COLUMN metadata.contract_metadata."Fee Per Contract" IS
    'Dollars per contract per side that the transaction cost model charges for a fill of this '
    'contract (explicit fee: broker commission plus exchange and clearing, all-in). Every row '
    'starts at the configured default fee per contract, 1.50, the reference until per-row fees '
    'land (HD ruling 33, 2026-09-26). Read by the instrument registry by name; a database without '
    'this column prices every fill at the code default 1.50 (migration 014).';

COMMIT;
