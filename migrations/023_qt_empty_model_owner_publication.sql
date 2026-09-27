-- STAGED PROPOSAL ONLY. No live application or changes to reviewed016/v1.
-- Requires reviewed publisher/consumer v2 dispatch before any actual use.
BEGIN;
LOCK TABLE trading.qt_storage_capabilities,trading.qt_model_seed_publications
    IN SHARE ROW EXCLUSIVE MODE;
DO $preflight$
BEGIN
    IF to_regclass('trading.qt_empty_model_owner_publications') IS NOT NULL
       OR NOT EXISTS (SELECT 1 FROM trading.qt_storage_capabilities
          WHERE capability_name='qt_exact_decimal8' AND capability_version=1)
       OR EXISTS (SELECT 1 FROM trading.qt_storage_capabilities
          WHERE capability_name='qt_empty_model_owner_publication_v2') THEN
       RAISE EXCEPTION 'qt023 prerequisites or reserved owner shape unsupported';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE
      conrelid='trading.qt_model_seed_publications'::regclass
      AND conname='qt_model_seed_publications_system_components_check'
      AND contype='c' AND convalidated
      AND regexp_replace(pg_get_constraintdef(oid),'\s','','g')=
       'CHECK(((jsonb_typeof(system_components)=''array''::text)AND(jsonb_array_length(system_components)>0)))') THEN
       RAISE EXCEPTION 'qt023 requires unchanged nonempty v1 predecessor';
    END IF;
END $preflight$;
CREATE TABLE trading.qt_empty_model_owner_publications (
    publication_id uuid PRIMARY KEY,
    attempt_id uuid UNIQUE,
    schema_version text NOT NULL CHECK(schema_version='qt-empty-model-owner-publication/v2'),
    portfolio_id text NOT NULL CHECK(length(btrim(portfolio_id))>0),
    strategy_id text NOT NULL CHECK(strategy_id='LIVE_EQUITY_MEAN_REVERSION'),
    source_day date NOT NULL,
    publication_version bigint NOT NULL CHECK(publication_version>0),
    registry_id text NOT NULL CHECK(length(btrim(registry_id))>0),
    registry_revision bigint NOT NULL CHECK(registry_revision>=0),
    configured_owner_names jsonb NOT NULL CHECK(jsonb_typeof(configured_owner_names)='array'
        AND jsonb_array_length(configured_owner_names)>0 AND jsonb_array_length(configured_owner_names)<=4096),
    configuration_snapshot jsonb NOT NULL CHECK(jsonb_typeof(configuration_snapshot)='object'),
    configuration_digest text NOT NULL CHECK(configuration_digest ~ '^[0-9a-f]{64}$'),
    fresh_empty_batches jsonb NOT NULL CHECK(jsonb_typeof(fresh_empty_batches)='array'
        AND jsonb_array_length(fresh_empty_batches)>0 AND jsonb_array_length(fresh_empty_batches)<=4096),
    inspection_capture jsonb NOT NULL CHECK(jsonb_typeof(inspection_capture)='object'),
    system_components jsonb NOT NULL CHECK(jsonb_typeof(system_components)='array' AND jsonb_array_length(system_components)=0),
    seed_digest text NOT NULL CHECK(seed_digest ~ '^[0-9a-f]{64}$'),
    proposal_components jsonb NOT NULL CHECK(jsonb_typeof(proposal_components)='array' AND jsonb_array_length(proposal_components)<=4096),
    proposal_manifest_digest text NOT NULL CONSTRAINT qt_empty_owner_proposal_manifest_digest_check CHECK(proposal_manifest_digest ~ '^[0-9a-f]{64}$'),
    qt_components jsonb NOT NULL CHECK(jsonb_typeof(qt_components)='array' AND jsonb_array_length(qt_components)<=4096),
    qt_digest text NOT NULL CHECK(qt_digest ~ '^[0-9a-f]{64}$'),
    producer_version text NOT NULL CHECK(length(btrim(producer_version))>0),
    created_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE(portfolio_id,source_day,publication_version),
    UNIQUE(portfolio_id,strategy_id,source_day,attempt_id)
);
-- Database cross-table identity guards are mandatory, including the v1 INSERT
-- path. The new runtime capability checker must explicitly recognize this
-- pinned extra v1 trigger; old exact-shape binaries safely refuse023 instead.
CREATE FUNCTION trading.guard_qt_model_publication_identity() RETURNS trigger
LANGUAGE plpgsql SET search_path=pg_catalog AS $$
DECLARE identity_key text; conflict_found boolean;
BEGIN
    IF pg_catalog.current_setting('transaction_isolation')<>'read committed' THEN
        RAISE EXCEPTION 'qt023 writer isolation unsupported';
    END IF;
    PERFORM pg_catalog.pg_advisory_xact_lock(pg_catalog.hashtextextended(
        'algolens:qt-book:'||upper(btrim(NEW.portfolio_id)),0));
    FOR identity_key IN SELECT k FROM unnest(ARRAY[
        'algolens:qt-model-publication-id:'||NEW.publication_id::text,
        CASE WHEN NEW.attempt_id IS NULL THEN NULL ELSE
          'algolens:qt-model-attempt-id:'||NEW.attempt_id::text END]) k
        WHERE k IS NOT NULL ORDER BY k LOOP
        PERFORM pg_catalog.pg_advisory_xact_lock(pg_catalog.hashtextextended(identity_key,0));
    END LOOP;
    IF TG_RELID='trading.qt_model_seed_publications'::regclass THEN
        SELECT EXISTS(SELECT 1 FROM trading.qt_empty_model_owner_publications p
          WHERE p.publication_id=NEW.publication_id
             OR (NEW.attempt_id IS NOT NULL AND p.attempt_id=NEW.attempt_id)
             OR (p.portfolio_id=NEW.portfolio_id AND p.source_day=NEW.source_day
                 AND p.publication_version=NEW.publication_version)) INTO conflict_found;
    ELSIF TG_RELID='trading.qt_empty_model_owner_publications'::regclass THEN
        SELECT EXISTS(SELECT 1 FROM trading.qt_model_seed_publications p
          WHERE p.publication_id=NEW.publication_id
             OR (NEW.attempt_id IS NOT NULL AND p.attempt_id=NEW.attempt_id)
             OR (p.portfolio_id=NEW.portfolio_id AND p.source_day=NEW.source_day
                 AND p.publication_version=NEW.publication_version)) INTO conflict_found;
    ELSE RAISE EXCEPTION 'qt023 identity guard scope unsupported';
    END IF;
    IF conflict_found THEN RAISE EXCEPTION 'qt023 cross-version identity conflict'; END IF;
    RETURN NEW;
END $$;
CREATE TRIGGER qt_model_seed_identity_guard BEFORE INSERT
ON trading.qt_model_seed_publications FOR EACH ROW
EXECUTE FUNCTION trading.guard_qt_model_publication_identity();
CREATE TRIGGER qt_empty_model_owner_identity_guard BEFORE INSERT
ON trading.qt_empty_model_owner_publications FOR EACH ROW
EXECUTE FUNCTION trading.guard_qt_model_publication_identity();
CREATE TRIGGER qt_empty_model_owner_immutable BEFORE UPDATE OR DELETE
ON trading.qt_empty_model_owner_publications FOR EACH ROW
EXECUTE FUNCTION trading.refuse_qt_seed_mutation();
CREATE TRIGGER qt_empty_model_owner_no_truncate BEFORE TRUNCATE
ON trading.qt_empty_model_owner_publications FOR EACH STATEMENT
EXECUTE FUNCTION trading.refuse_qt_seed_mutation();
INSERT INTO trading.qt_storage_capabilities(capability_name,capability_version)
VALUES('qt_empty_model_owner_publication_v2',1);
-- Replace the v1-only market FK with a strict immutable union reference.
-- No dummy seed/publication, no data rewrite, no deletion of historical rows.
DO $reference_preflight$
DECLARE original_search_path text := pg_catalog.current_setting('search_path');
BEGIN
 PERFORM pg_catalog.set_config('search_path','pg_catalog',true);
 IF NOT EXISTS(SELECT 1 FROM pg_constraint WHERE
   conrelid='trading.qt_desk_market_sources'::regclass AND
   conname='qt_desk_market_sources_model_publication_id_fkey' AND contype='f' AND convalidated AND
   confrelid='trading.qt_model_seed_publications'::regclass AND
   pg_get_constraintdef(oid)='FOREIGN KEY (model_publication_id) REFERENCES trading.qt_model_seed_publications(publication_id)') THEN
   RAISE EXCEPTION 'qt023 requires exact immutable v1 market reference predecessor';
 END IF;
 IF EXISTS(SELECT 1 FROM pg_class WHERE oid IN('trading.qt_desk_market_sources'::regclass,
   'trading.qt_model_seed_publications'::regclass,'trading.qt_empty_model_owner_publications'::regclass) AND relhasrules) THEN
   RAISE EXCEPTION 'qt023 reference rewrite unsupported';
 END IF;
 PERFORM pg_catalog.set_config('search_path',original_search_path,true);
END $reference_preflight$;
ALTER TABLE trading.qt_desk_market_sources DROP CONSTRAINT qt_desk_market_sources_model_publication_id_fkey;
CREATE FUNCTION trading.guard_qt_market_model_reference() RETURNS trigger LANGUAGE plpgsql SET search_path=pg_catalog AS $reference$
DECLARE matches bigint; matched_book text; matched_day date; is_empty boolean;
BEGIN
    IF pg_catalog.current_setting('transaction_isolation')<>'read committed' THEN
        RAISE EXCEPTION 'qt023 writer isolation unsupported';
    END IF;
 PERFORM pg_catalog.pg_advisory_xact_lock(pg_catalog.hashtextextended('algolens:qt-book:'||upper(btrim(NEW.book_id)),0));
 -- Same global identity lock as BOTH publisher guards, including another book.
 PERFORM pg_catalog.pg_advisory_xact_lock(pg_catalog.hashtextextended('algolens:qt-model-publication-id:'||NEW.model_publication_id::text,0));
 IF EXISTS(SELECT 1 FROM pg_class WHERE oid IN('trading.qt_desk_market_sources'::regclass,
   'trading.qt_model_seed_publications'::regclass,'trading.qt_empty_model_owner_publications'::regclass) AND relhasrules) THEN
   RAISE EXCEPTION 'qt023 reference rewrite unsupported';
 END IF;
 SELECT count(*),min(portfolio_id),min(source_day),bool_and(empty_owner) INTO matches,matched_book,matched_day,is_empty
 FROM(SELECT portfolio_id,source_day,false AS empty_owner FROM trading.qt_model_seed_publications
       WHERE publication_id=NEW.model_publication_id UNION ALL
      SELECT portfolio_id,source_day,true AS empty_owner FROM trading.qt_empty_model_owner_publications
       WHERE publication_id=NEW.model_publication_id) identity_union;
 IF matches<>1 OR matched_book IS DISTINCT FROM NEW.book_id OR matched_day IS DISTINCT FROM NEW.source_day OR
    (NEW.payload->>'schema_version'='qt-equity-accounting-market-empty-owner/v2' AND NOT is_empty) THEN
   RAISE EXCEPTION 'qt023 market model reference unavailable';
 END IF;
 RETURN NEW;
END $reference$;
CREATE TRIGGER qt_market_model_reference BEFORE INSERT ON trading.qt_desk_market_sources
 FOR EACH ROW EXECUTE FUNCTION trading.guard_qt_market_model_reference();
-- Explicit empty-owner financial schema admission. Native/API authority and
-- actual recomputation remain mandatory; absent arrays alone never certify it.
-- All existing v1/nonempty tags and constraints retain their meaning.
ALTER TABLE trading.qt_desk_market_sources DROP CONSTRAINT qt_desk_market_sources_payload_check;
ALTER TABLE trading.qt_desk_market_sources ADD CONSTRAINT qt_desk_market_sources_payload_check CHECK(COALESCE(
 jsonb_typeof(payload)='object' AND payload->>'schema_version' IN
 ('qt-accounting-market/v1','qt-equity-accounting-market/v1','qt-equity-accounting-market-empty-owner/v2'),FALSE));
ALTER TABLE trading.qt_desk_market_sources ADD CONSTRAINT qt_empty_market_scope CHECK(
 payload->>'schema_version'<>'qt-equity-accounting-market-empty-owner/v2' OR COALESCE(
 payload->'instruments'='[]'::jsonb AND payload->>'day_mode'='open' AND
 payload->>'calculation_version'='qt-equity-main08b15c/v1' AND payload->>'currency'='USD',FALSE));
ALTER TABLE trading.qt_desk_accounting_inputs DROP CONSTRAINT qt_desk_accounting_inputs_payload_check;
ALTER TABLE trading.qt_desk_accounting_inputs ADD CONSTRAINT qt_desk_accounting_inputs_payload_check CHECK(COALESCE(
 jsonb_typeof(payload)='object' AND payload->>'schema_version' IN
 ('qt-futures-accounting-input/v1','qt-futures-accounting-input/v2','qt-equity-accounting-input/v1','qt-equity-accounting-input-empty-owner/v2'),FALSE));
ALTER TABLE trading.qt_desk_accounting_inputs ADD CONSTRAINT qt_empty_accounting_input_scope CHECK(
 payload->>'schema_version'<>'qt-equity-accounting-input-empty-owner/v2' OR COALESCE(
 payload->'previous_positions'='[]'::jsonb AND payload->'instruments'='[]'::jsonb AND payload->'actions'='[]'::jsonb AND
 jsonb_typeof(payload->'previous_totals')='array' AND jsonb_array_length(payload->'previous_totals')=1 AND
 payload->'previous_totals'->0->>'strategy_id'='LIVE_EQUITY_MEAN_REVERSION' AND
 payload->>'calculation_version'='qt-equity-main08b15c/v1' AND payload->>'day_mode'='open' AND payload->>'currency'='USD',FALSE));
ALTER TABLE trading.desk_run_results DROP CONSTRAINT desk_run_results_payload_check;
ALTER TABLE trading.desk_run_results ADD CONSTRAINT desk_run_results_payload_check CHECK(COALESCE(
 jsonb_typeof(payload)='object' AND payload->>'schema_version' IN
 ('qt-futures-accounting/v1','qt-equity-accounting/v1','qt-equity-accounting-empty-owner/v2'),FALSE));
ALTER TABLE trading.desk_run_results ADD CONSTRAINT qt_empty_accounting_output_scope CHECK(
 payload->>'schema_version'<>'qt-equity-accounting-empty-owner/v2' OR COALESCE(
 payload->'observation'->'fills'='[]'::jsonb AND payload->'executions'='[]'::jsonb AND
 payload->'corporate_action_adjustments'='[]'::jsonb AND payload->'distance'='[]'::jsonb AND payload->'layers_applied'='[]'::jsonb AND
 jsonb_typeof(payload->'live_results')='array' AND jsonb_array_length(payload->'live_results')=1 AND
 payload->'live_results'->0->>'strategy_id'='LIVE_EQUITY_MEAN_REVERSION' AND
 jsonb_typeof(payload->'equity_curve')='array' AND jsonb_array_length(payload->'equity_curve')=1 AND
 payload->'equity_curve'->0->>'strategy_id'='LIVE_EQUITY_MEAN_REVERSION',FALSE));
ALTER TABLE trading.qt_desk_finalizations DROP CONSTRAINT qt_desk_finalizations_payload_check;
ALTER TABLE trading.qt_desk_finalizations ADD CONSTRAINT qt_desk_finalizations_payload_check CHECK(COALESCE(
 jsonb_typeof(payload)='object' AND payload->>'schema_version' IN
 ('qt-desk-finalization/v1','qt-equity-desk-finalization/v1','qt-equity-desk-finalization-empty-owner/v2'),FALSE));
ALTER TABLE trading.qt_desk_finalizations ADD CONSTRAINT qt_empty_finalization_scope CHECK(
 payload->>'schema_version'<>'qt-equity-desk-finalization-empty-owner/v2' OR COALESCE(
 payload->'components'='[]'::jsonb AND payload->'before_financial'->'positions'='[]'::jsonb AND
 payload->'after_financial'->'positions'='[]'::jsonb AND jsonb_typeof(payload->'engine_totals')='array' AND
 jsonb_array_length(payload->'engine_totals')=1 AND payload->'engine_totals'->0->>'strategy_id'='LIVE_EQUITY_MEAN_REVERSION',FALSE));
ALTER TABLE trading.qt_desk_finalization_sources DROP CONSTRAINT qt_desk_finalization_sources_payload_check;
ALTER TABLE trading.qt_desk_finalization_sources ADD CONSTRAINT qt_desk_finalization_sources_payload_check CHECK(COALESCE(
 jsonb_typeof(payload)='object' AND payload->>'schema_version' IN
 ('qt-finalized-accounting/v1','qt-finalized-accounting/v2','qt-equity-finalized-accounting/v1','qt-equity-finalized-accounting/v2','qt-equity-finalized-accounting-empty-owner/v3'),FALSE));
ALTER TABLE trading.qt_desk_finalization_sources ADD CONSTRAINT qt_empty_finalized_anchor_scope CHECK(
 payload->>'schema_version'<>'qt-equity-finalized-accounting-empty-owner/v3' OR COALESCE(
 payload->'previous_positions'='[]'::jsonb AND jsonb_typeof(payload->'previous_totals')='array' AND
 jsonb_array_length(payload->'previous_totals')=1 AND payload->'previous_totals'->0->>'strategy_id'='LIVE_EQUITY_MEAN_REVERSION' AND
 payload->>'book_id'=book_id AND payload->>'source_day'=source_day::text AND
 payload->>'finalization_id' ~ '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$' AND
 payload->>'finalization_digest' ~ '^[0-9a-f]{64}$' AND source_id='qt-finalization/'||(payload->>'finalization_id') AND source_version=source_id,FALSE));
COMMIT;
