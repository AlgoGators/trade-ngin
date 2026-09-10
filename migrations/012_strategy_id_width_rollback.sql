-- 012_strategy_id_width_rollback.sql
--
-- Rollback for 012. Narrows trading.positions.strategy_id, trading.live_results.strategy_id
-- and trading.signals.strategy_id from varchar(100) back to varchar(50).
--
-- READ THIS BEFORE RUNNING IT.
--
-- Unlike the migration, this one CAN LOSE DATA, and PostgreSQL will not do it quietly: a
-- narrowing scans the table and fails with "value too long for type character varying(50)"
-- if any row is wider than 50. That is the safe outcome. The unsafe outcome is the one this
-- file exists to prevent, so it checks first and refuses with a count and an example rather
-- than letting the ALTER fail halfway through a three-table transaction.
--
-- Narrowing also REWRITES the table and REBUILDS every index on it, including
-- positions_pkey, live_results_portfolio_strategy_date_key and
-- signals_portfolio_strategy_name_symbol_timestamp_key. On a production-sized
-- trading.positions that is not instant and it takes an ACCESS EXCLUSIVE lock for the
-- duration. The forward migration is metadata-only; this is not symmetric with it.
--
-- Order:
--   1. Stop every runner. A run that stores a 62-character id while this is in progress
--      will fail, and a run that stores one after it completes will fail at the server.
--   2. Confirm no stored id needs the width:
--        SELECT count(*) FROM trading.positions    WHERE length(strategy_id) > 50;
--        SELECT count(*) FROM trading.live_results WHERE length(strategy_id) > 50;
--        SELECT count(*) FROM trading.signals      WHERE length(strategy_id) > 50;
--      All three must be 0. If they are not, decide what happens to those rows FIRST --
--      this file will refuse rather than choose for you.
--   3. Consider whether validate_strategy_id's 100-character cap (5b9c91d6) should go back
--      to 50 with it. Leaving the cap at 100 with the columns at 50 is exactly the state
--      migration 012 was written to end: the id passes the client check and dies at the
--      server. That state is survivable -- the error is loud and nothing is written -- but
--      it is not a state to sit in deliberately.
--   4. Only then run this file.
--
-- trading.executions.strategy_id is NOT touched here. It was varchar(100) before migration
-- 012 and is unrelated to it.

BEGIN;

DO $$
DECLARE
    tbl   text;
    w     integer;
    over  bigint;
    ex    text;
BEGIN
    FOREACH tbl IN ARRAY ARRAY['positions', 'live_results', 'signals'] LOOP
        SELECT character_maximum_length INTO w
          FROM information_schema.columns
         WHERE table_schema = 'trading' AND table_name = tbl AND column_name = 'strategy_id';

        IF w IS NULL THEN
            RAISE EXCEPTION 'trading.%.strategy_id does not exist, or is not a varchar', tbl;
        ELSIF w = 50 THEN
            RAISE NOTICE 'trading.%.strategy_id is already varchar(50); nothing to do', tbl;
            CONTINUE;
        END IF;

        EXECUTE format('SELECT count(*), min(strategy_id) FROM trading.%I '
                       'WHERE length(strategy_id) > 50', tbl)
           INTO over, ex;
        IF over > 0 THEN
            RAISE EXCEPTION 'trading.% holds % row(s) whose strategy_id is longer than 50 '
                            'characters (for example %). Narrowing would destroy them. '
                            'Decide what happens to those rows before rolling back 012.',
                            tbl, over, ex;
        END IF;

        EXECUTE format('ALTER TABLE trading.%I ALTER COLUMN strategy_id TYPE varchar(50)', tbl);
        RAISE NOTICE 'trading.%.strategy_id narrowed from varchar(%) to varchar(50)', tbl, w;
    END LOOP;
END $$;

COMMENT ON COLUMN trading.positions.strategy_id IS NULL;

COMMIT;

-- Verify the widths are back and no row was lost:
--   SELECT table_name, character_maximum_length FROM information_schema.columns
--    WHERE table_schema='trading' AND column_name='strategy_id'
--      AND table_name IN ('positions','live_results','signals');   -- expect 50, 50, 50
--   -- and the row hashes from 012's verification block, which must still match.
