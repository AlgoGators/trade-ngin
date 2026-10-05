-- 019_contract_metadata_fixes.sql
--
-- Correct nine rows of the contract metadata: the tick of two FX contracts, the broker symbol of
-- five full-size contracts, and the listed months of three contracts. A DATA migration: no DDL,
-- no new row, no micro or mini row.
--
-- WHAT
--
--   1. 6A and 6L, metadata.contract_metadata: "Tick Size" 0.0001 -> 0.00005 and "Minimum Price
--      Fluctuation" 10 -> 5. The exchange tick of both contracts is half a point (0.00005 on a
--      multiplier of 100,000 = 5 dollars); the table carried a full point.
--   2. 6B, 6E, 6S, ZS, ZW, "IB Symbol" in metadata.contract_metadata AND metadata.symbols:
--      M6B -> GBP, M6E -> EUR, MSF -> CHF, YK -> ZS, YW -> ZW. The engine trades the full-size
--      contracts (62,500 pounds, 125,000 euros, 125,000 francs, 5,000 bushels); the table named
--      the broker's micro FX and mini grain contracts. The FX values follow the other FX rows
--      (6C CAD, 6J JPY, 6N NZD).
--   3. HO, NG, 6L, "Contract Months" in both tables: "Mar Jun Sep Dec" -> "All Months" (the
--      wording of the CL row). All three list a contract for every calendar month.
--
-- WHY IT MOVES NUMBERS
--
-- "Tick Size" and "Minimum Price Fluctuation" feed the transaction cost model (tick_size and
-- tick_value of the contract; InstrumentRegistry::load_instruments), so every 6A and 6L fill is
-- priced on the corrected tick and everything sized after those costs follows. "IB Symbol" and
-- "Contract Months" are read by the daily email only (its symbol reference and rollover-warning
-- cells). No other consumer reads the changed cells.
--
-- SAFETY
--
--   * Value-guarded cell by cell: a cell is written only when it holds the value this file
--     replaces. A cell that already holds the corrected value is left alone, so a second apply
--     is a no-op. A cell that holds anything else, or a missing row, makes the whole migration
--     refuse and change nothing.
--   * Both tables are corrected in one transaction: metadata.symbols duplicates "IB Symbol" and
--     "Contract Months".
--   * No column, key, index, constraint or view is touched; no other cell changes. The four
--     margin columns and "Fee Per Contract" are not written.
--
-- ORDER AND NUMBERING
--
-- Independent DDL-wise of 001-018; 019 is the next number of the stage-3 lane. Apply it before
-- the binary that is measured against it. Its rollback restores the replaced values.

BEGIN;

DO $$
DECLARE
    fix record;
    cur text;
    n   bigint;
BEGIN
    IF to_regclass('metadata.contract_metadata') IS NULL OR to_regclass('metadata.symbols') IS NULL THEN
        RAISE EXCEPTION 'metadata.contract_metadata or metadata.symbols does not exist; this '
                        'migration corrects cells of both, it creates neither';
    END IF;

    FOR fix IN
        SELECT * FROM (VALUES
            ('contract_metadata', '6A', 'Tick Size',                 '0.0001',          '0.00005'),
            ('contract_metadata', '6A', 'Minimum Price Fluctuation', '10',              '5'),
            ('contract_metadata', '6L', 'Tick Size',                 '0.0001',          '0.00005'),
            ('contract_metadata', '6L', 'Minimum Price Fluctuation', '10',              '5'),
            ('contract_metadata', '6B', 'IB Symbol',                 'M6B',             'GBP'),
            ('contract_metadata', '6E', 'IB Symbol',                 'M6E',             'EUR'),
            ('contract_metadata', '6S', 'IB Symbol',                 'MSF',             'CHF'),
            ('contract_metadata', 'ZS', 'IB Symbol',                 'YK',              'ZS'),
            ('contract_metadata', 'ZW', 'IB Symbol',                 'YW',              'ZW'),
            ('symbols',           '6B', 'IB Symbol',                 'M6B',             'GBP'),
            ('symbols',           '6E', 'IB Symbol',                 'M6E',             'EUR'),
            ('symbols',           '6S', 'IB Symbol',                 'MSF',             'CHF'),
            ('symbols',           'ZS', 'IB Symbol',                 'YK',              'ZS'),
            ('symbols',           'ZW', 'IB Symbol',                 'YW',              'ZW'),
            ('contract_metadata', 'HO', 'Contract Months',           'Mar Jun Sep Dec', 'All Months'),
            ('contract_metadata', 'NG', 'Contract Months',           'Mar Jun Sep Dec', 'All Months'),
            ('contract_metadata', '6L', 'Contract Months',           'Mar Jun Sep Dec', 'All Months'),
            ('symbols',           'HO', 'Contract Months',           'Mar Jun Sep Dec', 'All Months'),
            ('symbols',           'NG', 'Contract Months',           'Mar Jun Sep Dec', 'All Months'),
            ('symbols',           '6L', 'Contract Months',           'Mar Jun Sep Dec', 'All Months')
        ) AS f(tbl, sym, col, old_value, new_value)
    LOOP
        EXECUTE format('SELECT count(*), max(%I) FROM metadata.%I WHERE "Databento Symbol" = $1',
                       fix.col, fix.tbl)
           INTO n, cur USING fix.sym;
        IF n <> 1 THEN
            RAISE EXCEPTION 'metadata.% holds % row(s) for %, expected exactly 1: refusing',
                            fix.tbl, n, fix.sym;
        END IF;
        IF cur = fix.new_value THEN
            CONTINUE;
        END IF;
        IF cur IS DISTINCT FROM fix.old_value THEN
            RAISE EXCEPTION 'metadata.%."%" of % is ''%'', neither the value this migration '
                            'replaces (''%'') nor the corrected one (''%''): refusing',
                            fix.tbl, fix.col, fix.sym, cur, fix.old_value, fix.new_value;
        END IF;
        EXECUTE format('UPDATE metadata.%I SET %I = $1 WHERE "Databento Symbol" = $2',
                       fix.tbl, fix.col)
          USING fix.new_value, fix.sym;
    END LOOP;
END $$;

COMMIT;

-- Verify:
--   SELECT "Databento Symbol", "IB Symbol", "Tick Size", "Minimum Price Fluctuation", "Contract Months"
--     FROM metadata.contract_metadata
--    WHERE "Databento Symbol" IN ('6A','6L','6B','6E','6S','ZS','ZW','HO','NG') ORDER BY 1;
--   SELECT "Databento Symbol", "IB Symbol", "Contract Months" FROM metadata.symbols
--    WHERE "Databento Symbol" IN ('6L','6B','6E','6S','ZS','ZW','HO','NG') ORDER BY 1;
