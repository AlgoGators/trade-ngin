-- Local upgrade artifact only. Audited binary and schema must roll out together.
-- No historical quantity/book inference, deduplication, trigger bypass or CASCADE.
BEGIN;
DO $migration$
DECLARE
    rel regclass;
    relname text;
    def text;
    stream_exists boolean;
    col record;
BEGIN
    IF to_regclass('trading.executions') IS NULL OR to_regclass('trading.live_results') IS NULL THEN
        RAISE EXCEPTION '014 requires executions and live_results';
    END IF;
    LOCK TABLE trading.executions, trading.live_results IN ACCESS EXCLUSIVE MODE;
    FOR relname IN SELECT unnest(ARRAY['executions','live_results']) LOOP
        rel := ('trading.' || relname)::regclass;
        IF (SELECT relkind FROM pg_class WHERE oid=rel) <> 'r' THEN
            RAISE EXCEPTION '014 requires ordinary tables';
        END IF;
        IF EXISTS (SELECT 1 FROM pg_constraint WHERE contype='f' AND (conrelid=rel OR confrelid=rel)) OR
           EXISTS (SELECT 1 FROM pg_rewrite WHERE ev_class=rel AND rulename<>'_RETURN') THEN
            RAISE EXCEPTION '014 unreviewed foreign key or rewrite dependency';
        END IF;
        IF EXISTS (SELECT 1 FROM pg_index WHERE indrelid=rel AND
            (NOT indisvalid OR NOT indisready OR indpred IS NOT NULL OR indexprs IS NOT NULL OR
             (indisunique AND indexrelid NOT IN
                (SELECT conindid FROM pg_constraint WHERE conrelid=rel AND contype IN ('p','u'))))) THEN
            RAISE EXCEPTION '014 unreviewed index shape';
        END IF;
        IF EXISTS (SELECT 1 FROM pg_constraint WHERE conrelid=rel AND
            (NOT convalidated OR contype NOT IN ('p','u','c') OR
             (contype IN ('p','u') AND NOT
                ((relname='executions' AND conname='executions_pkey') OR
                 (relname='live_results' AND conname IN ('live_results_pkey','live_results_portfolio_strategy_date_key')))) OR
             (contype='c' AND conname NOT IN ('chk_executions_quantity','chk_executions_side',
                'executions_stream_check','executions_qt_book_check',
                'live_results_stream_check','live_results_qt_book_check')))) THEN
            RAISE EXCEPTION '014 unreviewed constraint';
        END IF;
        FOR col IN SELECT * FROM (VALUES ('portfolio_id'),('strategy_id'),('date')) AS required(name) LOOP
            IF NOT EXISTS (SELECT 1 FROM pg_attribute WHERE attrelid=rel AND attname=col.name
                AND attnum>0 AND NOT attisdropped AND
                ((col.name='date' AND atttypid='date'::regtype) OR
                 (col.name<>'date' AND atttypid IN ('text'::regtype,'varchar'::regtype)))) THEN
                RAISE EXCEPTION '014 missing/incompatible required column %.%',relname,col.name;
            END IF;
        END LOOP;
        IF (SELECT count(*) FROM pg_attribute WHERE attrelid=rel AND attname IN ('strategy_id','date') AND attnotnull)<>2 THEN
            RAISE EXCEPTION '014 strategy id and date must be non-null';
        END IF;
        SELECT EXISTS(SELECT 1 FROM pg_attribute WHERE attrelid=rel AND attname='portfolio_type'
                      AND attnum>0 AND NOT attisdropped) INTO stream_exists;
        IF stream_exists THEN
            IF NOT EXISTS (SELECT 1 FROM pg_attribute a JOIN pg_attrdef d ON d.adrelid=a.attrelid AND d.adnum=a.attnum
                WHERE a.attrelid=rel AND a.attname='portfolio_type' AND a.atttypid='text'::regtype
                  AND a.attnotnull AND pg_get_expr(d.adbin,d.adrelid)='''system''::text') THEN
                RAISE EXCEPTION '014 incompatible preexisting stream column';
            END IF;
            EXECUTE format('SELECT EXISTS(SELECT 1 FROM %s WHERE portfolio_type IS NULL OR portfolio_type NOT IN (''system'',''qt''))',rel) INTO stream_exists;
            IF stream_exists THEN RAISE EXCEPTION '014 invalid stored stream'; END IF;
        ELSE
            EXECUTE format('ALTER TABLE %s ADD COLUMN portfolio_type text NOT NULL DEFAULT ''system''',rel);
        END IF;
        -- Preserve system NULL books, but never permit unscoped new QT rows.
        def := format('%s_stream_check',relname);
        IF EXISTS (SELECT 1 FROM pg_constraint WHERE conrelid=rel AND conname=def) THEN
            IF (SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid=rel AND conname=def)
                <> 'CHECK ((portfolio_type = ANY (ARRAY[''system''::text, ''qt''::text])))' THEN
                RAISE EXCEPTION '014 incompatible stream check';
            END IF;
        ELSE
            EXECUTE format('ALTER TABLE %s ADD CONSTRAINT %I CHECK (portfolio_type IN (''system'',''qt''))',rel,def);
        END IF;
        def := format('%s_qt_book_check',relname);
        IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conrelid=rel AND conname=def) THEN
            EXECUTE format('ALTER TABLE %s ADD CONSTRAINT %I CHECK (portfolio_type <> ''qt'' OR (portfolio_id IS NOT NULL AND btrim(portfolio_id) <> ''''))',rel,def);
        ELSE
            -- Validate a preexisting named check semantically below, and refuse altered shape.
            IF (SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid=rel AND conname=def)
               NOT IN ('CHECK (((portfolio_type <> ''qt''::text) OR ((portfolio_id IS NOT NULL) AND (btrim(portfolio_id) <> ''''::text))))',
                       'CHECK (((portfolio_type <> ''qt''::text) OR ((portfolio_id IS NOT NULL) AND (btrim((portfolio_id)::text) <> ''''::text))))') THEN
                RAISE EXCEPTION '014 incompatible QT book check';
            END IF;
        END IF;
    END LOOP;
    IF NOT EXISTS (SELECT 1 FROM pg_attribute WHERE attrelid='trading.executions'::regclass
        AND attname='strategy_name' AND atttypid IN ('text'::regtype,'varchar'::regtype) AND attnotnull) OR
       (SELECT count(*) FROM pg_attribute WHERE attrelid='trading.executions'::regclass
        AND attname IN ('order_id','exec_id') AND atttypid='varchar'::regtype AND atttypmod=54 AND attnotnull)<>2 THEN
        RAISE EXCEPTION '014 incompatible execution identity columns';
    END IF;
    SELECT pg_get_constraintdef(oid) INTO def FROM pg_constraint
        WHERE conrelid='trading.executions'::regclass AND conname='executions_pkey' AND contype='p';
    IF def='PRIMARY KEY (portfolio_id, strategy_id, strategy_name, date, exec_id)' THEN
        ALTER TABLE trading.executions DROP CONSTRAINT executions_pkey;
        ALTER TABLE trading.executions ADD CONSTRAINT executions_pkey
            PRIMARY KEY (portfolio_id,strategy_id,strategy_name,date,portfolio_type,exec_id);
    ELSIF def IS DISTINCT FROM 'PRIMARY KEY (portfolio_id, strategy_id, strategy_name, date, portfolio_type, exec_id)' THEN
        RAISE EXCEPTION '014 unexpected executions primary key';
    END IF;
    IF (SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid='trading.live_results'::regclass
        AND conname='live_results_pkey' AND contype='p') IS DISTINCT FROM 'PRIMARY KEY (id)' THEN
        RAISE EXCEPTION '014 unexpected live_results primary key';
    END IF;
    SELECT pg_get_constraintdef(oid) INTO def FROM pg_constraint WHERE conrelid='trading.live_results'::regclass
        AND conname='live_results_portfolio_strategy_date_key' AND contype='u';
    IF def='UNIQUE (portfolio_id, strategy_id, date)' THEN
        ALTER TABLE trading.live_results DROP CONSTRAINT live_results_portfolio_strategy_date_key;
        ALTER TABLE trading.live_results ADD CONSTRAINT live_results_portfolio_strategy_date_key
            UNIQUE(portfolio_id,strategy_id,date,portfolio_type);
    ELSIF def IS DISTINCT FROM 'UNIQUE (portfolio_id, strategy_id, date, portfolio_type)' THEN
        RAISE EXCEPTION '014 unexpected results business key';
    END IF;
    IF to_regclass('trading.executions_stream_order_idx') IS NOT NULL THEN
        IF (SELECT pg_get_indexdef('trading.executions_stream_order_idx'::regclass)) IS DISTINCT FROM
           'CREATE INDEX executions_stream_order_idx ON trading.executions USING btree (portfolio_id, strategy_id, strategy_name, date, portfolio_type, order_id)' THEN
            RAISE EXCEPTION '014 unexpected cleanup index';
        END IF;
    ELSE
        CREATE INDEX executions_stream_order_idx ON trading.executions
            (portfolio_id,strategy_id,strategy_name,date,portfolio_type,order_id);
    END IF;
END;
$migration$;
COMMIT;
