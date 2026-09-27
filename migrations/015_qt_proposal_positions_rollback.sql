-- Refuse downgrade while any proposal identity exists; never discard user edits.
BEGIN;
LOCK TABLE trading.positions, trading.risk_limits, trading.live_results,
    trading.equity_curve, trading.executions, trading.signals,
    trading.live_run_metadata, trading.run_inputs IN ACCESS EXCLUSIVE MODE;
DO $preflight$
DECLARE
    p oid := to_regclass('trading.positions');
    f oid := to_regprocedure('trading.fence_runtime_publication_row()');
    actual_check text;
    new_check constant text := 'CHECK((portfolio_type=ANY(ARRAY[''system''::text,''qt''::text,''benchmark''::text,''benchmark_rebench''::text,''benchmark_frozen_shadow''::text,''qt_proposal''::text])))';
    attached text[];
    key_columns text[];
BEGIN
    IF p IS NULL OR f IS NULL THEN RAISE EXCEPTION 'proposal015 rollback prerequisites missing'; END IF;
    SELECT regexp_replace(pg_get_constraintdef(oid),'\s','','g') INTO actual_check
      FROM pg_constraint WHERE conrelid=p AND conname='positions_portfolio_type_check'
      AND contype='c' AND convalidated;
    IF actual_check IS DISTINCT FROM new_check
      OR (SELECT count(*) FROM pg_constraint WHERE conrelid=p AND conname='positions_portfolio_type_check')<>1
    THEN RAISE EXCEPTION 'proposal015 rollback constraint unsupported'; END IF;
    IF NOT EXISTS (
      SELECT 1 FROM pg_attribute a JOIN pg_attrdef d ON d.adrelid=a.attrelid AND d.adnum=a.attnum
      WHERE a.attrelid=p AND a.attname='portfolio_type' AND a.attnotnull
        AND pg_get_expr(d.adbin,d.adrelid)='''system''::text')
    THEN RAISE EXCEPTION 'proposal015 rollback stream shape unsupported'; END IF;
    SELECT array_agg(a.attname ORDER BY key_order.ordinality) INTO key_columns
      FROM pg_constraint c, unnest(c.conkey) WITH ORDINALITY key_order(attnum,ordinality)
      JOIN pg_attribute a ON a.attrelid=p AND a.attnum=key_order.attnum
      WHERE c.conrelid=p AND c.contype='p' AND c.conname='positions_pkey';
    IF key_columns IS DISTINCT FROM ARRAY['portfolio_id','strategy_id','strategy_name','date','symbol','portfolio_type']
    THEN RAISE EXCEPTION 'proposal015 rollback key unsupported'; END IF;
    IF NOT EXISTS (SELECT 1 FROM pg_proc p JOIN pg_language l ON l.oid=p.prolang
      WHERE p.oid=f AND p.prorettype='pg_catalog.trigger'::regtype
        AND p.pronargs=0 AND l.lanname='plpgsql'
        AND md5(p.prosrc)='419771cec97836560ae952c120aac4d1')
    THEN RAISE EXCEPTION 'proposal015 rollback fence unsupported'; END IF;
    SELECT array_agg(n.nspname||'.'||r.relname ORDER BY n.nspname,r.relname) INTO attached
      FROM pg_trigger t JOIN pg_class r ON r.oid=t.tgrelid
      JOIN pg_namespace n ON n.oid=r.relnamespace
      WHERE t.tgfoid=f AND NOT t.tgisinternal
        AND t.tgname='runtime_publication_fence' AND t.tgenabled='O' AND t.tgtype=31
        AND t.tgqual IS NULL AND t.tgattr::text='' AND t.tgnargs=0
        AND octet_length(t.tgargs)=0 AND t.tgconstraint=0
        AND NOT t.tgdeferrable AND NOT t.tginitdeferred AND t.tgparentid=0
        AND t.tgoldtable IS NULL AND t.tgnewtable IS NULL;
    IF attached IS DISTINCT FROM ARRAY[
      'trading.equity_curve','trading.executions','trading.live_results',
      'trading.live_run_metadata','trading.positions','trading.risk_limits',
      'trading.run_inputs','trading.signals']
      OR (SELECT count(*) FROM pg_trigger WHERE tgfoid=f AND NOT tgisinternal)<>8
    THEN RAISE EXCEPTION 'proposal015 rollback trigger shape unsupported'; END IF;
    IF EXISTS (SELECT 1 FROM trading.positions WHERE portfolio_type='qt_proposal')
    THEN RAISE EXCEPTION 'proposal015 rollback refused: proposal rows exist'; END IF;
    IF EXISTS (SELECT 1 FROM pg_depend d JOIN pg_constraint c ON c.oid=d.refobjid
      WHERE d.refclassid='pg_constraint'::regclass AND c.conrelid=p
      AND c.conname='positions_portfolio_type_check' AND d.deptype NOT IN ('i','a'))
    THEN RAISE EXCEPTION 'proposal015 rollback constraint dependencies unsupported'; END IF;
END $preflight$;

ALTER TABLE trading.positions DROP CONSTRAINT positions_portfolio_type_check;
ALTER TABLE trading.positions ADD CONSTRAINT positions_portfolio_type_check
    CHECK (portfolio_type IN ('system','qt','benchmark','benchmark_rebench','benchmark_frozen_shadow'));

CREATE OR REPLACE FUNCTION trading.fence_runtime_publication_row()
RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE row_value jsonb;
DECLARE stream text;
BEGIN
    row_value := CASE WHEN TG_OP = 'DELETE' THEN to_jsonb(OLD) ELSE to_jsonb(NEW) END;
    stream := row_value->>'portfolio_type';
    IF TG_OP = 'UPDATE' AND
       (to_jsonb(OLD)->'strategy_id',to_jsonb(OLD)->'portfolio_id',to_jsonb(OLD)->'portfolio_type')
       IS DISTINCT FROM
       (to_jsonb(NEW)->'strategy_id',to_jsonb(NEW)->'portfolio_id',to_jsonb(NEW)->'portfolio_type') THEN
        RAISE EXCEPTION 'runtime_scope_move_unsupported';
    END IF;
    IF stream IS NOT NULL AND stream NOT IN
       ('system','qt','benchmark','benchmark_rebench','benchmark_frozen_shadow') THEN
        RAISE EXCEPTION 'runtime_stream_unsupported';
    END IF;
    IF TG_TABLE_NAME IN ('positions','equity_curve') AND
       stream IN ('benchmark','benchmark_rebench','benchmark_frozen_shadow') THEN
        -- Replay uses its historical snapshot, but still serializes the book.
        PERFORM pg_advisory_xact_lock(hashtextextended(
            'algolens:qt-book:' || upper(btrim(row_value->>'portfolio_id')),0));
    ELSE
        PERFORM trading.lock_runtime_scope(row_value->>'strategy_id',row_value->>'portfolio_id',
                                          TG_TABLE_NAME = 'positions' AND stream = 'qt');
    END IF;
    IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
    RETURN NEW;
END $$;
COMMIT;
