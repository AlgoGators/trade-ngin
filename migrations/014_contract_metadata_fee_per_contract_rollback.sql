-- 014_contract_metadata_fee_per_contract_rollback.sql
--
-- Rollback for 014. Drops metadata.contract_metadata."Fee Per Contract".
--
-- READ THIS BEFORE RUNNING IT.
--
--   1. A binary built with CM1 runs on a database without the column: every futures fill is then
--      priced at the code default 1.50 per contract (one WARN per registry load says so). Rolling
--      the code back first is not required.
--   2. Dropping the column destroys any per-contract fee that differs from the 1.50 default (a
--      micro or mini row's own fee, say). So this file REFUSES while any row carries a value other
--      than '1.50', with a count. Dump those rows first, then, in the same psql session:
--          SET migration.allow_fee_drop = 'yes';
--          \i 014_contract_metadata_fee_per_contract_rollback.sql
--   3. DROP COLUMN is metadata-only (no rewrite); every other column keeps its values.

BEGIN;

DO $$
DECLARE
    n bigint := 0;
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.columns
                WHERE table_schema = 'metadata' AND table_name = 'contract_metadata'
                  AND column_name = 'Fee Per Contract') THEN
        SELECT count(*) INTO n FROM metadata.contract_metadata
         WHERE "Fee Per Contract" IS DISTINCT FROM '1.50';
    END IF;
    IF n > 0 AND coalesce(current_setting('migration.allow_fee_drop', true), '') <> 'yes' THEN
        RAISE EXCEPTION 'Refusing: % metadata.contract_metadata row(s) carry a "Fee Per Contract" '
                        'other than 1.50, which dropping the column destroys. Dump them, then SET '
                        'migration.allow_fee_drop = ''yes'' in this session and re-run.', n;
    END IF;
END $$;

ALTER TABLE metadata.contract_metadata DROP COLUMN IF EXISTS "Fee Per Contract";

COMMIT;

-- Verify:
--   SELECT count(*) FROM information_schema.columns
--    WHERE table_schema = 'metadata' AND table_name = 'contract_metadata'
--      AND column_name = 'Fee Per Contract';   -- expect 0
