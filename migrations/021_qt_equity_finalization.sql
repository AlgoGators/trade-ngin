-- Additive schema admission for proved equity mark successors only.
-- Requires 018/019/020. No sources, policies, runtime switches or backfill.
BEGIN;
ALTER TABLE trading.qt_desk_finalizations
    DROP CONSTRAINT qt_desk_finalizations_payload_check;
ALTER TABLE trading.qt_desk_finalizations
    ADD CONSTRAINT qt_desk_finalizations_payload_check CHECK(COALESCE(
        jsonb_typeof(payload)='object' AND payload->>'schema_version' IN
        ('qt-desk-finalization/v1','qt-equity-desk-finalization/v1'),FALSE));
ALTER TABLE trading.qt_desk_finalization_sources
    DROP CONSTRAINT qt_desk_finalization_sources_payload_check;
ALTER TABLE trading.qt_desk_finalization_sources
    ADD CONSTRAINT qt_desk_finalization_sources_payload_check CHECK(COALESCE(
        jsonb_typeof(payload)='object' AND payload->>'schema_version' IN
        ('qt-finalized-accounting/v1','qt-finalized-accounting/v2',
         'qt-equity-finalized-accounting/v1','qt-equity-finalized-accounting/v2'),FALSE));
ALTER TABLE trading.qt_desk_finalization_sources
    ADD CONSTRAINT qt_equity_finalized_anchor_identity CHECK(
        payload->>'schema_version'<>'qt-equity-finalized-accounting/v2' OR COALESCE(
            payload->>'book_id'=book_id AND payload->>'source_day'=source_day::text AND
            payload->>'finalization_id' ~ '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$' AND
            payload->>'finalization_digest' ~ '^[0-9a-f]{64}$' AND
            source_id='qt-finalization/'||(payload->>'finalization_id') AND
            source_version=source_id,FALSE));
COMMIT;
