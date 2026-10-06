-- Add only the positions proposal stream. This migration contains no data copy.
BEGIN;
LOCK TABLE trading.positions, trading.risk_limits, trading.live_results,
    trading.equity_curve, trading.executions, trading.signals,
    trading.live_run_metadata, trading.run_inputs IN ACCESS EXCLUSIVE MODE;

DO $preflight$
DECLARE
    p oid := to_regclass('trading.positions');
    key_columns text[];
    stream_constraint record;
    actual_check text;
    old_check constant text := 'CHECK((portfolio_type=ANY(ARRAY[''system''::text,''qt''::text,''benchmark''::text,''benchmark_rebench''::text,''benchmark_frozen_shadow''::text])))';
    new_check constant text := 'CHECK((portfolio_type=ANY(ARRAY[''system''::text,''qt''::text,''benchmark''::text,''benchmark_rebench''::text,''benchmark_frozen_shadow''::text,''qt_proposal''::text])))';
    fence_oid oid := to_regprocedure('trading.fence_runtime_publication_row()');
    old_hash constant text := 'a469e2b5cfd77d447383ae1fb78d5e7a';
    new_hash constant text := '419771cec97836560ae952c120aac4d1';
    attached text[];
BEGIN
    IF p IS NULL OR fence_oid IS NULL THEN RAISE EXCEPTION 'proposal015 prerequisites missing'; END IF;
    IF EXISTS (
      SELECT 1 FROM (VALUES
        ('symbol','character varying(20)'),('quantity','numeric(20,6)'),('average_price','numeric(20,6)'),
        ('daily_unrealized_pnl','numeric(20,6)'),('daily_realized_pnl','numeric(20,6)'),
        ('last_update','timestamp with time zone'),('updated_at','timestamp with time zone'),
        ('strategy_id','character varying(50)'),('strategy_name','character varying(100)'),('date','date'),
        ('portfolio_id','character varying(100)'),('portfolio_type','text')) expected(name,kind)
      LEFT JOIN pg_attribute a ON a.attrelid=p AND a.attname=expected.name AND a.attnum>0 AND NOT a.attisdropped
      WHERE a.attnum IS NULL OR format_type(a.atttypid,a.atttypmod)<>expected.kind)
    THEN RAISE EXCEPTION 'proposal015 positions column shape unsupported'; END IF;
    IF NOT EXISTS (
      SELECT 1 FROM pg_attribute a JOIN pg_attrdef d ON d.adrelid=a.attrelid AND d.adnum=a.attnum
      WHERE a.attrelid=p AND a.attname='portfolio_type' AND a.attnotnull
        AND pg_get_expr(d.adbin,d.adrelid)='''system''::text')
    THEN RAISE EXCEPTION 'proposal015 stream default/nullability unsupported'; END IF;
    SELECT array_agg(a.attname ORDER BY key_order.ordinality) INTO key_columns
      FROM pg_constraint c, unnest(c.conkey) WITH ORDINALITY key_order(attnum,ordinality)
      JOIN pg_attribute a ON a.attrelid=p AND a.attnum=key_order.attnum
      WHERE c.conrelid=p AND c.contype='p' AND c.conname='positions_pkey';
    IF key_columns IS DISTINCT FROM ARRAY['portfolio_id','strategy_id','strategy_name','date','symbol','portfolio_type']
       OR (SELECT count(*) FROM pg_constraint WHERE conrelid=p AND contype='p')<>1
    THEN RAISE EXCEPTION 'proposal015 positions key unsupported'; END IF;
    SELECT * INTO stream_constraint FROM pg_constraint
      WHERE conrelid=p AND conname='positions_portfolio_type_check' AND contype='c' AND convalidated;
    IF NOT FOUND THEN RAISE EXCEPTION 'proposal015 stream constraint missing'; END IF;
    actual_check := regexp_replace(pg_get_constraintdef(stream_constraint.oid),'\s','','g');
    IF actual_check NOT IN (old_check,new_check)
       OR (SELECT count(*) FROM pg_constraint WHERE conrelid=p AND conname='positions_portfolio_type_check')<>1
       OR EXISTS (SELECT 1 FROM pg_depend WHERE refclassid='pg_constraint'::regclass
                  AND refobjid=stream_constraint.oid AND deptype NOT IN ('i','a'))
    THEN RAISE EXCEPTION 'proposal015 stream constraint unsupported: %', actual_check; END IF;
    IF EXISTS (SELECT 1 FROM trading.positions WHERE portfolio_type IS NULL OR portfolio_type NOT IN
      ('system','qt','benchmark','benchmark_rebench','benchmark_frozen_shadow','qt_proposal'))
    THEN RAISE EXCEPTION 'proposal015 existing stream unsupported'; END IF;
    IF NOT EXISTS (SELECT 1 FROM pg_proc f JOIN pg_language l ON l.oid=f.prolang
      WHERE f.oid=fence_oid AND f.prorettype='pg_catalog.trigger'::regtype
        AND f.pronargs=0 AND l.lanname='plpgsql'
        AND ((actual_check=old_check AND md5(f.prosrc)=old_hash) OR
             (actual_check=new_check AND md5(f.prosrc)=new_hash)))
    THEN RAISE EXCEPTION 'proposal015 fence/constraint pair unsupported'; END IF;
    SELECT array_agg(n.nspname||'.'||r.relname ORDER BY n.nspname,r.relname) INTO attached
      FROM pg_trigger t JOIN pg_class r ON r.oid=t.tgrelid
      JOIN pg_namespace n ON n.oid=r.relnamespace
      WHERE t.tgfoid=fence_oid AND NOT t.tgisinternal
        AND t.tgname='runtime_publication_fence' AND t.tgenabled='O' AND t.tgtype=31
        AND t.tgqual IS NULL AND t.tgattr::text='' AND t.tgnargs=0
        AND octet_length(t.tgargs)=0 AND t.tgconstraint=0
        AND NOT t.tgdeferrable AND NOT t.tginitdeferred AND t.tgparentid=0
        AND t.tgoldtable IS NULL AND t.tgnewtable IS NULL;
    IF attached IS DISTINCT FROM ARRAY[
      'trading.equity_curve','trading.executions','trading.live_results',
      'trading.live_run_metadata','trading.positions','trading.risk_limits',
      'trading.run_inputs','trading.signals']
      OR (SELECT count(*) FROM pg_trigger WHERE tgfoid=fence_oid AND NOT tgisinternal)<>8
    THEN RAISE EXCEPTION 'proposal015 fence triggers unsupported'; END IF;
END $preflight$;

ALTER TABLE trading.positions DROP CONSTRAINT positions_portfolio_type_check;
ALTER TABLE trading.positions ADD CONSTRAINT positions_portfolio_type_check
    CHECK (portfolio_type IN ('system','qt','benchmark','benchmark_rebench',
                              'benchmark_frozen_shadow','qt_proposal'));

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
       ('system','qt','benchmark','benchmark_rebench','benchmark_frozen_shadow')
       AND NOT (TG_TABLE_SCHEMA = 'trading' AND TG_TABLE_NAME = 'positions' AND stream = 'qt_proposal') THEN
        RAISE EXCEPTION 'runtime_stream_unsupported';
    END IF;
    IF TG_TABLE_NAME IN ('positions','equity_curve') AND
       stream IN ('benchmark','benchmark_rebench','benchmark_frozen_shadow') THEN
        -- Replay uses its historical snapshot, but still serializes the book.
        PERFORM pg_advisory_xact_lock(hashtextextended(
            'algolens:qt-book:' || upper(btrim(row_value->>'portfolio_id')),0));
    ELSE
        PERFORM trading.lock_runtime_scope(row_value->>'strategy_id',row_value->>'portfolio_id',
                                          TG_TABLE_NAME = 'positions' AND stream IN ('qt','qt_proposal'));
    END IF;
    IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
    RETURN NEW;
END $$;
COMMIT;
