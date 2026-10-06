BEGIN;
DO $$ BEGIN
 IF to_regclass('trading.qt_first_day_anchors') IS NOT NULL
    AND EXISTS(SELECT 1 FROM trading.qt_first_day_anchors) THEN
  RAISE EXCEPTION 'refusing to discard QT first-day anchor evidence';
 END IF;
END $$;
ALTER TABLE trading.qt_desk_accounting_inputs
 DROP CONSTRAINT qt_desk_accounting_inputs_payload_check;
ALTER TABLE trading.qt_desk_accounting_inputs
 ADD CONSTRAINT qt_desk_accounting_inputs_payload_check CHECK(
  jsonb_typeof(payload)='object' AND payload->>'schema_version' IN
   ('qt-futures-accounting-input/v1','qt-futures-accounting-input/v2',
    'qt-equity-accounting-input/v1','qt-equity-accounting-input-empty-owner/v2'));
DROP TABLE IF EXISTS trading.qt_first_day_anchors;
DROP FUNCTION IF EXISTS trading.qt_first_day_anchor_immutable();
COMMIT;
