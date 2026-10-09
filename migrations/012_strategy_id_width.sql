-- 012_strategy_id_width.sql
--
-- Widen trading.positions.strategy_id, trading.live_results.strategy_id and
-- trading.signals.strategy_id from varchar(50) to varchar(100), so the joined futures
-- strategy id a third enabled trend strategy produces can actually be stored (E2-F36 /
-- REG-F9).
--
-- WHY
--
-- The futures runners store a JOINED strategy id: "LIVE_" followed by every enabled trend
-- strategy's name, sorted (live_portfolio.cpp / live_portfolio_conservative.cpp, at the
-- combined_strategy_id construction). Two strategies give 41 characters and everything
-- works. A third gives 62:
--
--     LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST_TREND_FOLLOWING_SLOW
--
-- validate_strategy_id used to cap identifiers at 50 and refuse that id client-side. T-1
-- raised the cap to 100 (5b9c91d6), which is the width trading.executions.strategy_id
-- already had -- so the id now reaches the server, and the server refuses it, because
-- these three columns are still varchar(50):
--
--     ERROR:  value too long for type character varying(50)
--
-- Measured on the scratch copy of production (T-1_CLAIMS_VERIFICATION section 2): direct
-- INSERTs of the 62-character id are rejected by positions, live_results and signals, and
-- accepted by executions. That asymmetry is the whole defect. This migration removes it by
-- bringing the three narrow columns up to the width the fourth already has.
--
-- WHAT IT DOES NOT DO
--
-- Nothing in the engine changes behaviour because of this migration. No id in use today is
-- longer than 41 characters, so no existing row is affected and no run writes anything
-- different. It removes a wall that a THIRD enabled trend strategy would hit -- the
-- tripwire for the five new strategies on the roadmap -- and it is applied ahead of that so
-- the wall is not discovered by a live run.
--
-- SAFETY
--
--   * A varchar(n) -> varchar(m) widening with m > n is metadata-only on PostgreSQL 9.2+
--     (src/backend/commands/tablecmds.c, ATColumnChangeRequiresRewrite): no table rewrite,
--     no index rebuild, no row touched. Every value stays byte-identical.
--   * Indexes and constraints are NOT dropped or recreated. All three columns take part in
--     keys that must survive verbatim:
--       positions_pkey                                       (portfolio_id, strategy_id, strategy_name, date, symbol, portfolio_type)
--       live_results_portfolio_strategy_date_key             (portfolio_id, strategy_id, date)
--       signals_portfolio_strategy_name_symbol_timestamp_key (portfolio_id, strategy_id, strategy_name, symbol, timestamp)
--       idx_positions_strategy_id, idx_signals_strategy, idx_trading_signals_strategy_symbol_time
--     A widening keeps the btree opclass (text_ops via varchar), so none of them is
--     invalidated. The verification block below lists them before and after.
--   * No view depends on these columns (checked: no view in any schema references them).
--   * No foreign key references them, in either direction.
--   * Widening only. Nothing here narrows a column, so nothing can truncate.
--   * Transactional and idempotent: each ALTER is guarded on the current width, so
--     re-running the file is a no-op.
--
-- ORDER AND NUMBERING
--
-- Independent of every other migration: it touches three columns no other migration
-- touches, and it neither reads nor is read by 001-011. Apply it anywhere in the order.
--
-- The number is 012, not the 010 STAGE3_PLAN section 20 predicted. 010 and 011 were still
-- free when that was written; they are now taken by
-- 010_clear_profit_factor_sentinel.sql and 011_live_results_and_executions_columns.sql on
-- origin/qt-platform-preview, alongside 007 (#55), 008 (config-from-database) and 009
-- (preview). 001 through 011 are all occupied somewhere in the interleaved set, so 012 is
-- the next free number and the rule in the plan -- "the next free number after the whole
-- interleaved set" -- is what decides it. Tell the #55 author, as condition 6 requires.
--
-- FUTURES AND EQUITIES REACHABILITY
--
-- Both books write all three tables. Neither moves: every id they write today is well under
-- 50 characters, so no stored value changes and no code path branches on the column's
-- declared width. The A/B for this batch is run BEFORE the migration and is byte-identical;
-- the migration's own evidence is the before/after row hashes in its test script.

BEGIN;

DO $$
DECLARE
    tbl  text;
    w    integer;
BEGIN
    FOREACH tbl IN ARRAY ARRAY['positions', 'live_results', 'signals'] LOOP
        SELECT character_maximum_length INTO w
          FROM information_schema.columns
         WHERE table_schema = 'trading' AND table_name = tbl AND column_name = 'strategy_id';

        IF w IS NULL THEN
            RAISE EXCEPTION 'trading.%.strategy_id does not exist, or is not a varchar', tbl;
        ELSIF w > 100 THEN
            RAISE EXCEPTION 'trading.%.strategy_id is varchar(%), wider than the 100 this '
                            'migration would set: refusing to NARROW a column', tbl, w;
        ELSIF w = 100 THEN
            RAISE NOTICE 'trading.%.strategy_id is already varchar(100); nothing to do', tbl;
        ELSE
            EXECUTE format('ALTER TABLE trading.%I ALTER COLUMN strategy_id TYPE varchar(100)',
                           tbl);
            RAISE NOTICE 'trading.%.strategy_id widened from varchar(%) to varchar(100)', tbl, w;
        END IF;
    END LOOP;
END $$;

COMMENT ON COLUMN trading.positions.strategy_id IS
    'The strategy identifier the row belongs to. For the futures runners this is the JOINED '
    'id -- "LIVE_" followed by every enabled trend strategy name, sorted -- which reaches 62 '
    'characters with three strategies enabled. varchar(100) matches '
    'trading.executions.strategy_id and validate_strategy_id''s cap (E2-F36, migration 012).';

COMMIT;

-- ---------------------------------------------------------------------------
-- VERIFICATION -- run before and after; every line must be identical except the widths.
--
--   -- 1. the widths (the only thing that may change)
--   SELECT table_name, character_maximum_length
--     FROM information_schema.columns
--    WHERE table_schema='trading' AND column_name='strategy_id'
--      AND table_name IN ('positions','live_results','signals','executions')
--    ORDER BY 1;
--   -- after: all four = 100
--
--   -- 2. row counts and a hash of every row, per table
--   SELECT 'positions' t, count(*) n, md5(string_agg(x::text, ';' ORDER BY x::text)) fp
--     FROM (SELECT to_jsonb(p) x FROM trading.positions p) s
--   UNION ALL SELECT 'live_results', count(*), md5(string_agg(x::text, ';' ORDER BY x::text))
--     FROM (SELECT to_jsonb(l) x FROM trading.live_results l) s
--   UNION ALL SELECT 'signals', count(*), md5(string_agg(x::text, ';' ORDER BY x::text))
--     FROM (SELECT to_jsonb(g) x FROM trading.signals g) s;
--   -- after: identical to before, all three
--
--   -- 3. every index and constraint on the three tables
--   SELECT c.relname, i.relname, pg_get_indexdef(i.oid)
--     FROM pg_index x JOIN pg_class c ON c.oid=x.indrelid JOIN pg_class i ON i.oid=x.indexrelid
--     JOIN pg_namespace n ON n.oid=c.relnamespace
--    WHERE n.nspname='trading' AND c.relname IN ('positions','live_results','signals')
--      AND x.indisvalid ORDER BY 1,2;
--   SELECT conrelid::regclass, conname, contype, pg_get_constraintdef(oid)
--     FROM pg_constraint
--    WHERE conrelid::regclass::text IN
--          ('trading.positions','trading.live_results','trading.signals') ORDER BY 1,2;
--   -- after: identical to before, including indisvalid
-- ---------------------------------------------------------------------------
