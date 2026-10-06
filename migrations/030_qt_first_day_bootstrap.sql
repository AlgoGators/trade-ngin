-- One immutable opening anchor per book. The worker constructs payload from
-- authoritative rows; this table records evidence and never synthesizes history.
BEGIN;
DO $$ BEGIN
 IF to_regclass('trading.qt_decisions') IS NULL
    OR to_regclass('trading.qt_model_seed_publications') IS NULL THEN
  RAISE EXCEPTION 'qt030 prerequisites unavailable';
 END IF;
 IF to_regclass('trading.qt_first_day_anchors') IS NOT NULL THEN
  RAISE EXCEPTION 'qt030 reserved table already exists';
 END IF;
END $$;

CREATE TABLE trading.qt_first_day_anchors(
 anchor_id uuid PRIMARY KEY,
 decision_id uuid NOT NULL UNIQUE REFERENCES trading.qt_decisions(decision_id) ON DELETE RESTRICT,
 book_id text NOT NULL UNIQUE CHECK(length(btrim(book_id))>0),
 source_day date NOT NULL,
 model_publication_id uuid NOT NULL REFERENCES trading.qt_model_seed_publications(publication_id) ON DELETE RESTRICT,
 execution_policy_revision bigint NOT NULL CHECK(execution_policy_revision>0),
 evaluation_policy_revision bigint NOT NULL CHECK(evaluation_policy_revision>0),
 content_digest text NOT NULL CHECK(content_digest ~ '^[0-9a-f]{64}$'),
 payload jsonb NOT NULL CHECK(jsonb_typeof(payload)='object'
   AND payload->>'schema_version'='qt-first-day-anchor/v1'
   AND payload->>'anchor_id'=anchor_id::text
   AND payload->>'decision_id'=decision_id::text
   AND payload->>'book_id'=book_id
   AND payload->>'source_day'=source_day::text
   AND payload->>'model_publication_id'=model_publication_id::text),
 created_at timestamptz NOT NULL DEFAULT clock_timestamp()
);

CREATE FUNCTION trading.qt_first_day_anchor_immutable() RETURNS trigger
LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'QT first-day anchor is append only'; END $$;
CREATE TRIGGER qt_first_day_anchor_immutable BEFORE UPDATE OR DELETE
 ON trading.qt_first_day_anchors FOR EACH ROW
 EXECUTE FUNCTION trading.qt_first_day_anchor_immutable();
CREATE TRIGGER qt_first_day_anchor_no_truncate BEFORE TRUNCATE
 ON trading.qt_first_day_anchors FOR EACH STATEMENT
 EXECUTE FUNCTION trading.qt_first_day_anchor_immutable();

REVOKE ALL ON trading.qt_first_day_anchors FROM PUBLIC;

ALTER TABLE trading.qt_desk_accounting_inputs
 DROP CONSTRAINT qt_desk_accounting_inputs_payload_check;
ALTER TABLE trading.qt_desk_accounting_inputs
 ADD CONSTRAINT qt_desk_accounting_inputs_payload_check CHECK(
  jsonb_typeof(payload)='object' AND payload->>'schema_version' IN
   ('qt-futures-accounting-input/v1','qt-futures-accounting-input/v2',
    'qt-futures-accounting-input-first-day/v1',
    'qt-equity-accounting-input/v1','qt-equity-accounting-input-empty-owner/v2'));
COMMIT;
