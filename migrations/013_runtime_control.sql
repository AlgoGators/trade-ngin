-- Local controlled-runtime contract. Does not enable execution or grant roles.
-- Requires the existing AlgoLens registry/lifecycle and membership migrations.
BEGIN;

DO $$
DECLARE relation_name text;
BEGIN
    FOREACH relation_name IN ARRAY ARRAY['positions','risk_limits','live_results','equity_curve',
                                         'executions','signals','live_run_metadata','run_inputs'] LOOP
        IF to_regclass('trading.' || relation_name) IS NULL OR
           (SELECT count(*) FROM information_schema.columns WHERE table_schema='trading'
              AND table_name=relation_name AND column_name IN ('strategy_id','portfolio_id')) <> 2 THEN
            RAISE EXCEPTION 'runtime migration prerequisites missing: %', relation_name;
        END IF;
    END LOOP;
    IF NOT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema='trading'
                   AND table_name='positions' AND column_name='portfolio_type') THEN
        RAISE EXCEPTION 'runtime migration requires position streams';
    END IF;
END $$;

ALTER TABLE trading.strategy_registry
    ADD COLUMN IF NOT EXISTS runtime_revision BIGINT NOT NULL DEFAULT 0;

CREATE OR REPLACE FUNCTION trading.bump_runtime_registry_revision()
RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF (NEW.lifecycle, NEW.is_active, NEW.strategy_type, NEW.portfolio_id)
       IS DISTINCT FROM
       (OLD.lifecycle, OLD.is_active, OLD.strategy_type, OLD.portfolio_id) THEN
        NEW.runtime_revision := OLD.runtime_revision + 1;
    ELSIF NEW.runtime_revision NOT IN (OLD.runtime_revision, OLD.runtime_revision + 1) THEN
        RAISE EXCEPTION 'invalid runtime revision';
    END IF;
    RETURN NEW;
END $$;
DROP TRIGGER IF EXISTS runtime_registry_revision ON trading.strategy_registry;
CREATE TRIGGER runtime_registry_revision BEFORE UPDATE ON trading.strategy_registry
FOR EACH ROW EXECUTE FUNCTION trading.bump_runtime_registry_revision();

CREATE OR REPLACE FUNCTION trading.bump_runtime_membership_revision()
RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE identity text;
BEGIN
    -- Callers must lock all parent registry rows before editing membership.
    -- Sorted parent updates also handle a deliberate cross-registry move.
    FOR identity IN
        SELECT DISTINCT value FROM unnest(ARRAY[
            CASE WHEN TG_OP <> 'INSERT' THEN OLD.strategy_id END,
            CASE WHEN TG_OP <> 'DELETE' THEN NEW.strategy_id END]) AS ids(value)
        WHERE value IS NOT NULL ORDER BY value
    LOOP
        UPDATE trading.strategy_registry SET runtime_revision = runtime_revision + 1
        WHERE id = identity;
    END LOOP;
    RETURN NULL;
END $$;
DROP TRIGGER IF EXISTS runtime_membership_revision ON trading.strategy_book_memberships;
CREATE TRIGGER runtime_membership_revision AFTER INSERT OR UPDATE OR DELETE
ON trading.strategy_book_memberships FOR EACH ROW
EXECUTE FUNCTION trading.bump_runtime_membership_revision();

CREATE TABLE IF NOT EXISTS trading.runtime_intents (
    id BIGSERIAL PRIMARY KEY,
    registry_id TEXT NOT NULL REFERENCES trading.strategy_registry(id),
    portfolio_id TEXT NOT NULL CHECK (portfolio_id = upper(btrim(portfolio_id)) AND portfolio_id <> ''),
    engine_strategy_id TEXT NOT NULL CHECK (btrim(engine_strategy_id) <> ''),
    action TEXT NOT NULL CHECK (action IN ('run','stop')),
    registry_revision BIGINT NOT NULL CHECK (registry_revision >= 0),
    config_snapshot JSONB NOT NULL CHECK (jsonb_typeof(config_snapshot) = 'object'),
    status TEXT NOT NULL CHECK (status IN ('pending','approved','rejected','superseded')),
    requested_by TEXT NOT NULL CHECK (btrim(requested_by) <> ''),
    request_reason TEXT NOT NULL CHECK (btrim(request_reason) <> ''),
    requested_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    approved_by TEXT,
    approval_reason TEXT,
    approved_at TIMESTAMPTZ,
    CHECK (status <> 'approved' OR
       (approved_by IS NOT NULL AND btrim(approved_by) <> '' AND
        approval_reason IS NOT NULL AND btrim(approval_reason) <> '' AND approved_at IS NOT NULL))
);
CREATE UNIQUE INDEX IF NOT EXISTS one_approved_runtime_scope
ON trading.runtime_intents (registry_id, portfolio_id, engine_strategy_id)
WHERE status = 'approved';
CREATE INDEX IF NOT EXISTS runtime_intents_scope_history
ON trading.runtime_intents (registry_id, requested_at DESC, id DESC);

CREATE TABLE IF NOT EXISTS trading.runtime_attempts (
    id TEXT PRIMARY KEY CHECK (btrim(id) <> ''),
    intent_id BIGINT NOT NULL REFERENCES trading.runtime_intents(id),
    registry_revision BIGINT NOT NULL CHECK (registry_revision >= 0),
    config_snapshot JSONB NOT NULL CHECK (jsonb_typeof(config_snapshot) = 'object'),
    run_date DATE NOT NULL,
    producer_version TEXT NOT NULL,
    status TEXT NOT NULL CHECK (status IN ('running','applied','failed')),
    started_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    finished_at TIMESTAMPTZ,
    publication_id TEXT,
    outcome TEXT CHECK (outcome IN ('published','stopped')),
    failure_code TEXT CHECK (failure_code ~ '^[a-z][a-z0-9_]{0,63}$'),
    CHECK (status <> 'applied' OR
       (finished_at IS NOT NULL AND outcome IS NOT NULL AND
        (outcome <> 'published' OR publication_id IS NOT NULL))),
    CHECK (status <> 'failed' OR (finished_at IS NOT NULL AND failure_code IS NOT NULL)),
    CHECK (status <> 'running' OR
       (finished_at IS NULL AND publication_id IS NULL AND outcome IS NULL AND failure_code IS NULL))
);
CREATE INDEX IF NOT EXISTS runtime_attempts_intent_history
ON trading.runtime_attempts (intent_id, started_at DESC, id DESC);

CREATE OR REPLACE FUNCTION trading.protect_runtime_intent()
RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'DELETE' THEN RAISE EXCEPTION 'runtime intent is append only'; END IF;
    IF (NEW.id,NEW.registry_id,NEW.portfolio_id,NEW.engine_strategy_id,NEW.action,
        NEW.registry_revision,NEW.config_snapshot,NEW.requested_by,NEW.request_reason,NEW.requested_at)
       IS DISTINCT FROM
       (OLD.id,OLD.registry_id,OLD.portfolio_id,OLD.engine_strategy_id,OLD.action,
        OLD.registry_revision,OLD.config_snapshot,OLD.requested_by,OLD.request_reason,OLD.requested_at)
    THEN RAISE EXCEPTION 'runtime intent snapshot is immutable'; END IF;
    IF NOT ((OLD.status = 'pending' AND NEW.status IN ('approved','rejected','superseded')) OR
            (OLD.status = 'approved' AND NEW.status = 'superseded')) THEN
        RAISE EXCEPTION 'invalid runtime intent transition';
    END IF;
    IF OLD.status <> 'pending' AND
       (NEW.approved_by,NEW.approval_reason,NEW.approved_at) IS DISTINCT FROM
       (OLD.approved_by,OLD.approval_reason,OLD.approved_at) THEN
        RAISE EXCEPTION 'runtime approval is immutable';
    END IF;
    RETURN NEW;
END $$;
DROP TRIGGER IF EXISTS protect_runtime_intent ON trading.runtime_intents;
CREATE TRIGGER protect_runtime_intent BEFORE UPDATE OR DELETE ON trading.runtime_intents
FOR EACH ROW EXECUTE FUNCTION trading.protect_runtime_intent();

CREATE OR REPLACE FUNCTION trading.protect_runtime_attempt()
RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'DELETE' THEN RAISE EXCEPTION 'runtime attempt is append only'; END IF;
    IF (NEW.id,NEW.intent_id,NEW.registry_revision,NEW.config_snapshot,
        NEW.run_date,NEW.producer_version,NEW.started_at) IS DISTINCT FROM
       (OLD.id,OLD.intent_id,OLD.registry_revision,OLD.config_snapshot,
        OLD.run_date,OLD.producer_version,OLD.started_at) THEN
        RAISE EXCEPTION 'runtime attempt snapshot is immutable';
    END IF;
    IF OLD.status <> 'running' OR NEW.status NOT IN ('applied','failed') THEN
        RAISE EXCEPTION 'invalid runtime attempt transition';
    END IF;
    RETURN NEW;
END $$;
DROP TRIGGER IF EXISTS protect_runtime_attempt ON trading.runtime_attempts;
CREATE TRIGGER protect_runtime_attempt BEFORE UPDATE OR DELETE ON trading.runtime_attempts
FOR EACH ROW EXECUTE FUNCTION trading.protect_runtime_attempt();

-- A row-level final fence covers generic/direct SQL as well as typed clients.
-- Participating multi-scope clients still pre-lock all registry rows, then all
-- canonical books in sorted order; a row trigger cannot infer an entire batch.
CREATE OR REPLACE FUNCTION trading.lock_runtime_scope(engine_id text, book text,
                                                      allow_incubating boolean DEFAULT false)
RETURNS TABLE(registry_id text, registry_revision bigint)
LANGUAGE plpgsql AS $$
DECLARE matched trading.strategy_registry%ROWTYPE;
DECLARE count_matches integer := 0;
BEGIN
    FOR matched IN SELECT r.* FROM trading.strategy_registry r
        WHERE r.strategy_type = engine_id AND
          (upper(btrim(r.portfolio_id)) = upper(btrim(book)) OR EXISTS (
            SELECT 1 FROM trading.strategy_book_memberships m
            WHERE m.strategy_id = r.id AND upper(btrim(m.portfolio_id)) = upper(btrim(book))))
        ORDER BY r.id FOR UPDATE OF r
    LOOP
        count_matches := count_matches + 1;
        registry_id := matched.id;
        registry_revision := matched.runtime_revision;
        IF (coalesce(allow_incubating,false) AND matched.lifecycle NOT IN ('live','incubating')) OR
           (NOT coalesce(allow_incubating,false) AND (matched.lifecycle <> 'live' OR NOT matched.is_active)) THEN
            RAISE EXCEPTION 'runtime_scope_ineligible';
        END IF;
    END LOOP;
    IF count_matches <> 1 OR book IS NULL OR btrim(book) = '' THEN
        RAISE EXCEPTION 'runtime_scope_unsupported';
    END IF;
    PERFORM pg_advisory_xact_lock(hashtextextended('algolens:qt-book:' || upper(btrim(book)),0));
    RETURN NEXT;
END $$;

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

DO $$
DECLARE relation_name text;
BEGIN
    FOREACH relation_name IN ARRAY ARRAY['positions','risk_limits','live_results',
                                         'equity_curve','executions','signals','live_run_metadata','run_inputs']
    LOOP
        IF to_regclass('trading.' || relation_name) IS NOT NULL THEN
            EXECUTE format('DROP TRIGGER IF EXISTS runtime_publication_fence ON trading.%I',relation_name);
            EXECUTE format('CREATE TRIGGER runtime_publication_fence BEFORE INSERT OR UPDATE OR DELETE '
                'ON trading.%I FOR EACH ROW EXECUTE FUNCTION trading.fence_runtime_publication_row()',relation_name);
        END IF;
    END LOOP;
END $$;

COMMIT;
