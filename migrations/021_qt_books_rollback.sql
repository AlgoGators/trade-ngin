-- 021_qt_books_rollback.sql
--
-- Rollback for 021. Restores the pre-021 keys of trading.executions and trading.live_results
-- (under the names they carry), drops their portfolio_type columns and CHECKs, drops
-- trading.positions.moved_by, and narrows the positions and equity_curve CHECK back to
-- ('system','qt').
--
-- REFUSES while any row would be lost or made invalid by it: an executions or live_results row
-- of a book other than system (the column that tells it apart is dropped, and it would collide
-- with the system row under the old key), a qt_proposal row in positions or equity_curve (the
-- narrowed CHECK forbids it), or a positions row carrying moved_by. Delete those rows
-- deliberately first if that is genuinely intended; there is no force switch, because unlike a
-- recomputable column these rows are the desk's record.
--
-- qt rows in positions and equity_curve are untouched: 001 allowed them.
--
-- Transactional and idempotent: on a database without 021 it changes nothing.

BEGIN;

DO $$
DECLARE
    n_exec bigint := 0; n_live bigint := 0; n_pos bigint := 0; n_eq bigint := 0; n_moved bigint := 0;
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = 'trading'
               AND table_name = 'executions' AND column_name = 'portfolio_type') THEN
        EXECUTE 'SELECT count(*) FROM trading.executions WHERE portfolio_type <> ''system''' INTO n_exec;
    END IF;
    IF EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = 'trading'
               AND table_name = 'live_results' AND column_name = 'portfolio_type') THEN
        EXECUTE 'SELECT count(*) FROM trading.live_results WHERE portfolio_type <> ''system''' INTO n_live;
    END IF;
    SELECT count(*) INTO n_pos FROM trading.positions WHERE portfolio_type = 'qt_proposal';
    SELECT count(*) INTO n_eq FROM trading.equity_curve WHERE portfolio_type = 'qt_proposal';
    IF EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = 'trading'
               AND table_name = 'positions' AND column_name = 'moved_by') THEN
        EXECUTE 'SELECT count(*) FROM trading.positions WHERE moved_by IS NOT NULL' INTO n_moved;
    END IF;
    IF n_exec + n_live + n_pos + n_eq + n_moved > 0 THEN
        RAISE EXCEPTION '021 rollback refused: % non-system executions row(s), % non-system live_results row(s), '
                        '% qt_proposal positions row(s), % qt_proposal equity_curve row(s), % positions row(s) '
                        'with moved_by. Delete them deliberately first if that is genuinely intended.',
                        n_exec, n_live, n_pos, n_eq, n_moved;
    END IF;
END $$;

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

-- The keys first: dropping the column would silently drop the six- and four-column keys with it.
DO $$
DECLARE k text;
BEGIN
    k := pg_temp.qt021_key('executions', ARRAY['portfolio_id','strategy_id','strategy_name','date','exec_id','portfolio_type']);
    IF k IS NOT NULL THEN
        EXECUTE format('ALTER TABLE trading.executions DROP CONSTRAINT %I', k);
        EXECUTE format('ALTER TABLE trading.executions ADD CONSTRAINT %I PRIMARY KEY '
                       '(portfolio_id, strategy_id, strategy_name, date, exec_id)', k);
    END IF;
    k := pg_temp.qt021_key('live_results', ARRAY['portfolio_id','strategy_id','date','portfolio_type']);
    IF k IS NOT NULL THEN
        EXECUTE format('ALTER TABLE trading.live_results DROP CONSTRAINT %I', k);
        EXECUTE format('ALTER TABLE trading.live_results ADD CONSTRAINT %I UNIQUE '
                       '(portfolio_id, strategy_id, date)', k);
    END IF;
END $$;

ALTER TABLE trading.executions DROP CONSTRAINT IF EXISTS executions_portfolio_type_check;
ALTER TABLE trading.executions DROP COLUMN IF EXISTS portfolio_type;
ALTER TABLE trading.live_results DROP CONSTRAINT IF EXISTS live_results_portfolio_type_check;
ALTER TABLE trading.live_results DROP COLUMN IF EXISTS portfolio_type;

ALTER TABLE trading.positions DROP CONSTRAINT IF EXISTS positions_moved_by_qt_only;
ALTER TABLE trading.positions DROP COLUMN IF EXISTS moved_by;

ALTER TABLE trading.positions DROP CONSTRAINT IF EXISTS positions_portfolio_type_check;
ALTER TABLE trading.positions ADD CONSTRAINT positions_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt'));
ALTER TABLE trading.equity_curve DROP CONSTRAINT IF EXISTS equity_curve_portfolio_type_check;
ALTER TABLE trading.equity_curve ADD CONSTRAINT equity_curve_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt'));

COMMIT;
