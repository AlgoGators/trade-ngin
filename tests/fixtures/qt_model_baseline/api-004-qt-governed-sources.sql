-- Explicit versioned authority for evaluator inputs and observed execution.
-- No policy, publisher, enabled workflow or real financial input is seeded.
BEGIN;

CREATE FUNCTION trading.qt_source_codes_valid(value jsonb) RETURNS boolean
LANGUAGE sql IMMUTABLE STRICT AS $$
    SELECT jsonb_typeof(value) = 'array'
       AND jsonb_array_length(value) <= 128
       AND NOT EXISTS (SELECT 1 FROM jsonb_array_elements(value) AS item
                       WHERE jsonb_typeof(item) <> 'string'
                          OR length(btrim(item #>> '{}')) = 0)
       AND (SELECT count(*) = count(DISTINCT item)
            FROM jsonb_array_elements(value) AS item)
$$;

CREATE TABLE trading.qt_source_policies (
    book_id text NOT NULL CHECK (length(btrim(book_id)) > 0),
    purpose text NOT NULL CHECK (purpose IN ('evaluation','execution')),
    enabled boolean NOT NULL,
    version bigint NOT NULL CHECK (version > 0),
    producer_id text NOT NULL CHECK (length(btrim(producer_id)) > 0),
    policy_version text NOT NULL CHECK (length(btrim(policy_version)) > 0),
    evaluator_build text,
    evaluator_sha256 text,
    allowed_override_codes jsonb NOT NULL CHECK (trading.qt_source_codes_valid(allowed_override_codes)),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (book_id,purpose),
    CHECK ((purpose='evaluation' AND length(btrim(evaluator_build))>0
                AND evaluator_build IS NOT NULL AND evaluator_sha256 IS NOT NULL
                AND evaluator_sha256 ~ '^[0-9a-f]{64}$')
        OR (purpose='execution' AND evaluator_build IS NULL
                AND evaluator_sha256 IS NULL AND allowed_override_codes='[]'::jsonb))
);

CREATE TABLE trading.qt_evaluation_snapshots (
    snapshot_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    book_id text NOT NULL CHECK (length(btrim(book_id))>0),
    source_day date NOT NULL,
    model_publication_id uuid NOT NULL,
    producer_id text NOT NULL CHECK (length(btrim(producer_id))>0),
    policy_version text NOT NULL CHECK (length(btrim(policy_version))>0),
    source_version text NOT NULL CHECK (length(btrim(source_version))>0),
    as_of timestamptz NOT NULL,
    valid_until timestamptz NOT NULL CHECK (valid_until>=as_of),
    content_digest text NOT NULL CHECK (content_digest ~ '^[0-9a-f]{64}$'),
    payload jsonb NOT NULL CHECK (jsonb_typeof(payload)='object'),
    UNIQUE (book_id,source_day,source_version)
);
CREATE INDEX qt_evaluation_snapshots_current
    ON trading.qt_evaluation_snapshots(book_id,source_day,snapshot_id DESC);

CREATE TABLE trading.qt_execution_observations (
    observation_id uuid PRIMARY KEY,
    decision_id uuid NOT NULL REFERENCES trading.qt_decisions(decision_id) ON DELETE RESTRICT,
    producer_id text NOT NULL CHECK (length(btrim(producer_id))>0),
    policy_version text NOT NULL CHECK (length(btrim(policy_version))>0),
    source_version text NOT NULL CHECK (length(btrim(source_version))>0),
    as_of timestamptz NOT NULL,
    valid_until timestamptz NOT NULL CHECK (valid_until>=as_of),
    content_digest text NOT NULL CHECK (content_digest ~ '^[0-9a-f]{64}$'),
    payload jsonb NOT NULL CHECK (jsonb_typeof(payload)='object'),
    UNIQUE (decision_id,source_version),
    UNIQUE (observation_id,decision_id)
);

CREATE TABLE trading.qt_desk_results (
    decision_id uuid PRIMARY KEY REFERENCES trading.qt_decisions(decision_id) ON DELETE RESTRICT,
    attempt_id uuid NOT NULL UNIQUE,
    observation_id uuid NOT NULL,
    content_digest text NOT NULL CHECK (content_digest ~ '^[0-9a-f]{64}$'),
    payload jsonb NOT NULL CHECK (jsonb_typeof(payload)='object'),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    FOREIGN KEY (observation_id,decision_id)
        REFERENCES trading.qt_execution_observations(observation_id,decision_id) ON DELETE RESTRICT
);

-- Snapshot/result writers serialize with readers of the same canonical book.
-- The immutable decision provides routing for observed executions and results.
CREATE FUNCTION trading.qt_fence_governed_source() RETURNS trigger
LANGUAGE plpgsql AS $$
DECLARE target_book text;
BEGIN
    IF TG_TABLE_NAME IN ('qt_source_policies','qt_evaluation_snapshots') THEN
        target_book := NEW.book_id;
    ELSE
        SELECT book_id INTO target_book FROM trading.qt_decisions
          WHERE decision_id=NEW.decision_id;
    END IF;
    IF target_book IS NULL OR length(btrim(target_book))=0 THEN
        RAISE EXCEPTION 'QT source scope missing';
    END IF;
    PERFORM pg_advisory_xact_lock(hashtextextended('algolens:qt-book:' || upper(btrim(target_book)),0));
    IF TG_TABLE_NAME='qt_source_policies' THEN
        IF TG_OP='UPDATE' AND (NEW.book_id IS DISTINCT FROM OLD.book_id
            OR NEW.purpose IS DISTINCT FROM OLD.purpose OR NEW.version<>OLD.version+1) THEN
            RAISE EXCEPTION 'QT policy scope/version changed incorrectly';
        END IF;
        NEW.updated_at := clock_timestamp();
    END IF;
    RETURN NEW;
END $$;

CREATE TRIGGER qt_source_policy_fence BEFORE INSERT OR UPDATE ON trading.qt_source_policies
    FOR EACH ROW EXECUTE FUNCTION trading.qt_fence_governed_source();
CREATE TRIGGER qt_source_policy_no_delete BEFORE DELETE ON trading.qt_source_policies
    FOR EACH ROW EXECUTE FUNCTION trading.qt_reject_row_change();
CREATE TRIGGER qt_source_policy_no_truncate BEFORE TRUNCATE ON trading.qt_source_policies
    FOR EACH STATEMENT EXECUTE FUNCTION trading.qt_reject_row_change();

CREATE TRIGGER qt_snapshot_fence BEFORE INSERT ON trading.qt_evaluation_snapshots
    FOR EACH ROW EXECUTE FUNCTION trading.qt_fence_governed_source();
CREATE TRIGGER qt_snapshot_immutable BEFORE UPDATE OR DELETE ON trading.qt_evaluation_snapshots
    FOR EACH ROW EXECUTE FUNCTION trading.qt_reject_row_change();
CREATE TRIGGER qt_snapshot_no_truncate BEFORE TRUNCATE ON trading.qt_evaluation_snapshots
    FOR EACH STATEMENT EXECUTE FUNCTION trading.qt_reject_row_change();

CREATE TRIGGER qt_execution_observation_fence BEFORE INSERT ON trading.qt_execution_observations
    FOR EACH ROW EXECUTE FUNCTION trading.qt_fence_governed_source();
CREATE TRIGGER qt_execution_observation_immutable BEFORE UPDATE OR DELETE ON trading.qt_execution_observations
    FOR EACH ROW EXECUTE FUNCTION trading.qt_reject_row_change();
CREATE TRIGGER qt_execution_observation_no_truncate BEFORE TRUNCATE ON trading.qt_execution_observations
    FOR EACH STATEMENT EXECUTE FUNCTION trading.qt_reject_row_change();

CREATE TRIGGER qt_desk_result_fence BEFORE INSERT ON trading.qt_desk_results
    FOR EACH ROW EXECUTE FUNCTION trading.qt_fence_governed_source();
CREATE TRIGGER qt_desk_result_immutable BEFORE UPDATE OR DELETE ON trading.qt_desk_results
    FOR EACH ROW EXECUTE FUNCTION trading.qt_reject_row_change();
CREATE TRIGGER qt_desk_result_no_truncate BEFORE TRUNCATE ON trading.qt_desk_results
    FOR EACH STATEMENT EXECUTE FUNCTION trading.qt_reject_row_change();

COMMIT;
