-- 026_contract_metadata_ibkr_fees.sql
--
-- Price "Fee Per Contract" of the 40 rows of metadata.contract_metadata per row from IBKR's
-- published futures schedule, and refuse a fee cell that is not a positive number. A DATA
-- migration plus one CHECK constraint: no column, no table, no row added or removed, no micro or
-- mini row.
--
-- WHAT
--
--   1. "Fee Per Contract" of every row = IBKR's commission + the exchange fee recovery charge +
--      the regulatory fee recovery charge, in dollars per contract per side:
--        * commission: the lowest-volume tier (up to 1,000 contracts a month), the same under
--          Tiered and Fixed: 0.85 for a standard contract and for CME Bitcoin Micro (MBT), 0.25
--          for an E-micro contract (MES, MNQ, M2K, MYM);
--        * exchange fee recovery charge: the non-member column of IBKR's page for the venue;
--        * regulatory fee recovery charge: 0.011 on the CME page, 0.01 on the CBOT, NYMEX and
--          COMEX pages. IBKR's pages say it is the National Futures Association's assessment.
--      Read on 2026-10-08 from interactivebrokers.com/en/pricing/commissions-futures.php and
--      interactivebrokers.com/en/accounts/fees/{CME,CBOT,NYMEX,COMEX}.php. Every one of the 40
--      rows has a figure on those pages, so the 1.50 fallback (HD ruling 33) is used on no row.
--   2. A CHECK on the column: the cell is a plain decimal number above zero. The column is text
--      NOT NULL, so an empty cell was legal, and the loader reads an empty cell as a fee of 0
--      with no message (PostgresDatabase::convert_metadata_to_arrow). With the CHECK an empty,
--      zero, negative or non-numeric fee cannot be stored.
--
-- WHAT THE FIGURE DOES NOT ITEMISE
--
-- IBKR's pages itemise no clearing fee for these venues (its own worked example on the
-- commission page prints "Clearing Fee USD 0.00" for an ES contract), and the notes of the fee
-- pages say the charges "may be higher or lower than the fees charged by the exchange". The
-- pages list a give-up surcharge and an overnight position fee, which do not apply to a fill
-- IBKR both executes and clears and are not in the figure. The desk's IB statement is the later
-- check on the total.
--
-- WHY IT MOVES NUMBERS
--
-- The instrument registry reads the cell into each futures contract and all three cost managers
-- charge it (the backtest fills, the live fills, the optimiser's cost vector), so every futures
-- fill's commission and total cost moves, and everything sized after the first changed cost
-- follows. The equity cost model charges per share and never reads the column. Every row was
-- 1.50 (migration 014): 36 rows rise (to between 1.510 and 3.010) and the four micro equity
-- index rows fall (to 0.610 and 0.614).
--
-- SAFETY
--
--   * Refuses unless the table holds exactly the 40 rows of the list below, each once.
--   * Value-guarded cell by cell: a cell is written only when it holds 1.50, the value this file
--     replaces. A cell that already holds its listed fee is left alone, so a second apply is a
--     no-op. A cell that holds anything else makes the whole migration refuse and change nothing.
--   * No other cell, column, key or index is touched. The four margin columns are not written.
--   * One transaction.
--
-- ORDER AND NUMBERING
--
-- Needs 014 (the column). Independent of 015-020; 026 is the next number of the stage-3 lane.
-- Apply it before the binary that is measured against it. Its rollback carries the 40 replaced
-- values and drops the CHECK; no snapshot table is made.

BEGIN;

CREATE TEMP TABLE _m026_fees (sym text PRIMARY KEY, old_fee text NOT NULL, fee text NOT NULL,
                              detail text NOT NULL) ON COMMIT DROP;
INSERT INTO _m026_fees VALUES
  ('6A',  '1.50', '2.461', 'CME standard 0.85 + exchange 1.600 + regulatory 0.011'),
  ('6B',  '1.50', '2.461', 'CME standard 0.85 + exchange 1.600 + regulatory 0.011'),
  ('6C',  '1.50', '2.461', 'CME standard 0.85 + exchange 1.600 + regulatory 0.011'),
  ('6E',  '1.50', '2.461', 'CME standard 0.85 + exchange 1.600 + regulatory 0.011'),
  ('6J',  '1.50', '2.461', 'CME standard 0.85 + exchange 1.600 + regulatory 0.011'),
  ('6L',  '1.50', '2.461', 'CME standard 0.85 + exchange 1.600 + regulatory 0.011'),
  ('6M',  '1.50', '2.461', 'CME standard 0.85 + exchange 1.600 + regulatory 0.011'),
  ('6N',  '1.50', '2.461', 'CME standard 0.85 + exchange 1.600 + regulatory 0.011'),
  ('6S',  '1.50', '2.461', 'CME standard 0.85 + exchange 1.600 + regulatory 0.011'),
  ('ES',  '1.50', '2.247', 'CME standard 0.85 + exchange 1.386 + regulatory 0.011'),
  ('NQ',  '1.50', '2.247', 'CME standard 0.85 + exchange 1.386 + regulatory 0.011'),
  ('RTY', '1.50', '2.247', 'CME standard 0.85 + exchange 1.386 + regulatory 0.011'),
  ('MES', '1.50', '0.614', 'CME E-micro 0.25 + exchange 0.353 + regulatory 0.011'),
  ('MNQ', '1.50', '0.614', 'CME E-micro 0.25 + exchange 0.353 + regulatory 0.011'),
  ('M2K', '1.50', '0.614', 'CME E-micro 0.25 + exchange 0.353 + regulatory 0.011'),
  ('MBT', '1.50', '2.011', 'CME Bitcoin Micro 0.85 + exchange 1.150 + regulatory 0.011'),
  ('GF',  '1.50', '2.961', 'CME standard 0.85 + exchange 2.100 + regulatory 0.011'),
  ('HE',  '1.50', '2.961', 'CME standard 0.85 + exchange 2.100 + regulatory 0.011'),
  ('LE',  '1.50', '2.961', 'CME standard 0.85 + exchange 2.100 + regulatory 0.011'),
  ('YM',  '1.50', '2.240', 'CBOT standard 0.85 + exchange 1.380 + regulatory 0.01'),
  ('MYM', '1.50', '0.610', 'CBOT E-micro 0.25 + exchange 0.350 + regulatory 0.01'),
  ('ZC',  '1.50', '3.010', 'CBOT standard 0.85 + exchange 2.150 + regulatory 0.01'),
  ('ZL',  '1.50', '3.010', 'CBOT standard 0.85 + exchange 2.150 + regulatory 0.01'),
  ('ZM',  '1.50', '3.010', 'CBOT standard 0.85 + exchange 2.150 + regulatory 0.01'),
  ('ZR',  '1.50', '3.010', 'CBOT standard 0.85 + exchange 2.150 + regulatory 0.01'),
  ('ZS',  '1.50', '3.010', 'CBOT standard 0.85 + exchange 2.150 + regulatory 0.01'),
  ('ZW',  '1.50', '3.010', 'CBOT standard 0.85 + exchange 2.150 + regulatory 0.01'),
  ('KE',  '1.50', '3.010', 'CBOT standard 0.85 + exchange 2.150 + regulatory 0.01'),
  ('ZT',  '1.50', '1.510', 'CBOT standard 0.85 + exchange 0.650 + regulatory 0.01'),
  ('ZF',  '1.50', '1.510', 'CBOT standard 0.85 + exchange 0.650 + regulatory 0.01'),
  ('ZN',  '1.50', '1.660', 'CBOT standard 0.85 + exchange 0.800 + regulatory 0.01'),
  ('UB',  '1.50', '1.810', 'CBOT standard 0.85 + exchange 0.950 + regulatory 0.01'),
  ('CL',  '1.50', '2.360', 'NYMEX standard 0.85 + exchange 1.500 + regulatory 0.01'),
  ('HO',  '1.50', '2.360', 'NYMEX standard 0.85 + exchange 1.500 + regulatory 0.01'),
  ('RB',  '1.50', '2.360', 'NYMEX standard 0.85 + exchange 1.500 + regulatory 0.01'),
  ('NG',  '1.50', '2.460', 'NYMEX standard 0.85 + exchange 1.600 + regulatory 0.01'),
  ('PL',  '1.50', '2.510', 'NYMEX standard 0.85 + exchange 1.650 + regulatory 0.01'),
  ('GC',  '1.50', '2.510', 'COMEX standard 0.85 + exchange 1.650 + regulatory 0.01'),
  ('HG',  '1.50', '2.510', 'COMEX standard 0.85 + exchange 1.650 + regulatory 0.01'),
  ('SI',  '1.50', '2.510', 'COMEX standard 0.85 + exchange 1.650 + regulatory 0.01');

DO $$
DECLARE
    bad text;
BEGIN
    IF to_regclass('metadata.contract_metadata') IS NULL THEN
        RAISE EXCEPTION '026: metadata.contract_metadata does not exist; this migration prices '
                        'its rows, it does not create the table';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                    WHERE table_schema = 'metadata' AND table_name = 'contract_metadata'
                      AND column_name = 'Fee Per Contract' AND data_type = 'text') THEN
        RAISE EXCEPTION '026: metadata.contract_metadata."Fee Per Contract" (text) is missing: '
                        'apply migration 014 first';
    END IF;

    SELECT string_agg(coalesce(f.sym, c."Databento Symbol"), ', '
                      ORDER BY coalesce(f.sym, c."Databento Symbol")) INTO bad
      FROM _m026_fees f
      FULL JOIN metadata.contract_metadata c ON c."Databento Symbol" = f.sym
     WHERE f.sym IS NULL OR c."Databento Symbol" IS NULL;
    IF bad IS NOT NULL THEN
        RAISE EXCEPTION '026: the table and the fee list do not name the same rows (%); '
                        'nothing changed', bad;
    END IF;
    SELECT string_agg(f.sym, ', ' ORDER BY f.sym) INTO bad
      FROM _m026_fees f
     WHERE (SELECT count(*) FROM metadata.contract_metadata c
             WHERE c."Databento Symbol" = f.sym) <> 1;
    IF bad IS NOT NULL THEN
        RAISE EXCEPTION '026: row(s) not found exactly once: %; nothing changed', bad;
    END IF;

    -- The value guard: every cell holds the replaced value or already the listed fee.
    SELECT string_agg(f.sym || ' holds ''' || c."Fee Per Contract" || '''', ', ' ORDER BY f.sym)
      INTO bad
      FROM _m026_fees f
      JOIN metadata.contract_metadata c ON c."Databento Symbol" = f.sym
     WHERE c."Fee Per Contract" NOT IN (f.old_fee, f.fee);
    IF bad IS NOT NULL THEN
        RAISE EXCEPTION '026: "Fee Per Contract" is neither the replaced value nor the listed '
                        'fee on: %; nothing changed', bad;
    END IF;
END $$;

UPDATE metadata.contract_metadata c
   SET "Fee Per Contract" = f.fee
  FROM _m026_fees f
 WHERE c."Databento Symbol" = f.sym AND c."Fee Per Contract" = f.old_fee;

DO $$
DECLARE
    bad text;
BEGIN
    SELECT string_agg(f.sym, ', ' ORDER BY f.sym) INTO bad
      FROM _m026_fees f
      JOIN metadata.contract_metadata c ON c."Databento Symbol" = f.sym
     WHERE c."Fee Per Contract" IS DISTINCT FROM f.fee;
    IF bad IS NOT NULL THEN
        RAISE EXCEPTION '026 post-check: the fee is not the listed value on: %', bad;
    END IF;

    IF NOT EXISTS (SELECT 1 FROM pg_constraint
                    WHERE conrelid = 'metadata.contract_metadata'::regclass
                      AND conname = 'contract_metadata_fee_per_contract_positive') THEN
        ALTER TABLE metadata.contract_metadata
            ADD CONSTRAINT contract_metadata_fee_per_contract_positive
            CHECK (CASE WHEN "Fee Per Contract" ~ '^[0-9]+(\.[0-9]+)?$'
                        THEN "Fee Per Contract"::numeric > 0
                        ELSE false END);
    END IF;
END $$;

COMMENT ON COLUMN metadata.contract_metadata."Fee Per Contract" IS
    'Dollars per contract per side that the transaction cost model charges for a fill of this '
    'contract: IBKR''s commission (lowest-volume tier) + exchange fee recovery charge '
    '(non-member) + regulatory fee recovery charge, from IBKR''s published schedule read '
    '2026-10-08 (migration 026). IBKR itemises no clearing fee for these venues; the IB '
    'statement is the check on the total. A plain positive decimal number (CHECK '
    'contract_metadata_fee_per_contract_positive). Read by the instrument registry by name; a '
    'database without this column prices every fill at the code default 1.50 (migration 014).';

COMMIT;
