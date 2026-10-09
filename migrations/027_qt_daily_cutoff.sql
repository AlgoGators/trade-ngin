-- 027_qt_daily_cutoff.sql
--
-- The QT daily cutoff (decided 2026-10-09; docs/design/qt-contract.md C7, amending rulings 18
-- and 29). Applied after 022 and 025. Additive and transactional; re-applying it is a no-op.
--
--   1. trading.live_run_metadata gains the day's approval and send state:
--        - publish_source text NULL, CHECK (publish_source IN ('desk', 'fallback', 'model-only')):
--          who published the day. 'desk' = the desk's approval (the Publish command, "Approve"
--          in AlgoLens); 'fallback' = the 10:00 fallback or the catch-up of a missed past day
--          (qt reset to the model's book); 'model-only' = the untouched model twin.
--        - sent_at timestamptz NULL: when the day's e-mail went out (set once, by the engine).
--      published_by and published_at (022) are kept. Rows published before 027 keep NULL in
--      both columns: their source is not known, and an e-mail they sent is still found in the
--      publish row's result.email_sent_at (the engine checks both and never sends twice).
--   2. Grants, as 025 set them: svc_trade_ngin (the engine and the desk service) reads and writes
--      the two columns (it already holds SELECT, INSERT, UPDATE, DELETE on the table; granted
--      again here, idempotently). svc_algolens (qt_algolens_app) only reads them: its table-wide
--      INSERT and UPDATE become column grants on every OTHER column, so everything it could
--      write before it still can, and it cannot write publish_source or sent_at. Other roles are
--      unchanged (the table-wide grants 025 made cover the new columns: readers read, the *_rw
--      roles write).

BEGIN;

DO $$
DECLARE
    bad text;
BEGIN
    IF to_regclass('trading.live_run_metadata') IS NULL THEN
        RAISE EXCEPTION 'trading.live_run_metadata does not exist';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = 'trading'
                   AND table_name = 'live_run_metadata' AND column_name = 'published_at') THEN
        RAISE EXCEPTION 'trading.live_run_metadata has no published_at (apply 022 first)';
    END IF;
    SELECT string_agg(column_name || ' ' || data_type, ', ') INTO bad
    FROM information_schema.columns
    WHERE table_schema = 'trading' AND table_name = 'live_run_metadata'
      AND ((column_name = 'publish_source' AND data_type <> 'text')
        OR (column_name = 'sent_at' AND data_type <> 'timestamp with time zone'));
    IF bad IS NOT NULL THEN
        RAISE EXCEPTION 'trading.live_run_metadata has a column of another type: %', bad;
    END IF;
END $$;

-- 1. The columns.
ALTER TABLE trading.live_run_metadata ADD COLUMN IF NOT EXISTS publish_source text;
ALTER TABLE trading.live_run_metadata ADD COLUMN IF NOT EXISTS sent_at timestamptz;

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_constraint
                   WHERE conname = 'live_run_metadata_publish_source_check'
                     AND conrelid = 'trading.live_run_metadata'::regclass) THEN
        ALTER TABLE trading.live_run_metadata ADD CONSTRAINT live_run_metadata_publish_source_check
            CHECK (publish_source IN ('desk', 'fallback', 'model-only'));
    END IF;
END $$;

COMMENT ON COLUMN trading.live_run_metadata.publish_source IS
    'QT contract C7: who published the day: desk (the desk''s approval), fallback (the 10:00 fallback, or the catch-up of a missed past day: the model''s book), model-only (the model twin). NULL: unpublished, or published before migration 027.';
COMMENT ON COLUMN trading.live_run_metadata.sent_at IS
    'QT contract C7: when the day''s daily e-mail and CSV went out (set once by the engine). NULL: not sent (not yet, a past day caught up, or e-mail disabled).';

-- 2. Grants.
DO $$
DECLARE
    others text;
    priv text;
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'svc_trade_ngin') THEN
        GRANT SELECT, INSERT, UPDATE ON trading.live_run_metadata TO svc_trade_ngin;
    END IF;
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'svc_algolens') THEN
        SELECT string_agg(quote_ident(column_name), ', ' ORDER BY ordinal_position) INTO others
        FROM information_schema.columns
        WHERE table_schema = 'trading' AND table_name = 'live_run_metadata'
          AND column_name NOT IN ('publish_source', 'sent_at');
        GRANT SELECT ON trading.live_run_metadata TO svc_algolens;
        FOREACH priv IN ARRAY ARRAY['INSERT', 'UPDATE'] LOOP
            -- Only a table-wide grant held directly is narrowed (re-applying finds none). A
            -- table-wide REVOKE also drops the role's column grants, so it goes first.
            IF EXISTS (SELECT 1 FROM information_schema.role_table_grants
                       WHERE grantee = 'svc_algolens' AND table_schema = 'trading'
                         AND table_name = 'live_run_metadata' AND privilege_type = priv) THEN
                EXECUTE format('REVOKE %s ON trading.live_run_metadata FROM svc_algolens', priv);
                EXECUTE format('GRANT %s (%s) ON trading.live_run_metadata TO svc_algolens',
                               priv, others);
            END IF;
        END LOOP;
    END IF;
END $$;

COMMIT;
