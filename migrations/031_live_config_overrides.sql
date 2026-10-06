-- Governed configuration storage. No users or capability grants are enrolled.
-- Requires trade 013 and AlgoLens registry/membership schema. AlgoLens 011 must
-- separately enable config_submit/config_approve BEFORE its config API is enabled.
-- API transaction order: auth users/grants/mappings -> lock_live_config_scope ->
-- native revalidation -> INSERT candidate/activation. See task-2-report.md.
BEGIN;
CREATE TABLE IF NOT EXISTS trading.live_config_versions (
    version_id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    registry_id text NOT NULL REFERENCES trading.strategy_registry(id),
    portfolio_id text NOT NULL CHECK (portfolio_id=upper(btrim(portfolio_id)) AND portfolio_id<>''),
    engine_strategy_id text NOT NULL CHECK (btrim(engine_strategy_id)<>''),
    registry_revision bigint NOT NULL CHECK (registry_revision>=0),
    validator_build text NOT NULL CHECK (btrim(validator_build)<>''),
    -- Digest of the provisioned closed validator bundle (executable + engine/dependency closure).
    validator_sha256 text NOT NULL CHECK (validator_sha256 ~ '^[0-9a-f]{64}$'),
    base_sha256 text NOT NULL CHECK (base_sha256 ~ '^[0-9a-f]{64}$'),
    effective_sha256 text NOT NULL CHECK (effective_sha256 ~ '^[0-9a-f]{64}$'),
    operation text NOT NULL DEFAULT 'override' CHECK (operation IN ('override','reset_to_baseline')),
    changes jsonb NOT NULL CHECK (jsonb_typeof(changes)='object'),
    effective_snapshot jsonb NOT NULL CHECK (jsonb_typeof(effective_snapshot)='object'
        AND effective_snapshot->'snapshot_version'='2'::jsonb),
    previous_version_id uuid REFERENCES trading.live_config_versions(version_id),
    CONSTRAINT live_config_operation_valid CHECK (
        (operation='override' AND changes<>'{}'::jsonb) OR
        (operation='reset_to_baseline' AND changes='{}'::jsonb AND previous_version_id IS NOT NULL
            AND base_sha256=effective_sha256)),
    submitted_by text NOT NULL CHECK (btrim(submitted_by)<>''),
    submitter_person_id text NOT NULL CHECK (btrim(submitter_person_id)<>''),
    reason text NOT NULL CHECK (btrim(reason)<>''),
    submitted_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (version_id,registry_id,portfolio_id,engine_strategy_id)
);
CREATE TABLE IF NOT EXISTS trading.live_config_activations (
    activation_id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    version_id uuid NOT NULL UNIQUE REFERENCES trading.live_config_versions(version_id),
    approved_by text NOT NULL CHECK (btrim(approved_by)<>''),
    approver_person_id text NOT NULL CHECK (btrim(approver_person_id)<>''),
    authority_versions jsonb NOT NULL CHECK (jsonb_typeof(authority_versions)='object'
        AND authority_versions<>'{}'::jsonb),
    reason text NOT NULL CHECK (btrim(reason)<>''),
    activated_at timestamptz NOT NULL DEFAULT clock_timestamp()
);
CREATE TABLE IF NOT EXISTS trading.live_config_active (
    registry_id text NOT NULL,
    portfolio_id text NOT NULL,
    engine_strategy_id text NOT NULL,
    version_id uuid NOT NULL UNIQUE REFERENCES trading.live_config_activations(version_id),
    PRIMARY KEY (registry_id,portfolio_id,engine_strategy_id),
    FOREIGN KEY(version_id,registry_id,portfolio_id,engine_strategy_id)
        REFERENCES trading.live_config_versions(version_id,registry_id,portfolio_id,engine_strategy_id)
);
-- Independent of runtime_attempts: investor and uncontrolled attempts have no
-- runtime intent. This is immutable admission evidence, never proof of completion.
CREATE TABLE IF NOT EXISTS trading.live_config_attempt_selections (
    attempt_id text PRIMARY KEY CHECK (btrim(attempt_id)<>''),
    portfolio_id text NOT NULL CHECK (portfolio_id=upper(btrim(portfolio_id)) AND portfolio_id<>''),
    engine_strategy_id text NOT NULL CHECK (btrim(engine_strategy_id)<>''),
    run_date date NOT NULL,
    engine_build text NOT NULL CHECK (btrim(engine_build)<>''),
    selection jsonb NOT NULL CHECK (jsonb_typeof(selection)='object'),
    config_snapshot jsonb NOT NULL CHECK (jsonb_typeof(config_snapshot)='object'),
    admitted_at timestamptz NOT NULL DEFAULT clock_timestamp()
);
-- Task4 owns side-effect marking, verified recovery and final publication updates.
-- API receives no write privileges on either attempt table.
CREATE TABLE IF NOT EXISTS trading.live_config_attempt_safety (
    attempt_id text PRIMARY KEY REFERENCES trading.live_config_attempt_selections(attempt_id),
    state text NOT NULL DEFAULT 'clean' CHECK (state IN ('clean','unsafe','published','recovered')),
    pre_write_evidence jsonb,
    recovery_evidence jsonb,
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CHECK (state<>'unsafe' OR (pre_write_evidence IS NOT NULL AND jsonb_typeof(pre_write_evidence)='object')),
    CHECK (state<>'recovered' OR (pre_write_evidence IS NOT NULL AND recovery_evidence IS NOT NULL
                                AND jsonb_typeof(pre_write_evidence)='object'
                                AND jsonb_typeof(recovery_evidence)='object'))
);

CREATE OR REPLACE FUNCTION trading.refuse_live_config_mutation()
RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN RAISE EXCEPTION 'live_config_history_immutable'; END $$;
DO $$
DECLARE t text;
BEGIN
    FOREACH t IN ARRAY ARRAY['live_config_versions','live_config_activations','live_config_attempt_selections'] LOOP
        EXECUTE format('DROP TRIGGER IF EXISTS live_config_immutable ON trading.%I',t);
        EXECUTE format('CREATE TRIGGER live_config_immutable BEFORE UPDATE OR DELETE ON trading.%I '
                       'FOR EACH ROW EXECUTE FUNCTION trading.refuse_live_config_mutation()',t);
    END LOOP;
END $$;

-- This function owns no transaction: all locks live until caller commit/rollback.
-- Includes an advisory scope lock so absence of an active row is serialized.
-- Does not require an eligible lifecycle: controlled stop admission handles retired.
CREATE OR REPLACE FUNCTION trading.lock_live_config_scope(p_engine text,p_book text)
RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE
    r trading.strategy_registry%ROWTYPE;
    n integer:=0;
    book text:=upper(btrim(p_book));
    result jsonb;
    investor uuid;
BEGIN
    IF p_engine IS NULL OR btrim(p_engine)='' OR book IS NULL OR book='' THEN
        RAISE EXCEPTION 'live_config_scope_invalid';
    END IF;
    FOR r IN SELECT s.* FROM trading.strategy_registry s WHERE s.strategy_type=p_engine
      AND (upper(btrim(s.portfolio_id))=book OR EXISTS(SELECT 1 FROM trading.strategy_book_memberships m
        WHERE m.strategy_id=s.id AND upper(btrim(m.portfolio_id))=book)) ORDER BY s.id FOR UPDATE OF s
    LOOP
        n:=n+1;
        result:=jsonb_build_object('registry_id',r.id,'registry_revision',r.runtime_revision,
            'investor_book_id',NULL,'portfolio_id',book,'engine_strategy_id',p_engine);
    END LOOP;
    IF n>1 THEN RAISE EXCEPTION 'live_config_scope_ambiguous'; END IF;
    PERFORM pg_advisory_xact_lock(hashtextextended('algolens:qt-book:'||book,0));
    IF to_regclass('trading.investor_books') IS NOT NULL THEN
        PERFORM pg_advisory_xact_lock(hashtextextended('trade-ngin:investor-book:'||book,0));
        EXECUTE 'SELECT b.book_id FROM trading.investor_books b JOIN trading.investor_book_strategies s '
          'ON s.portfolio_id=b.portfolio_id WHERE b.portfolio_id=$1 AND s.strategy_id=$2 '
          'AND b.is_active AND b.model_stream=''system'' FOR UPDATE OF b'
          INTO investor USING book,p_engine;
        IF investor IS NOT NULL THEN
            IF n<>0 THEN RAISE EXCEPTION 'live_config_scope_ambiguous'; END IF;
            result:=jsonb_build_object('registry_id',NULL,'registry_revision',NULL,
                'investor_book_id',investor,'portfolio_id',book,'engine_strategy_id',p_engine);
        END IF;
    END IF;
    IF result IS NULL THEN RAISE EXCEPTION 'live_config_scope_unsupported'; END IF;
    PERFORM pg_advisory_xact_lock(hashtextextended('trade-ngin:live-config:'||p_engine||':'||book,0));
    RETURN result;
END $$;

CREATE OR REPLACE FUNCTION trading.check_live_config_candidate()
RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE scope jsonb; current_version uuid;
BEGIN
    IF upper(btrim(NEW.effective_snapshot->>'portfolio_id')) IS DISTINCT FROM NEW.portfolio_id THEN
        RAISE EXCEPTION 'live_config_snapshot_scope_invalid';
    END IF;
    scope:=trading.lock_live_config_scope(NEW.engine_strategy_id,NEW.portfolio_id);
    IF scope->>'registry_id' IS DISTINCT FROM NEW.registry_id OR
       (scope->>'registry_revision')::bigint IS DISTINCT FROM NEW.registry_revision THEN
        RAISE EXCEPTION 'live_config_scope_changed';
    END IF;
    SELECT version_id INTO current_version FROM trading.live_config_active
      WHERE registry_id=NEW.registry_id AND portfolio_id=NEW.portfolio_id
        AND engine_strategy_id=NEW.engine_strategy_id;
    IF current_version IS DISTINCT FROM NEW.previous_version_id THEN
        RAISE EXCEPTION 'live_config_active_changed';
    END IF;
    RETURN NEW;
END $$;
DROP TRIGGER IF EXISTS live_config_candidate_scope ON trading.live_config_versions;
CREATE TRIGGER live_config_candidate_scope BEFORE INSERT ON trading.live_config_versions
FOR EACH ROW EXECUTE FUNCTION trading.check_live_config_candidate();

-- Only this trigger can move the active pointer. Its owner must be the migration
-- owner, not the API/publisher. No dynamic SQL and a fixed trusted search_path.
-- Authority resolution and exact native revalidation are API responsibilities;
-- callers must retain auth locks through this INSERT. SQL still enforces scope,
-- distinct authenticated/canonical actors, immutable payload and expected pointer.
CREATE OR REPLACE FUNCTION trading.activate_live_config_candidate()
RETURNS trigger LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,trading AS $$
DECLARE v trading.live_config_versions%ROWTYPE; scope jsonb; current_version uuid;
BEGIN
    SELECT * INTO STRICT v FROM trading.live_config_versions WHERE version_id=NEW.version_id;
    scope:=trading.lock_live_config_scope(v.engine_strategy_id,v.portfolio_id);
    IF scope->>'registry_id' IS DISTINCT FROM v.registry_id OR
       (scope->>'registry_revision')::bigint IS DISTINCT FROM v.registry_revision THEN
        RAISE EXCEPTION 'live_config_scope_changed';
    END IF;
    IF NOT EXISTS(SELECT 1 FROM trading.strategy_registry WHERE id=v.registry_id
                  AND lifecycle IN ('live','incubating') AND is_active) THEN
        RAISE EXCEPTION 'live_config_scope_ineligible';
    END IF;
    IF NEW.approved_by=v.submitted_by OR NEW.approver_person_id=v.submitter_person_id THEN
        RAISE EXCEPTION 'live_config_distinct_approver_required';
    END IF;
    SELECT version_id INTO current_version FROM trading.live_config_active
      WHERE registry_id=v.registry_id AND portfolio_id=v.portfolio_id AND engine_strategy_id=v.engine_strategy_id
      FOR UPDATE;
    IF current_version IS DISTINCT FROM v.previous_version_id THEN
        RAISE EXCEPTION 'live_config_active_changed';
    END IF;
    INSERT INTO trading.live_config_active(registry_id,portfolio_id,engine_strategy_id,version_id)
      VALUES(v.registry_id,v.portfolio_id,v.engine_strategy_id,v.version_id)
      ON CONFLICT(registry_id,portfolio_id,engine_strategy_id) DO UPDATE SET version_id=excluded.version_id;
    RETURN NEW;
END $$;
DROP TRIGGER IF EXISTS live_config_activate ON trading.live_config_activations;
CREATE TRIGGER live_config_activate AFTER INSERT ON trading.live_config_activations
FOR EACH ROW EXECUTE FUNCTION trading.activate_live_config_candidate();

CREATE OR REPLACE FUNCTION trading.check_live_config_attempt_selection()
RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE scope jsonb; v trading.live_config_versions%ROWTYPE; selected uuid;
BEGIN
    IF NOT (NEW.selection ?& ARRAY['schema','source','version_id','base_sha256','effective_sha256','scope','engine_build'])
       OR (SELECT count(*) FROM jsonb_object_keys(NEW.selection))<>7
       OR NEW.selection->>'schema' IS DISTINCT FROM 'live-config-selection/v1'
       OR NEW.selection->>'engine_build' IS DISTINCT FROM NEW.engine_build
       OR NOT COALESCE((NEW.selection->>'base_sha256') ~ '^[0-9a-f]{64}$',false)
       OR NOT COALESCE((NEW.selection->>'effective_sha256') ~ '^[0-9a-f]{64}$',false) THEN
        RAISE EXCEPTION 'live_config_receipt_invalid';
    END IF;
    scope:=trading.lock_live_config_scope(NEW.engine_strategy_id,NEW.portfolio_id);
    IF scope IS DISTINCT FROM NEW.selection->'scope' THEN RAISE EXCEPTION 'live_config_scope_changed'; END IF;
    SELECT version_id INTO selected FROM trading.live_config_active
      WHERE portfolio_id=NEW.portfolio_id AND engine_strategy_id=NEW.engine_strategy_id;
    IF selected IS NULL THEN
        IF NEW.selection->>'source' IS DISTINCT FROM 'file' OR NEW.selection->'version_id'<>'null'::jsonb
           OR NEW.selection->>'base_sha256' IS DISTINCT FROM NEW.selection->>'effective_sha256' THEN
            RAISE EXCEPTION 'live_config_active_changed';
        END IF;
    ELSE
        SELECT * INTO STRICT v FROM trading.live_config_versions WHERE version_id=selected;
        IF NEW.selection->>'source' IS DISTINCT FROM 'approved_override'
           OR NEW.selection->>'version_id' IS DISTINCT FROM selected::text
           OR NEW.selection->>'base_sha256' IS DISTINCT FROM v.base_sha256
           OR NEW.selection->>'effective_sha256' IS DISTINCT FROM v.effective_sha256
           OR NEW.engine_build IS DISTINCT FROM v.validator_build
           OR NEW.config_snapshot IS DISTINCT FROM v.effective_snapshot
           OR scope->>'registry_id' IS DISTINCT FROM v.registry_id
           OR (scope->>'registry_revision')::bigint IS DISTINCT FROM v.registry_revision
           OR NOT EXISTS(SELECT 1 FROM trading.runtime_attempts a JOIN trading.runtime_intents i ON i.id=a.intent_id
              WHERE a.id=NEW.attempt_id AND a.config_snapshot=NEW.config_snapshot
                AND a.run_date=NEW.run_date AND a.producer_version=NEW.engine_build
                AND a.registry_revision=v.registry_revision
                AND i.status='approved' AND i.config_snapshot=NEW.config_snapshot
                AND i.registry_id=v.registry_id AND i.portfolio_id=v.portfolio_id
                AND i.engine_strategy_id=v.engine_strategy_id AND i.registry_revision=v.registry_revision) THEN
            RAISE EXCEPTION 'live_config_admission_refused';
        END IF;
    END IF;
    RETURN NEW;
END $$;
DROP TRIGGER IF EXISTS live_config_attempt_scope ON trading.live_config_attempt_selections;
CREATE TRIGGER live_config_attempt_scope BEFORE INSERT ON trading.live_config_attempt_selections
FOR EACH ROW EXECUTE FUNCTION trading.check_live_config_attempt_selection();

REVOKE ALL ON trading.live_config_versions,trading.live_config_activations,trading.live_config_active,
    trading.live_config_attempt_selections,trading.live_config_attempt_safety FROM PUBLIC;
REVOKE ALL ON FUNCTION trading.refuse_live_config_mutation(),trading.check_live_config_candidate(),
    trading.activate_live_config_candidate(),trading.check_live_config_attempt_selection(),
    trading.lock_live_config_scope(text,text) FROM PUBLIC;
-- Exact grants on only these new objects, if provisioned roles exist. Reset
-- accidental/default table AND column ACLs on reapply. No role/user enrollment.
DO $$
DECLARE role_name text; table_name text; columns text;
BEGIN
    FOREACH role_name IN ARRAY ARRAY['qt_algolens_api','qt_system_publisher','qt_worker'] LOOP
        IF EXISTS(SELECT 1 FROM pg_roles WHERE rolname=role_name) THEN
            FOREACH table_name IN ARRAY ARRAY['live_config_versions','live_config_activations','live_config_active',
                'live_config_attempt_selections','live_config_attempt_safety'] LOOP
                SELECT string_agg(quote_ident(attname),',') INTO columns FROM pg_attribute
                  WHERE attrelid=('trading.'||table_name)::regclass AND attnum>0 AND NOT attisdropped;
                EXECUTE format('REVOKE ALL ON trading.%I FROM %I',table_name,role_name);
                EXECUTE format('REVOKE ALL (%s) ON trading.%I FROM %I',columns,table_name,role_name);
            END LOOP;
            EXECUTE format('REVOKE ALL ON FUNCTION trading.refuse_live_config_mutation(),'
                'trading.check_live_config_candidate(),trading.activate_live_config_candidate(),'
                'trading.check_live_config_attempt_selection(),trading.lock_live_config_scope(text,text) FROM %I',role_name);
        END IF;
    END LOOP;
    IF EXISTS(SELECT 1 FROM pg_roles WHERE rolname='qt_algolens_api') THEN
        GRANT SELECT,INSERT ON trading.live_config_versions,trading.live_config_activations TO qt_algolens_api;
        GRANT SELECT ON trading.live_config_active,trading.live_config_attempt_selections TO qt_algolens_api;
        GRANT EXECUTE ON FUNCTION trading.lock_live_config_scope(text,text) TO qt_algolens_api;
    END IF;
    IF EXISTS(SELECT 1 FROM pg_roles WHERE rolname='qt_system_publisher') THEN
        GRANT SELECT ON trading.live_config_versions,trading.live_config_activations,trading.live_config_active,
            trading.live_config_attempt_selections,trading.live_config_attempt_safety TO qt_system_publisher;
        GRANT INSERT ON trading.live_config_attempt_selections TO qt_system_publisher;
        GRANT INSERT(attempt_id) ON trading.live_config_attempt_safety TO qt_system_publisher;
        GRANT EXECUTE ON FUNCTION trading.lock_live_config_scope(text,text) TO qt_system_publisher;
    END IF;
END $$;
-- Task 4: attempt lifecycle is independent from durable side-effect safety.
ALTER TABLE trading.live_config_attempt_safety
    ADD COLUMN IF NOT EXISTS lifecycle text NOT NULL DEFAULT 'running'
        CHECK (lifecycle IN ('running','failed','aborted','published','stopped')),
    ADD COLUMN IF NOT EXISTS publication_id text,
    ADD COLUMN IF NOT EXISTS classification_version integer CHECK (classification_version=2);
-- Old INSERT(attempt_id) callers stay unclassified, including after deployment.
ALTER TABLE trading.live_config_attempt_safety ALTER COLUMN classification_version DROP DEFAULT;
CREATE OR REPLACE FUNCTION trading.initialize_live_config_attempt_v2(p_attempt text)
RETURNS void LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,trading AS $$
DECLARE a trading.live_config_attempt_selections%ROWTYPE;
BEGIN
    SELECT * INTO STRICT a FROM trading.live_config_attempt_selections WHERE attempt_id=p_attempt
      AND xmin::text::bigint=(txid_current() % 4294967296);
    PERFORM trading.lock_live_config_scope(a.engine_strategy_id,a.portfolio_id);
    -- No UPDATE/upsert: a prior old-code safety row can never acquire this fact.
    INSERT INTO trading.live_config_attempt_safety(attempt_id,classification_version) VALUES(p_attempt,2);
END $$;

-- Exact scoped rowset proof, including timestamps and duplicate rows. Store only
-- bounded table counts and SHA-256 of sorted fixed-width SHA-256 row digests.
-- JSONB text encoding is PostgreSQL-native; recovery recomputes in this database.
-- No caller-supplied hash or Boolean can substitute for the database computation.
CREATE OR REPLACE FUNCTION trading.live_config_financial_state(p_engine text,p_book text)
RETURNS jsonb LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,trading SET TimeZone='UTC'
    SET DateStyle='ISO,YMD' SET extra_float_digits=3 SET IntervalStyle='iso_8601' AS $$
DECLARE result jsonb:=jsonb_build_object('schema','live-config-financial-state/v2',
    'engine_strategy_id',p_engine,'portfolio_id',p_book,'scope','all_dates_system_and_shared'); t text; rows jsonb;
BEGIN
    FOREACH t IN ARRAY ARRAY['positions','live_results','equity_curve','executions','signals',
                              'corp_action_applied','risk_limits','live_run_metadata','run_inputs'] LOOP
        IF to_regclass('trading.'||t) IS NULL THEN
            IF t<>'corp_action_applied' THEN RAISE EXCEPTION 'config_recovery_schema_missing'; END IF;
            rows:='null';
        ELSE
            EXECUTE format('SELECT jsonb_build_object(''row_count'',count(*),''sha256'','
                'encode(sha256(convert_to(coalesce(string_agg(h,'''' ORDER BY h COLLATE "C"),''''),''UTF8'')),''hex'')) '
                'FROM (SELECT encode(sha256(convert_to(to_jsonb(r)::text,''UTF8'')),''hex'') AS h FROM trading.%I r '
                'WHERE strategy_id=$1 AND portfolio_id=$2 AND (NOT (to_jsonb(r) ? ''portfolio_type'') '
                'OR to_jsonb(r)->>''portfolio_type''=''system'')) s',t)
                INTO rows USING p_engine,p_book;
        END IF;
        result:=result||jsonb_build_object(t,rows);
    END LOOP;
    RETURN result;
END $$;

CREATE OR REPLACE FUNCTION trading.check_live_config_retry()
RETURNS trigger LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,trading AS $$
DECLARE prior record;
BEGIN
    PERFORM trading.lock_live_config_scope(NEW.engine_strategy_id,NEW.portfolio_id);
    -- Pre-governance failures have no state proof; absence is never a clean fact.
    -- The current controlled attempt was inserted immediately before this trigger.
    IF EXISTS(SELECT 1 FROM trading.runtime_attempts r JOIN trading.runtime_intents i ON i.id=r.intent_id
        LEFT JOIN trading.live_config_attempt_selections a ON a.attempt_id=r.id
        LEFT JOIN trading.live_config_attempt_safety s ON s.attempt_id=a.attempt_id
        WHERE i.engine_strategy_id=NEW.engine_strategy_id AND i.portfolio_id=NEW.portfolio_id
          AND r.id<>NEW.attempt_id AND r.status IN ('running','failed')
          AND (a.attempt_id IS NULL OR s.attempt_id IS NULL)) THEN
        RAISE EXCEPTION 'config_retry_recovery_required';
    END IF;
    IF EXISTS(SELECT 1 FROM trading.live_config_attempt_selections a
        LEFT JOIN trading.live_config_attempt_safety s USING(attempt_id)
        WHERE a.engine_strategy_id=NEW.engine_strategy_id AND a.portfolio_id=NEW.portfolio_id
          AND (s.attempt_id IS NULL OR s.classification_version IS DISTINCT FROM 2)) THEN RAISE EXCEPTION 'config_retry_recovery_required'; END IF;
    FOR prior IN SELECT a.*,s.lifecycle,s.state FROM trading.live_config_attempt_selections a
        JOIN trading.live_config_attempt_safety s USING(attempt_id)
        WHERE a.engine_strategy_id=NEW.engine_strategy_id AND a.portfolio_id=NEW.portfolio_id
          AND (s.lifecycle='running' OR s.state='unsafe' OR (a.run_date=NEW.run_date AND s.lifecycle='published'))
        ORDER BY a.admitted_at FOR UPDATE OF s LOOP
        IF prior.lifecycle='running' THEN RAISE EXCEPTION 'config_attempt_unresolved'; END IF;
        IF prior.run_date=NEW.run_date AND prior.lifecycle='published' THEN
            RAISE EXCEPTION 'config_day_completed';
        END IF;
        IF prior.state='unsafe' THEN
            RAISE EXCEPTION 'config_retry_recovery_required';
        END IF;
    END LOOP;
    -- Older runs have no classified attempt history: never rewrite their inputs.
    IF EXISTS(SELECT 1 FROM trading.run_inputs WHERE strategy_id=NEW.engine_strategy_id
        AND portfolio_id=NEW.portfolio_id AND date=NEW.run_date) THEN
        RAISE EXCEPTION 'config_day_completed';
    END IF;
    RETURN NEW;
END $$;
DROP TRIGGER IF EXISTS live_config_retry_guard ON trading.live_config_attempt_selections;
CREATE TRIGGER live_config_retry_guard BEFORE INSERT ON trading.live_config_attempt_selections
FOR EACH ROW EXECUTE FUNCTION trading.check_live_config_retry();

CREATE OR REPLACE FUNCTION trading.assert_live_config_running(p_attempt text)
RETURNS void LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,trading AS $$
DECLARE a trading.live_config_attempt_selections%ROWTYPE; s trading.live_config_attempt_safety%ROWTYPE;
BEGIN
    SELECT * INTO STRICT a FROM trading.live_config_attempt_selections WHERE attempt_id=p_attempt;
    PERFORM trading.lock_live_config_scope(a.engine_strategy_id,a.portfolio_id);
    SELECT * INTO STRICT s FROM trading.live_config_attempt_safety WHERE attempt_id=p_attempt FOR UPDATE;
    IF s.classification_version IS DISTINCT FROM 2 THEN RAISE EXCEPTION 'config_retry_recovery_required'; END IF;
    IF s.lifecycle<>'running' THEN RAISE EXCEPTION 'config_attempt_not_running'; END IF;
END $$;

CREATE OR REPLACE FUNCTION trading.mark_live_config_unsafe(p_attempt text)
RETURNS void LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,trading AS $$
DECLARE a trading.live_config_attempt_selections%ROWTYPE;
BEGIN
    PERFORM trading.assert_live_config_running(p_attempt);
    SELECT * INTO STRICT a FROM trading.live_config_attempt_selections WHERE attempt_id=p_attempt;
    UPDATE trading.live_config_attempt_safety SET state='unsafe',
        pre_write_evidence=trading.live_config_financial_state(a.engine_strategy_id,a.portfolio_id),
        updated_at=clock_timestamp() WHERE attempt_id=p_attempt AND state='clean';
END $$;

CREATE OR REPLACE FUNCTION trading.finish_live_config_attempt(p_attempt text,p_outcome text,p_publication text DEFAULT NULL)
RETURNS void LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,trading AS $$
DECLARE a trading.live_config_attempt_selections%ROWTYPE; capture jsonb;
BEGIN
    PERFORM trading.assert_live_config_running(p_attempt);
    IF p_outcome NOT IN ('failed','published','stopped') OR
       (p_outcome='published' AND (p_publication IS NULL OR btrim(p_publication)='')) THEN
        RAISE EXCEPTION 'config_attempt_outcome_invalid';
    END IF;
    IF p_outcome='stopped' AND (NOT EXISTS(SELECT 1 FROM trading.runtime_attempts
        WHERE id=p_attempt AND status='applied' AND outcome='stopped') OR
        EXISTS(SELECT 1 FROM trading.live_config_attempt_safety WHERE attempt_id=p_attempt AND state<>'clean')) THEN
        RAISE EXCEPTION 'config_stop_evidence_missing';
    END IF;
    IF p_outcome='published' THEN
        SELECT * INTO STRICT a FROM trading.live_config_attempt_selections WHERE attempt_id=p_attempt;
        SELECT portfolio_config->'config_inspection' INTO capture FROM trading.live_run_metadata
            WHERE strategy_id=a.engine_strategy_id AND portfolio_id=a.portfolio_id AND date=a.run_date;
        IF p_publication<>p_attempt OR capture IS NULL OR
           capture->'identity'->>'publication_id' IS DISTINCT FROM p_publication OR
           capture->'identity'->>'config_attempt_id' IS DISTINCT FROM p_attempt OR
           capture->'configuration_selection' IS DISTINCT FROM
               (a.selection-'schema'-'scope'-'engine_build') OR
           capture->'publication_schema_version' NOT IN ('4'::jsonb,'5'::jsonb) OR
           NOT EXISTS(SELECT 1 FROM trading.run_inputs WHERE strategy_id=a.engine_strategy_id
                AND portfolio_id=a.portfolio_id AND date=a.run_date AND config_snapshot=a.config_snapshot) THEN
            RAISE EXCEPTION 'config_publication_evidence_missing';
        END IF;
    END IF;
    UPDATE trading.live_config_attempt_safety SET lifecycle=p_outcome,
        state=CASE WHEN p_outcome='published' THEN 'published' ELSE state END,
        publication_id=p_publication,updated_at=clock_timestamp() WHERE attempt_id=p_attempt;
END $$;

-- Governed investor publication reuses the frozen identity; legacy 4-arg API unchanged.
CREATE OR REPLACE FUNCTION trading.publish_system_investor_day(
    p_portfolio_id text,
    p_strategy_id text,
    p_source_day date,
    p_producer_version text,
    p_publication_id uuid
) RETURNS uuid LANGUAGE plpgsql AS $$
DECLARE
    matched_book_id uuid;
    calculated_digest text;
    existing record;
    created_id uuid;
BEGIN
    IF p_source_day IS NULL OR p_producer_version IS NULL
       OR btrim(p_producer_version) = '' THEN
        RAISE EXCEPTION 'investor_publication_input_invalid';
    END IF;
    matched_book_id := trading.lock_investor_publication_scope(
        p_strategy_id, p_portfolio_id);
    IF p_source_day < (SELECT opening_date FROM trading.investor_books
                       WHERE book_id = matched_book_id) THEN
        RAISE EXCEPTION 'investor_publication_precedes_opening_date';
    END IF;
    calculated_digest := trading.compute_system_investor_digest(
        upper(btrim(p_portfolio_id)), p_strategy_id, p_source_day);

    SELECT * INTO existing FROM trading.investor_book_publications
     WHERE portfolio_id = upper(btrim(p_portfolio_id))
       AND source_day = p_source_day
     FOR UPDATE;
    IF FOUND THEN
        IF (p_publication_id IS NOT NULL AND existing.publication_id IS DISTINCT FROM p_publication_id)
           OR existing.strategy_id IS DISTINCT FROM p_strategy_id
           OR existing.model_stream IS DISTINCT FROM 'system'
           OR existing.content_digest IS DISTINCT FROM calculated_digest THEN
            RAISE EXCEPTION 'investor_publication_conflict';
        END IF;
        RETURN existing.publication_id;
    END IF;

    INSERT INTO trading.investor_book_publications
        (publication_id, book_id, portfolio_id, source_day, strategy_id, model_stream,
         content_digest, producer_id, producer_version)
    VALUES
        (coalesce(p_publication_id,gen_random_uuid()), matched_book_id, upper(btrim(p_portfolio_id)), p_source_day,
         p_strategy_id, 'system', calculated_digest, 'trade-ngin',
         p_producer_version)
    RETURNING publication_id INTO created_id;
    RETURN created_id;
END $$;

-- Compatibility wrapper preserves historical generated-ID/idempotent behavior.
CREATE OR REPLACE FUNCTION trading.publish_system_investor_day(
    p_portfolio_id text,p_strategy_id text,p_source_day date,p_producer_version text
) RETURNS uuid LANGUAGE sql AS $$
    SELECT trading.publish_system_investor_day(p_portfolio_id,p_strategy_id,p_source_day,p_producer_version,NULL::uuid)
$$;

-- Restricted recovery/administration only: no API/publisher EXECUTE grant.
-- It fences the original process by ending its lifecycle under the same locks.
CREATE OR REPLACE FUNCTION trading.recover_live_config_attempt(p_attempt text,p_reason text)
RETURNS void LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,trading AS $$
DECLARE a trading.live_config_attempt_selections%ROWTYPE; s trading.live_config_attempt_safety%ROWTYPE;
        current_state jsonb;
BEGIN
    IF p_reason IS NULL OR btrim(p_reason)='' THEN RAISE EXCEPTION 'config_recovery_reason_required'; END IF;
    SELECT * INTO STRICT a FROM trading.live_config_attempt_selections WHERE attempt_id=p_attempt;
    PERFORM trading.lock_live_config_scope(a.engine_strategy_id,a.portfolio_id);
    SELECT * INTO STRICT s FROM trading.live_config_attempt_safety WHERE attempt_id=p_attempt FOR UPDATE;
    IF s.classification_version IS DISTINCT FROM 2 THEN RAISE EXCEPTION 'config_recovery_evidence_missing'; END IF;
    IF s.lifecycle IN ('published','stopped','aborted') OR s.state IN ('published','recovered') THEN
        RAISE EXCEPTION 'config_recovery_not_eligible';
    END IF;
    current_state:=trading.live_config_financial_state(a.engine_strategy_id,a.portfolio_id);
    IF s.state='unsafe' AND current_state IS DISTINCT FROM s.pre_write_evidence THEN
        RAISE EXCEPTION 'config_recovery_state_mismatch';
    END IF;
    UPDATE trading.live_config_attempt_safety SET lifecycle='aborted',
        state=CASE WHEN state='unsafe' THEN 'recovered' ELSE state END,
        recovery_evidence=jsonb_build_object('schema','live-config-recovery/v1','state',current_state,
            'reason',p_reason,'actor',session_user,'verified_at',clock_timestamp()),
        updated_at=clock_timestamp() WHERE attempt_id=p_attempt;
    UPDATE trading.runtime_attempts SET status='failed',failure_code='config_attempt_recovered',
        finished_at=clock_timestamp() WHERE id=p_attempt AND status='running';
END $$;
REVOKE ALL ON FUNCTION trading.initialize_live_config_attempt_v2(text),trading.live_config_financial_state(text,text),trading.check_live_config_retry(),
    trading.assert_live_config_running(text),trading.mark_live_config_unsafe(text),
    trading.finish_live_config_attempt(text,text,text),trading.recover_live_config_attempt(text,text),
    trading.publish_system_investor_day(text,text,date,text,uuid) FROM PUBLIC;
DO $$ DECLARE role_name text;
BEGIN
    FOREACH role_name IN ARRAY ARRAY['qt_algolens_api','qt_system_publisher','qt_worker'] LOOP
        IF EXISTS(SELECT 1 FROM pg_roles WHERE rolname=role_name) THEN
            EXECUTE format('REVOKE ALL ON FUNCTION trading.initialize_live_config_attempt_v2(text),trading.live_config_financial_state(text,text),'
                'trading.check_live_config_retry(),trading.assert_live_config_running(text),'
                'trading.mark_live_config_unsafe(text),trading.finish_live_config_attempt(text,text,text),'
                'trading.recover_live_config_attempt(text,text),trading.publish_system_investor_day(text,text,date,text,uuid) FROM %I',role_name);
        END IF;
    END LOOP;
    IF EXISTS(SELECT 1 FROM pg_roles WHERE rolname='qt_system_publisher') THEN
        GRANT EXECUTE ON FUNCTION trading.initialize_live_config_attempt_v2(text),trading.publish_system_investor_day(text,text,date,text,uuid),trading.assert_live_config_running(text),trading.mark_live_config_unsafe(text),
            trading.finish_live_config_attempt(text,text,text) TO qt_system_publisher;
    END IF;
END $$;

COMMIT;
