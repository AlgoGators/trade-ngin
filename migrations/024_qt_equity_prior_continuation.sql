-- STAGED PROPOSAL ONLY (equity day 2, lane N1). Not applied to any database
-- outside the owned, network-none test fixture. Requires 018/019/023 and the
-- reviewed native/API dispatch for the finalization-only market schema.
--
-- Part 1: a NULL model reference is admitted in trading.qt_desk_market_sources
--         only for the new schema 'qt-equity-finalization-market/v1', in both
--         the payload schema list and a NULL-iff-schema CHECK. The 023 market
--         reference trigger returns early for that schema only, after the
--         isolation check, the book lock and the rewrite-rule check.
-- Part 2: an immutable MODEL -> verified-prior binding table with a book
--         fence, a guard and a storage capability row. The guard admits a
--         binding only when (a) exactly one publication (v1 seed or
--         empty-owner v2) has the same id, book, day and strategy, (b) that
--         publication row was written by the current transaction, and (c) the
--         binding agrees with the finalization, anchor and actions rows it
--         names. The table is immutable, so a row that passes is final.
-- No dummy publication, no data rewrite and no deletion of historical rows:
-- every ALTER below is a metadata change plus a validating scan.
BEGIN;
LOCK TABLE trading.qt_storage_capabilities IN SHARE ROW EXCLUSIVE MODE;
DO $preflight$
BEGIN
    IF to_regclass('trading.qt_equity_model_prior_bindings') IS NOT NULL
       OR EXISTS (SELECT 1 FROM trading.qt_storage_capabilities
          WHERE capability_name='qt_equity_model_prior_binding_v1') THEN
       RAISE EXCEPTION 'qt024 reserved binding shape already present';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM trading.qt_storage_capabilities
          WHERE capability_name='qt_empty_model_owner_publication_v2' AND capability_version=1)
       OR to_regclass('trading.qt_empty_model_owner_publications') IS NULL
       OR to_regclass('trading.qt_model_seed_publications') IS NULL
       OR to_regclass('trading.qt_desk_finalizations') IS NULL
       OR to_regclass('trading.qt_desk_finalization_sources') IS NULL
       OR to_regclass('trading.qt_equity_desk_evidence_sources') IS NULL
       OR to_regprocedure('trading.qt_fence_desk_upstream()') IS NULL
       OR to_regprocedure('trading.qt_desk_accounting_immutable()') IS NULL THEN
       RAISE EXCEPTION 'qt024 requires migration 023 and its predecessors';
    END IF;
    -- 018 declared the column NOT NULL; 023 kept that. Refuse any other shape.
    IF NOT EXISTS (SELECT 1 FROM pg_attribute WHERE
         attrelid='trading.qt_desk_market_sources'::regclass
         AND attname='model_publication_id' AND attnotnull AND NOT attisdropped
         AND atttypid='uuid'::regtype) THEN
       RAISE EXCEPTION 'qt024 requires the NOT NULL 018 model_publication_id column';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE
         conrelid='trading.qt_desk_market_sources'::regclass
         AND conname='qt_desk_market_sources_payload_check' AND contype='c' AND convalidated
         AND regexp_replace(pg_get_constraintdef(oid),'\s','','g')=
          'CHECK(COALESCE(((jsonb_typeof(payload)=''object''::text)AND((payload->>''schema_version''::text)=ANY(ARRAY[''qt-accounting-market/v1''::text,''qt-equity-accounting-market/v1''::text,''qt-equity-accounting-market-empty-owner/v2''::text]))),false))') THEN
       RAISE EXCEPTION 'qt024 requires the unchanged 023 market payload check';
    END IF;
    IF EXISTS (SELECT 1 FROM pg_constraint WHERE
         conrelid='trading.qt_desk_market_sources'::regclass
         AND conname IN ('qt_market_model_null_iff_finalization','qt_market_finalization_payload_model_null')) THEN
       RAISE EXCEPTION 'qt024 reserved market constraint already present';
    END IF;
    -- The 023 trigger must be the unchanged, enabled predecessor whose body we extend.
    IF NOT EXISTS (SELECT 1 FROM pg_trigger t
         JOIN pg_proc f ON f.oid=t.tgfoid
         JOIN pg_language l ON l.oid=f.prolang
        WHERE t.tgrelid='trading.qt_desk_market_sources'::regclass
          AND t.tgname='qt_market_model_reference' AND NOT t.tgisinternal
          AND t.tgenabled='O' AND t.tgtype=7
          AND f.oid=to_regprocedure('trading.guard_qt_market_model_reference()')
          AND l.lanname='plpgsql' AND NOT f.prosecdef AND NOT f.proisstrict
          AND f.proconfig=ARRAY['search_path=pg_catalog']::text[]
          AND md5(f.prosrc)='9ee4f921a959943649072e757fe107b8') THEN
       RAISE EXCEPTION 'qt024 requires the unchanged 023 market reference trigger';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM pg_trigger t
        WHERE t.tgrelid='trading.qt_desk_market_sources'::regclass
          AND t.tgname='qt_market_source_fence' AND NOT t.tgisinternal
          AND t.tgenabled='O' AND t.tgtype=7
          AND t.tgfoid=to_regprocedure('trading.qt_fence_desk_upstream()')) THEN
       RAISE EXCEPTION 'qt024 requires the enabled 018 market fence trigger';
    END IF;
    IF EXISTS (SELECT 1 FROM pg_class WHERE oid IN('trading.qt_desk_market_sources'::regclass,
         'trading.qt_model_seed_publications'::regclass,
         'trading.qt_empty_model_owner_publications'::regclass) AND relhasrules) THEN
       RAISE EXCEPTION 'qt024 reference rewrite unsupported';
    END IF;
END $preflight$;

-- Part 1a: NULL model reference, only together with the new schema.
ALTER TABLE trading.qt_desk_market_sources ALTER COLUMN model_publication_id DROP NOT NULL;
ALTER TABLE trading.qt_desk_market_sources DROP CONSTRAINT qt_desk_market_sources_payload_check;
ALTER TABLE trading.qt_desk_market_sources ADD CONSTRAINT qt_desk_market_sources_payload_check CHECK(COALESCE(
 jsonb_typeof(payload)='object' AND payload->>'schema_version' IN
 ('qt-accounting-market/v1','qt-equity-accounting-market/v1','qt-equity-accounting-market-empty-owner/v2',
  'qt-equity-finalization-market/v1'),FALSE));
ALTER TABLE trading.qt_desk_market_sources ADD CONSTRAINT qt_market_model_null_iff_finalization CHECK(
 (model_publication_id IS NULL)=(payload->>'schema_version'='qt-equity-finalization-market/v1'));
-- The 018 payload/column agreement CHECK is NULL-permissive (a NULL column makes it
-- vacuously true), so the payload side is pinned here: a finalization-only market
-- carries the key with a JSON null. Absent or non-null values are refused.
ALTER TABLE trading.qt_desk_market_sources ADD CONSTRAINT qt_market_finalization_payload_model_null CHECK(
 payload->>'schema_version'<>'qt-equity-finalization-market/v1' OR
 COALESCE(payload->'model_publication_id'='null'::jsonb,FALSE));

-- Part 1b: the 023 trigger, body identical except for the early RETURN that
-- follows the isolation check, the book lock and the rewrite-rule check.
CREATE OR REPLACE FUNCTION trading.guard_qt_market_model_reference() RETURNS trigger LANGUAGE plpgsql SET search_path=pg_catalog AS $reference$
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
 IF NEW.payload->>'schema_version'='qt-equity-finalization-market/v1' THEN
   RETURN NEW;
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

-- Part 2: immutable MODEL -> verified-prior binding, one row per D publication.
CREATE TABLE trading.qt_equity_model_prior_bindings(
  publication_id uuid PRIMARY KEY,
  book_id text NOT NULL, source_day date NOT NULL,
  strategy_id text NOT NULL CHECK(strategy_id='LIVE_EQUITY_MEAN_REVERSION'),
  decision_id uuid NOT NULL,
  finalization_id uuid NOT NULL REFERENCES trading.qt_desk_finalizations(finalization_id),
  finalization_digest text NOT NULL CHECK(finalization_digest ~ '^[0-9a-f]{64}$'),
  finalization_source_id text NOT NULL REFERENCES trading.qt_desk_finalization_sources(source_id),
  finalization_source_digest text NOT NULL CHECK(finalization_source_digest ~ '^[0-9a-f]{64}$'),
  actions_source_id text NULL REFERENCES trading.qt_equity_desk_evidence_sources(source_id),
  actions_source_digest text NULL CHECK(actions_source_digest IS NULL OR actions_source_digest ~ '^[0-9a-f]{64}$'),
  replay_reference jsonb NOT NULL,
  replay_reference_digest text NOT NULL CHECK(replay_reference_digest ~ '^[0-9a-f]{64}$'),
  created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
  CHECK((actions_source_id IS NULL)=(actions_source_digest IS NULL)),
  -- COALESCE: a replay reference missing any of the three keys yields NULL, which a
  -- bare CHECK would admit. The three keys must be present and equal.
  CHECK(COALESCE(replay_reference->>'mode'='verified_desk_prior'
    AND replay_reference->>'finalization_id'=finalization_id::text
    AND replay_reference->>'decision_id'=decision_id::text,FALSE)));

-- Binding guard. Same isolation, book lock and rewrite-rule discipline as the 023 market
-- reference guard (the publication-identity lock is inherited from the publication insert, see below), then:
--  1. exactly one publication (v1 seed or empty-owner v2) has this id, and its
--     book, day and strategy equal the binding's;
--  2. that publication row was written by THIS transaction, so a binding can only
--     be written by the publishing transaction and never attached afterwards.
--     The row is visible to this statement, so its xmin is either committed or
--     belongs to the current transaction (an uncommitted row of any other
--     transaction is invisible). txid_status() on the row's (epoch-extended) xmin
--     answers 'in progress' exactly for the current transaction and for every
--     subtransaction of it (it first asks TransactionIdIsCurrentTransactionId, which
--     covers released savepoints), and 'committed' for any earlier transaction,
--     frozen rows included. So the publication and the binding may come from
--     different subtransactions of one transaction, and a row written by any earlier
--     transaction is refused. A publication written in a savepoint that was rolled
--     back is invisible and fails the reference check. No clock, default or explicit
--     created_at is consulted, so the check holds for v1 rows (default now()), for v2
--     rows (the native writer stamps clock_timestamp()) and under a shadowed SQL clock;
--  3. the binding agrees with the rows it names: the finalization (decision,
--     book, valuation day = publication day, content digest), the anchor
--     ('qt-finalization/'||finalization_id, same book, content digest) and, when
--     present, the actions evidence row (purpose 'actions', same book and day,
--     content digest).
CREATE FUNCTION trading.guard_qt_equity_prior_binding_publication() RETURNS trigger
LANGUAGE plpgsql SET search_path=pg_catalog AS $binding$
DECLARE matches bigint; matched_book text; matched_day date; matched_strategy text; matched_own boolean;
BEGIN
    IF pg_catalog.current_setting('transaction_isolation')<>'read committed' THEN
        RAISE EXCEPTION 'qt024 writer isolation unsupported';
    END IF;
 PERFORM pg_catalog.pg_advisory_xact_lock(pg_catalog.hashtextextended('algolens:qt-book:'||upper(btrim(NEW.book_id)),0));
 -- No separate publication-identity lock: the publication row must have been written by this very
 -- transaction (below), and both publication guards (023) already took that same advisory lock
 -- when they inserted it; advisory transaction locks are held until the top-level commit.
 IF EXISTS(SELECT 1 FROM pg_class WHERE oid IN('trading.qt_equity_model_prior_bindings'::regclass,
   'trading.qt_model_seed_publications'::regclass,'trading.qt_empty_model_owner_publications'::regclass) AND relhasrules) THEN
   RAISE EXCEPTION 'qt024 binding rewrite unsupported';
 END IF;
 SELECT count(*),min(portfolio_id),min(source_day),min(strategy_id),bool_and(written_here)
   INTO matches,matched_book,matched_day,matched_strategy,matched_own
 FROM(SELECT p.portfolio_id,p.source_day,p.strategy_id,
        (pg_catalog.txid_status(((pg_catalog.txid_current() >> 32) << 32) + p.xmin::text::bigint)='in progress') AS written_here
      FROM trading.qt_model_seed_publications p WHERE p.publication_id=NEW.publication_id UNION ALL
      SELECT p.portfolio_id,p.source_day,p.strategy_id,
        (pg_catalog.txid_status(((pg_catalog.txid_current() >> 32) << 32) + p.xmin::text::bigint)='in progress') AS written_here
      FROM trading.qt_empty_model_owner_publications p WHERE p.publication_id=NEW.publication_id) identity_union;
 IF matches<>1 OR matched_book IS DISTINCT FROM NEW.book_id OR matched_day IS DISTINCT FROM NEW.source_day
    OR matched_strategy IS DISTINCT FROM NEW.strategy_id THEN
   RAISE EXCEPTION 'qt024 binding publication reference unavailable';
 END IF;
 IF NOT COALESCE(matched_own,FALSE) THEN
   RAISE EXCEPTION 'qt024 binding must be written in its publication transaction';
 END IF;
 IF NOT EXISTS(SELECT 1 FROM trading.qt_desk_finalizations f
     WHERE f.finalization_id=NEW.finalization_id AND f.decision_id=NEW.decision_id
       AND f.book_id=NEW.book_id AND f.valuation_day=NEW.source_day
       AND f.content_digest=NEW.finalization_digest) THEN
   RAISE EXCEPTION 'qt024 binding finalization mismatch';
 END IF;
 IF NEW.finalization_source_id IS DISTINCT FROM 'qt-finalization/'||NEW.finalization_id::text
    OR NOT EXISTS(SELECT 1 FROM trading.qt_desk_finalization_sources s
     WHERE s.source_id=NEW.finalization_source_id AND s.book_id=NEW.book_id
       AND s.content_digest=NEW.finalization_source_digest) THEN
   RAISE EXCEPTION 'qt024 binding anchor mismatch';
 END IF;
 IF NEW.actions_source_id IS NOT NULL AND NOT EXISTS(SELECT 1 FROM trading.qt_equity_desk_evidence_sources a
     WHERE a.source_id=NEW.actions_source_id AND a.purpose='actions' AND a.book_id=NEW.book_id
       AND a.source_day=NEW.source_day AND a.content_digest=NEW.actions_source_digest) THEN
   RAISE EXCEPTION 'qt024 binding actions mismatch';
 END IF;
 RETURN NEW;
END $binding$;
CREATE TRIGGER qt_equity_prior_binding_fence BEFORE INSERT
  ON trading.qt_equity_model_prior_bindings FOR EACH ROW
  EXECUTE FUNCTION trading.qt_fence_desk_upstream();
CREATE TRIGGER qt_equity_prior_binding_publication BEFORE INSERT
  ON trading.qt_equity_model_prior_bindings FOR EACH ROW
  EXECUTE FUNCTION trading.guard_qt_equity_prior_binding_publication();
CREATE TRIGGER immutable_row BEFORE UPDATE OR DELETE
  ON trading.qt_equity_model_prior_bindings FOR EACH ROW
  EXECUTE FUNCTION trading.qt_desk_accounting_immutable();
CREATE TRIGGER immutable_truncate BEFORE TRUNCATE
  ON trading.qt_equity_model_prior_bindings FOR EACH STATEMENT
  EXECUTE FUNCTION trading.qt_desk_accounting_immutable();

INSERT INTO trading.qt_storage_capabilities(capability_name,capability_version)
VALUES('qt_equity_model_prior_binding_v1',1);
COMMIT;
