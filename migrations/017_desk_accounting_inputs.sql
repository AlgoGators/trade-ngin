-- Requires engine 014/016 and AlgoLens 003_qt_decision_workflow.sql /
-- 004_qt_governed_sources.sql. No backfill, enabled policy, or scheduling.
BEGIN;
CREATE TABLE trading.qt_desk_finalization_sources (
    source_id text PRIMARY KEY CHECK (length(source_id)>0),
    book_id text NOT NULL CHECK (length(book_id)>0),
    source_day date NOT NULL,
    producer_id text NOT NULL CHECK (length(producer_id)>0),
    policy_version text NOT NULL CHECK (length(policy_version)>0),
    source_version text NOT NULL CHECK (length(source_version)>0),
    content_digest text NOT NULL CHECK (content_digest ~ '^[0-9a-f]{64}$'),
    payload jsonb NOT NULL CHECK (jsonb_typeof(payload)='object'
        AND payload->>'schema_version'='qt-finalized-accounting/v1'),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp()
);
CREATE TABLE trading.qt_desk_accounting_inputs (
    input_id uuid PRIMARY KEY,
    decision_id uuid NOT NULL UNIQUE REFERENCES trading.qt_decisions(decision_id),
    producer_id text NOT NULL CHECK (length(producer_id)>0),
    policy_version text NOT NULL CHECK (length(policy_version)>0),
    source_version text NOT NULL CHECK (length(source_version)>0),
    as_of timestamptz NOT NULL,
    valid_until timestamptz NOT NULL CHECK (valid_until>as_of),
    content_digest text NOT NULL CHECK (content_digest ~ '^[0-9a-f]{64}$'),
    payload jsonb NOT NULL CHECK (jsonb_typeof(payload)='object'
        AND payload->>'schema_version'='qt-futures-accounting-input/v1'),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp()
);
CREATE TABLE trading.desk_run_results (
    decision_id uuid PRIMARY KEY REFERENCES trading.qt_decisions(decision_id),
    input_id uuid NOT NULL UNIQUE REFERENCES trading.qt_desk_accounting_inputs(input_id),
    portfolio_id text NOT NULL,
    date date NOT NULL,
    content_digest text NOT NULL CHECK (content_digest ~ '^[0-9a-f]{64}$'),
    payload jsonb NOT NULL CHECK (jsonb_typeof(payload)='object'
        AND payload->>'schema_version'='qt-futures-accounting/v1'),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp()
);
CREATE FUNCTION trading.qt_desk_accounting_immutable() RETURNS trigger
LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'QT accounting evidence is immutable'; END $$;
DO $$ DECLARE name text; BEGIN
    FOREACH name IN ARRAY ARRAY['qt_desk_finalization_sources','qt_desk_accounting_inputs','desk_run_results'] LOOP
        EXECUTE format('CREATE TRIGGER immutable_row BEFORE UPDATE OR DELETE ON trading.%I FOR EACH ROW EXECUTE FUNCTION trading.qt_desk_accounting_immutable()', name);
        EXECUTE format('CREATE TRIGGER immutable_truncate BEFORE TRUNCATE ON trading.%I FOR EACH STATEMENT EXECUTE FUNCTION trading.qt_desk_accounting_immutable()', name);
    END LOOP;
END $$;
COMMIT;
