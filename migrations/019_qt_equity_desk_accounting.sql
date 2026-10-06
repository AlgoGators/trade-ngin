-- Governed equity-only evidence and additive closed schema admission.
-- Requires 017/018 and AlgoLens governed QT source tables. No backfill,
-- enabled policy, source publication, profile activation or scheduling.
-- Staging only: root owns application and shared dispatcher composition.
BEGIN;
CREATE TABLE trading.qt_equity_desk_evidence_sources (
    source_id text PRIMARY KEY CHECK(length(btrim(source_id))>0),
    purpose text NOT NULL CHECK(purpose IN ('basis','actions')),
    book_id text NOT NULL CHECK(length(btrim(book_id))>0),
    source_day date NOT NULL,
    producer_id text NOT NULL CHECK(length(btrim(producer_id))>0),
    policy_version text NOT NULL CHECK(length(btrim(policy_version))>0),
    policy_revision bigint NOT NULL CHECK(policy_revision>0),
    source_version text NOT NULL CHECK(length(btrim(source_version))>0),
    content_digest text NOT NULL CHECK(content_digest ~ '^[0-9a-f]{64}$'),
    payload jsonb NOT NULL CHECK(jsonb_typeof(payload)='object'),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CHECK(COALESCE(payload->>'book_id'=book_id AND
                   payload->>'source_day'=source_day::text,FALSE)),
    CHECK(COALESCE((purpose='basis' AND
        payload->>'schema_version'='qt-equity-basis-source/v1' AND
        payload->'key'->>'portfolio_id'=book_id AND
        payload->'key'->>'date'=source_day::text AND
        payload->'key'->>'portfolio_type'='qt') OR
        (purpose='actions' AND
        payload->>'schema_version'='qt-equity-actions-source/v1' AND
        jsonb_typeof(payload->'events')='array'),FALSE))
);
CREATE TRIGGER qt_equity_evidence_fence BEFORE INSERT
    ON trading.qt_equity_desk_evidence_sources FOR EACH ROW
    EXECUTE FUNCTION trading.qt_fence_desk_upstream();
CREATE TRIGGER immutable_row BEFORE UPDATE OR DELETE
    ON trading.qt_equity_desk_evidence_sources FOR EACH ROW
    EXECUTE FUNCTION trading.qt_desk_accounting_immutable();
CREATE TRIGGER immutable_truncate BEFORE TRUNCATE
    ON trading.qt_equity_desk_evidence_sources FOR EACH STATEMENT
    EXECUTE FUNCTION trading.qt_desk_accounting_immutable();

ALTER TABLE trading.qt_desk_market_sources
    DROP CONSTRAINT qt_desk_market_sources_payload_check;
ALTER TABLE trading.qt_desk_market_sources
    ADD CONSTRAINT qt_desk_market_sources_payload_check CHECK(
        jsonb_typeof(payload)='object' AND payload->>'schema_version' IN
        ('qt-accounting-market/v1','qt-equity-accounting-market/v1'));
ALTER TABLE trading.qt_desk_accounting_inputs
    DROP CONSTRAINT qt_desk_accounting_inputs_payload_check;
ALTER TABLE trading.qt_desk_accounting_inputs
    ADD CONSTRAINT qt_desk_accounting_inputs_payload_check CHECK(
        jsonb_typeof(payload)='object' AND payload->>'schema_version' IN
        ('qt-futures-accounting-input/v1','qt-futures-accounting-input/v2',
         'qt-equity-accounting-input/v1'));
ALTER TABLE trading.desk_run_results
    DROP CONSTRAINT desk_run_results_payload_check;
ALTER TABLE trading.desk_run_results
    ADD CONSTRAINT desk_run_results_payload_check CHECK(
        jsonb_typeof(payload)='object' AND payload->>'schema_version' IN
        ('qt-futures-accounting/v1','qt-equity-accounting/v1'));
ALTER TABLE trading.qt_desk_finalization_sources
    DROP CONSTRAINT qt_desk_finalization_sources_payload_check;
ALTER TABLE trading.qt_desk_finalization_sources
    ADD CONSTRAINT qt_desk_finalization_sources_payload_check CHECK(
        jsonb_typeof(payload)='object' AND payload->>'schema_version' IN
        ('qt-finalized-accounting/v1','qt-finalized-accounting/v2',
         'qt-equity-finalized-accounting/v1'));
COMMIT;
