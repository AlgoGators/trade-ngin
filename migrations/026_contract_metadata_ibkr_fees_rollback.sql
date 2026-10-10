-- 026_contract_metadata_ibkr_fees_rollback.sql
--
-- Rollback for 026. Restores "Fee Per Contract" of the 40 rows of metadata.contract_metadata to
-- the values the table held before it (listed below, row by row: every one was 1.50, migration
-- 014's value) and drops the CHECK 026 added.
--
-- READ THIS BEFORE RUNNING IT.
--
--   1. It puts back a flat fee that is not IBKR's schedule. Its use is to reproduce a number
--      measured before 026.
--   2. Value-guarded like 026: a cell is written only when it holds 026's fee; a cell already at
--      the earlier value is left alone (a second run is a no-op); any other value, or a row
--      list that is not the 40, refuses and changes nothing.
--   3. With the CHECK gone an empty fee cell is legal again, and the loader reads it as a fee
--      of 0 with no message.
--   4. No other cell is written. No table is made or dropped.

BEGIN;

CREATE TEMP TABLE _m026_fees (sym text PRIMARY KEY, old_fee text NOT NULL, fee text NOT NULL)
    ON COMMIT DROP;
INSERT INTO _m026_fees VALUES
  ('6A',  '1.50', '2.461'), ('6B',  '1.50', '2.461'), ('6C',  '1.50', '2.461'),
  ('6E',  '1.50', '2.461'), ('6J',  '1.50', '2.461'), ('6L',  '1.50', '2.461'),
  ('6M',  '1.50', '2.461'), ('6N',  '1.50', '2.461'), ('6S',  '1.50', '2.461'),
  ('ES',  '1.50', '2.247'), ('NQ',  '1.50', '2.247'), ('RTY', '1.50', '2.247'),
  ('MES', '1.50', '0.614'), ('MNQ', '1.50', '0.614'), ('M2K', '1.50', '0.614'),
  ('MBT', '1.50', '2.011'),
  ('GF',  '1.50', '2.961'), ('HE',  '1.50', '2.961'), ('LE',  '1.50', '2.961'),
  ('YM',  '1.50', '2.240'), ('MYM', '1.50', '0.610'),
  ('ZC',  '1.50', '3.010'), ('ZL',  '1.50', '3.010'), ('ZM',  '1.50', '3.010'),
  ('ZR',  '1.50', '3.010'), ('ZS',  '1.50', '3.010'), ('ZW',  '1.50', '3.010'),
  ('KE',  '1.50', '3.010'),
  ('ZT',  '1.50', '1.510'), ('ZF',  '1.50', '1.510'), ('ZN',  '1.50', '1.660'),
  ('UB',  '1.50', '1.810'),
  ('CL',  '1.50', '2.360'), ('HO',  '1.50', '2.360'), ('RB',  '1.50', '2.360'),
  ('NG',  '1.50', '2.460'), ('PL',  '1.50', '2.510'),
  ('GC',  '1.50', '2.510'), ('HG',  '1.50', '2.510'), ('SI',  '1.50', '2.510');

DO $$
DECLARE
    bad text;
BEGIN
    IF to_regclass('metadata.contract_metadata') IS NULL THEN
        RAISE EXCEPTION '026 rollback: metadata.contract_metadata does not exist';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                    WHERE table_schema = 'metadata' AND table_name = 'contract_metadata'
                      AND column_name = 'Fee Per Contract') THEN
        RAISE EXCEPTION '026 rollback: metadata.contract_metadata."Fee Per Contract" is missing';
    END IF;

    SELECT string_agg(coalesce(f.sym, c."Databento Symbol"), ', '
                      ORDER BY coalesce(f.sym, c."Databento Symbol")) INTO bad
      FROM _m026_fees f
      FULL JOIN metadata.contract_metadata c ON c."Databento Symbol" = f.sym
     WHERE f.sym IS NULL OR c."Databento Symbol" IS NULL;
    IF bad IS NOT NULL THEN
        RAISE EXCEPTION '026 rollback: the table and the fee list do not name the same rows (%); '
                        'nothing changed', bad;
    END IF;
    SELECT string_agg(f.sym, ', ' ORDER BY f.sym) INTO bad
      FROM _m026_fees f
     WHERE (SELECT count(*) FROM metadata.contract_metadata c
             WHERE c."Databento Symbol" = f.sym) <> 1;
    IF bad IS NOT NULL THEN
        RAISE EXCEPTION '026 rollback: row(s) not found exactly once: %; nothing changed', bad;
    END IF;

    SELECT string_agg(f.sym || ' holds ''' || c."Fee Per Contract" || '''', ', ' ORDER BY f.sym)
      INTO bad
      FROM _m026_fees f
      JOIN metadata.contract_metadata c ON c."Databento Symbol" = f.sym
     WHERE c."Fee Per Contract" NOT IN (f.old_fee, f.fee);
    IF bad IS NOT NULL THEN
        RAISE EXCEPTION '026 rollback: "Fee Per Contract" is neither 026''s fee nor the earlier '
                        'value on: %; nothing changed', bad;
    END IF;
END $$;

ALTER TABLE metadata.contract_metadata
    DROP CONSTRAINT IF EXISTS contract_metadata_fee_per_contract_positive;

UPDATE metadata.contract_metadata c
   SET "Fee Per Contract" = f.old_fee
  FROM _m026_fees f
 WHERE c."Databento Symbol" = f.sym AND c."Fee Per Contract" = f.fee;

DO $$
DECLARE
    bad text;
BEGIN
    SELECT string_agg(f.sym, ', ' ORDER BY f.sym) INTO bad
      FROM _m026_fees f
      JOIN metadata.contract_metadata c ON c."Databento Symbol" = f.sym
     WHERE c."Fee Per Contract" IS DISTINCT FROM f.old_fee;
    IF bad IS NOT NULL THEN
        RAISE EXCEPTION '026 rollback post-check: the fee is not the earlier value on: %', bad;
    END IF;
END $$;

COMMENT ON COLUMN metadata.contract_metadata."Fee Per Contract" IS
    'Dollars per contract per side that the transaction cost model charges for a fill of this '
    'contract (explicit fee: broker commission plus exchange and clearing, all-in). Every row '
    'starts at the configured default fee per contract, 1.50, the reference until per-row fees '
    'land (HD ruling 33, 2026-09-26). Read by the instrument registry by name; a database without '
    'this column prices every fill at the code default 1.50 (migration 014).';

COMMIT;
