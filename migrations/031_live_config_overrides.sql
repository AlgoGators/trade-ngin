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
COMMIT;
