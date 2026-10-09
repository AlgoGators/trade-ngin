-- 021_qt_books.sql
--
-- Three books side by side in every live trading table (QT platform, PR E1; master document
-- rulings 5, 9, 12, 17 and "final trading schema changes"):
--
--   system       the model's answer, written by the model run. Every row stored to date.
--   qt_proposal  what the desk asked for: seeded from system by the model run, edited in AlgoLens.
--   qt           what the engine gave back: what goes out, and what tomorrow's run starts from.
--
-- One column tells the books apart, `portfolio_type`, the column migration 001 put on
-- trading.positions and trading.equity_curve. This migration:
--
--   1. widens the portfolio_type CHECK on trading.positions and trading.equity_curve from
--      ('system','qt') to ('system','qt_proposal','qt');
--   2. adds portfolio_type TEXT NOT NULL DEFAULT 'system', with the same CHECK, to
--      trading.executions and trading.live_results, and puts it last in their keys:
--        executions_pkey                             (portfolio_id, strategy_id, strategy_name,
--                                                     date, exec_id) + portfolio_type
--        the live_results UNIQUE key                 (portfolio_id, strategy_id, date)
--                                                     + portfolio_type
--      The key constraints are found by their columns, not their names (the stage-3 scratch and
--      the test fixtures name the live_results key differently), and are re-created under the
--      name they had;
--   3. adds trading.positions.moved_by TEXT NULL: the one-pass step that moved the symbol
--      between the desk's request (qt_proposal) and what the engine gave back (qt) (ruling 12).
--      A CHECK keeps it NULL on every book but qt.
--
-- WHY THE KEYS. A qt row and a system row for the same portfolio, sleeve, date and exec id (or
-- the same portfolio, strategy and date in live_results) collide on the old keys, so the second
-- book's write fails or, worse, the first book's re-run DELETE takes the other's rows. With the
-- book last in the key the two coexist, and the engine's DELETEs are scoped by book (same PR).
-- Order ids are NOT changed: system ids stay byte-identical, and the book in the key is what
-- keeps two books' executions apart.
--
-- SAFETY
--
--   * ADD COLUMN with a constant DEFAULT is metadata-only on PostgreSQL 11+; every existing row
--     reads 'system', which is what every existing row is. No backfill.
--   * Adding a column to a key only makes it more permissive: no existing row can conflict.
--     The key rebuild re-indexes the table (executions and live_results are small: thousands of
--     rows), inside this transaction.
--   * NULL-able moved_by with no default is metadata-only; every existing row reads NULL.
--   * Guarded: refuses unless migration 001 is applied (positions/equity_curve.portfolio_type
--     exist); refuses an existing portfolio_type or moved_by of another type than text; refuses
--     when the executions primary key or the live_results unique key is not found in either its
--     pre-021 or its post-021 shape.
--   * Transactional and idempotent: a second apply changes nothing.
--
-- DEPLOY ORDERING: ship with the binary of the same commit. That binary names portfolio_type in
-- every read, write and delete of these four tables, so it fails (refuses) against a database
-- without 021; the old binary keeps working against 021 (it writes the default 'system' and its
-- DELETE predicates only ever matched system rows, the only rows a database without a desk has).
--
-- Rollback: migrations/021_qt_books_rollback.sql

BEGIN;

DO $$
DECLARE
    t   text;
    tbl text;
BEGIN
    FOREACH tbl IN ARRAY ARRAY['positions', 'equity_curve', 'executions', 'live_results'] LOOP
        IF to_regclass('trading.' || tbl) IS NULL THEN
            RAISE EXCEPTION 'trading.% does not exist; migration 021 changes it', tbl;
        END IF;
    END LOOP;
    FOREACH tbl IN ARRAY ARRAY['positions', 'equity_curve'] LOOP
        IF NOT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = 'trading'
                       AND table_name = tbl AND column_name = 'portfolio_type') THEN
            RAISE EXCEPTION 'trading.%.portfolio_type does not exist: apply migration 001 first', tbl;
        END IF;
    END LOOP;
    FOREACH tbl IN ARRAY ARRAY['executions', 'live_results'] LOOP
        SELECT data_type INTO t FROM information_schema.columns
         WHERE table_schema = 'trading' AND table_name = tbl AND column_name = 'portfolio_type';
        IF t IS NOT NULL AND t <> 'text' THEN
            RAISE EXCEPTION 'trading.%.portfolio_type exists with type %, not text', tbl, t;
        END IF;
    END LOOP;
    SELECT data_type INTO t FROM information_schema.columns
     WHERE table_schema = 'trading' AND table_name = 'positions' AND column_name = 'moved_by';
    IF t IS NOT NULL AND t <> 'text' THEN
        RAISE EXCEPTION 'trading.positions.moved_by exists with type %, not text', t;
    END IF;
END $$;

-- The key of `tbl` whose column set is exactly `cols` (contype 'p' or 'u'), or NULL.
CREATE OR REPLACE FUNCTION pg_temp.qt021_key(tbl text, cols text[]) RETURNS text
LANGUAGE sql STABLE AS $$
    SELECT c.conname::text
      FROM pg_constraint c
     WHERE c.conrelid = ('trading.' || tbl)::regclass AND c.contype IN ('p', 'u')
       AND (SELECT array_agg(a.attname::text ORDER BY a.attname)
              FROM pg_attribute a
             WHERE a.attrelid = c.conrelid AND a.attnum = ANY (c.conkey))
           = (SELECT array_agg(x ORDER BY x) FROM unnest(cols) x)
     LIMIT 1
$$;

DO $$
BEGIN
    IF pg_temp.qt021_key('executions', ARRAY['portfolio_id','strategy_id','strategy_name','date','exec_id']) IS NULL
       AND pg_temp.qt021_key('executions', ARRAY['portfolio_id','strategy_id','strategy_name','date','exec_id','portfolio_type']) IS NULL THEN
        RAISE EXCEPTION 'trading.executions has no key on (portfolio_id, strategy_id, strategy_name, date, exec_id[, portfolio_type]); refusing';
    END IF;
    IF pg_temp.qt021_key('live_results', ARRAY['portfolio_id','strategy_id','date']) IS NULL
       AND pg_temp.qt021_key('live_results', ARRAY['portfolio_id','strategy_id','date','portfolio_type']) IS NULL THEN
        RAISE EXCEPTION 'trading.live_results has no key on (portfolio_id, strategy_id, date[, portfolio_type]); refusing';
    END IF;
END $$;

-- 1. positions and equity_curve: widen the CHECK.
ALTER TABLE trading.positions DROP CONSTRAINT IF EXISTS positions_portfolio_type_check;
ALTER TABLE trading.positions ADD CONSTRAINT positions_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt_proposal', 'qt'));
ALTER TABLE trading.equity_curve DROP CONSTRAINT IF EXISTS equity_curve_portfolio_type_check;
ALTER TABLE trading.equity_curve ADD CONSTRAINT equity_curve_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt_proposal', 'qt'));

-- 2. executions and live_results: the column, its CHECK, and the key.
ALTER TABLE trading.executions ADD COLUMN IF NOT EXISTS portfolio_type TEXT NOT NULL DEFAULT 'system';
ALTER TABLE trading.executions DROP CONSTRAINT IF EXISTS executions_portfolio_type_check;
ALTER TABLE trading.executions ADD CONSTRAINT executions_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt_proposal', 'qt'));
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS portfolio_type TEXT NOT NULL DEFAULT 'system';
ALTER TABLE trading.live_results DROP CONSTRAINT IF EXISTS live_results_portfolio_type_check;
ALTER TABLE trading.live_results ADD CONSTRAINT live_results_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt_proposal', 'qt'));

DO $$
DECLARE k text;
BEGIN
    k := pg_temp.qt021_key('executions', ARRAY['portfolio_id','strategy_id','strategy_name','date','exec_id']);
    IF k IS NOT NULL THEN
        EXECUTE format('ALTER TABLE trading.executions DROP CONSTRAINT %I', k);
        EXECUTE format('ALTER TABLE trading.executions ADD CONSTRAINT %I PRIMARY KEY '
                       '(portfolio_id, strategy_id, strategy_name, date, exec_id, portfolio_type)', k);
    END IF;
    k := pg_temp.qt021_key('live_results', ARRAY['portfolio_id','strategy_id','date']);
    IF k IS NOT NULL THEN
        EXECUTE format('ALTER TABLE trading.live_results DROP CONSTRAINT %I', k);
        EXECUTE format('ALTER TABLE trading.live_results ADD CONSTRAINT %I UNIQUE '
                       '(portfolio_id, strategy_id, date, portfolio_type)', k);
    END IF;
END $$;

-- 3. positions.moved_by.
ALTER TABLE trading.positions ADD COLUMN IF NOT EXISTS moved_by TEXT;
ALTER TABLE trading.positions DROP CONSTRAINT IF EXISTS positions_moved_by_qt_only;
ALTER TABLE trading.positions ADD CONSTRAINT positions_moved_by_qt_only
    CHECK (moved_by IS NULL OR portfolio_type = 'qt');

COMMENT ON COLUMN trading.executions.portfolio_type IS
    'The book the fill belongs to: system (the model run), qt_proposal (the desk''s request) or qt (what the engine gave back and what goes out). Part of executions_pkey. Every row before migration 021 is system.';
COMMENT ON COLUMN trading.live_results.portfolio_type IS
    'The book the row belongs to: system, qt_proposal or qt (as trading.positions.portfolio_type). Part of the (portfolio_id, strategy_id, date, portfolio_type) key. Every row before migration 021 is system.';
COMMENT ON COLUMN trading.positions.moved_by IS
    'qt rows only: the one-pass step that moved the symbol from the desk''s request (the qt_proposal row) to what the engine gave back (this row); NULL when nothing moved it. NULL on every system and qt_proposal row (CHECK positions_moved_by_qt_only). Ruling 12; migration 021.';

COMMIT;
