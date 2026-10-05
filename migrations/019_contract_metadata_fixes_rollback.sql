-- 019_contract_metadata_fixes_rollback.sql
--
-- Rollback for 019. Restores the twenty cells 019 corrected to the values the tables held before
-- it: 6A and 6L "Tick Size" 0.0001 and "Minimum Price Fluctuation" 10; "IB Symbol" M6B, M6E, MSF,
-- YK, YW for 6B, 6E, 6S, ZS, ZW in metadata.contract_metadata and metadata.symbols; "Contract
-- Months" "Mar Jun Sep Dec" for HO, NG and 6L in both tables.
--
-- READ THIS BEFORE RUNNING IT.
--
--   1. It puts back values that are WRONG for the contracts the engine trades (a full point of
--      tick on 6A and 6L, the broker's micro and mini symbols, quarterly months). Its use is to
--      reproduce a number measured before 019.
--   2. Value-guarded like 019: a cell is written only when it holds 019's corrected value; a
--      cell already at the earlier value is left alone (a second run is a no-op); any other
--      value, or a missing row, refuses and changes nothing.
--   3. No DDL. No other cell is written.

BEGIN;

DO $$
DECLARE
    fix record;
    cur text;
    n   bigint;
BEGIN
    IF to_regclass('metadata.contract_metadata') IS NULL OR to_regclass('metadata.symbols') IS NULL THEN
        RAISE EXCEPTION 'metadata.contract_metadata or metadata.symbols does not exist; this '
                        'rollback restores cells of both, it creates neither';
    END IF;

    FOR fix IN
        SELECT * FROM (VALUES
            ('contract_metadata', '6A', 'Tick Size', '0.00005', '0.0001'),
            ('contract_metadata', '6A', 'Minimum Price Fluctuation', '5', '10'),
            ('contract_metadata', '6L', 'Tick Size', '0.00005', '0.0001'),
            ('contract_metadata', '6L', 'Minimum Price Fluctuation', '5', '10'),
            ('contract_metadata', '6B', 'IB Symbol', 'GBP', 'M6B'),
            ('contract_metadata', '6E', 'IB Symbol', 'EUR', 'M6E'),
            ('contract_metadata', '6S', 'IB Symbol', 'CHF', 'MSF'),
            ('contract_metadata', 'ZS', 'IB Symbol', 'ZS', 'YK'),
            ('contract_metadata', 'ZW', 'IB Symbol', 'ZW', 'YW'),
            ('symbols', '6B', 'IB Symbol', 'GBP', 'M6B'),
            ('symbols', '6E', 'IB Symbol', 'EUR', 'M6E'),
            ('symbols', '6S', 'IB Symbol', 'CHF', 'MSF'),
            ('symbols', 'ZS', 'IB Symbol', 'ZS', 'YK'),
            ('symbols', 'ZW', 'IB Symbol', 'ZW', 'YW'),
            ('contract_metadata', 'HO', 'Contract Months', 'All Months', 'Mar Jun Sep Dec'),
            ('contract_metadata', 'NG', 'Contract Months', 'All Months', 'Mar Jun Sep Dec'),
            ('contract_metadata', '6L', 'Contract Months', 'All Months', 'Mar Jun Sep Dec'),
            ('symbols', 'HO', 'Contract Months', 'All Months', 'Mar Jun Sep Dec'),
            ('symbols', 'NG', 'Contract Months', 'All Months', 'Mar Jun Sep Dec'),
            ('symbols', '6L', 'Contract Months', 'All Months', 'Mar Jun Sep Dec')
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
            RAISE EXCEPTION 'metadata.%."%" of % is ''%'', neither the corrected value this rollback '
                            'replaces (''%'') nor the earlier one (''%''): refusing',
                            fix.tbl, fix.col, fix.sym, cur, fix.old_value, fix.new_value;
        END IF;
        EXECUTE format('UPDATE metadata.%I SET %I = $1 WHERE "Databento Symbol" = $2',
                       fix.tbl, fix.col)
          USING fix.new_value, fix.sym;
    END LOOP;
END $$;

COMMIT;

