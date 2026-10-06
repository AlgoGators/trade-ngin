-- Explicit governed market inputs and a proved successor to provisional QT
-- accounting. Requires engine016/017 and AlgoLens003/004. No policy activation.
BEGIN;
CREATE TABLE trading.qt_desk_market_sources (
    source_id uuid PRIMARY KEY,
    book_id text NOT NULL CHECK(length(btrim(book_id))>0),
    source_day date NOT NULL,
    model_publication_id uuid NOT NULL REFERENCES trading.qt_model_seed_publications(publication_id),
    producer_id text NOT NULL CHECK(length(btrim(producer_id))>0),
    policy_version text NOT NULL CHECK(length(btrim(policy_version))>0),
    policy_revision bigint NOT NULL CHECK(policy_revision>0),
    source_version text NOT NULL CHECK(length(btrim(source_version))>0),
    as_of timestamptz NOT NULL,
    valid_until timestamptz NOT NULL CHECK(valid_until>as_of),
    content_digest text NOT NULL CHECK(content_digest ~ '^[0-9a-f]{64}$'),
    payload jsonb NOT NULL CHECK(jsonb_typeof(payload)='object' AND payload->>'schema_version'='qt-accounting-market/v1'),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    UNIQUE(book_id,source_day,source_version),
    CHECK(payload->>'book_id'=book_id AND payload->>'source_day'=source_day::text AND payload->>'model_publication_id'=model_publication_id::text)
);
CREATE TABLE trading.qt_desk_finalizations (
    finalization_id uuid PRIMARY KEY,
    decision_id uuid NOT NULL UNIQUE REFERENCES trading.desk_run_results(decision_id),
    market_source_id uuid NOT NULL REFERENCES trading.qt_desk_market_sources(source_id),
    book_id text NOT NULL CHECK(length(btrim(book_id))>0),
    source_day date NOT NULL,
    valuation_day date NOT NULL CHECK(valuation_day>source_day),
    producer_id text NOT NULL CHECK(length(btrim(producer_id))>0),
    policy_version text NOT NULL CHECK(length(btrim(policy_version))>0),
    policy_revision bigint NOT NULL CHECK(policy_revision>0),
    source_version text NOT NULL CHECK(length(btrim(source_version))>0),
    input_digest text NOT NULL CHECK(input_digest ~ '^[0-9a-f]{64}$'),
    output_digest text NOT NULL CHECK(output_digest ~ '^[0-9a-f]{64}$'),
    content_digest text NOT NULL CHECK(content_digest ~ '^[0-9a-f]{64}$'),
    payload jsonb NOT NULL CHECK(jsonb_typeof(payload)='object' AND payload->>'schema_version'='qt-desk-finalization/v1'),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CHECK(payload->>'book_id'=book_id AND payload->>'source_day'=source_day::text AND payload->>'valuation_day'=valuation_day::text AND payload->>'decision_id'=decision_id::text AND payload->>'finalization_id'=finalization_id::text AND payload->>'market_source_id'=market_source_id::text)
);
CREATE FUNCTION trading.qt_fence_desk_upstream() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    PERFORM pg_advisory_xact_lock(hashtextextended('algolens:qt-book:'||upper(btrim(NEW.book_id)),0));
    RETURN NEW;
END $$;
CREATE TRIGGER qt_market_source_fence BEFORE INSERT ON trading.qt_desk_market_sources FOR EACH ROW EXECUTE FUNCTION trading.qt_fence_desk_upstream();
CREATE TRIGGER qt_finalization_fence BEFORE INSERT ON trading.qt_desk_finalizations FOR EACH ROW EXECUTE FUNCTION trading.qt_fence_desk_upstream();
DO $$ DECLARE name text; BEGIN
    FOREACH name IN ARRAY ARRAY['qt_desk_market_sources','qt_desk_finalizations'] LOOP
        EXECUTE format('CREATE TRIGGER immutable_row BEFORE UPDATE OR DELETE ON trading.%I FOR EACH ROW EXECUTE FUNCTION trading.qt_desk_accounting_immutable()',name);
        EXECUTE format('CREATE TRIGGER immutable_truncate BEFORE TRUNCATE ON trading.%I FOR EACH STATEMENT EXECUTE FUNCTION trading.qt_desk_accounting_immutable()',name);
    END LOOP;
END $$;
ALTER TABLE trading.qt_desk_finalization_sources DROP CONSTRAINT qt_desk_finalization_sources_payload_check;
ALTER TABLE trading.qt_desk_finalization_sources ADD CONSTRAINT qt_desk_finalization_sources_payload_check
    CHECK(jsonb_typeof(payload)='object' AND payload->>'schema_version' IN ('qt-finalized-accounting/v1','qt-finalized-accounting/v2'));
ALTER TABLE trading.qt_desk_accounting_inputs DROP CONSTRAINT qt_desk_accounting_inputs_payload_check;
ALTER TABLE trading.qt_desk_accounting_inputs ADD CONSTRAINT qt_desk_accounting_inputs_payload_check
    CHECK(jsonb_typeof(payload)='object' AND payload->>'schema_version' IN ('qt-futures-accounting-input/v1','qt-futures-accounting-input/v2'));
COMMIT;
