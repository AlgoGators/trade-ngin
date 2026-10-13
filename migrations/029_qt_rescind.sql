-- 029_qt_rescind.sql
--
-- Rescind (decided by Dom 2026-10-13; docs/design/qt-contract.md C8): the desk takes back its
-- own approval before the 09:30 send. Applied after 023, 025 and 027. Transactional; re-applying
-- it is a no-op.
--
--   1. trading.position_overrides.kind gains 'rescind': the command AlgoLens writes for "Rescind
--      approval" (desk.proto 1.2.0 Rescind). The engine clears published_by, published_at and
--      publish_source on the day's live_run_metadata rows and records the approval it took back
--      in the row's result.
--   2. position_overrides_one_publish (023: at most one done publish row per portfolio and day)
--      is dropped. A rescinded approval leaves its publish row done (C4: terminal rows are
--      final), so the day's next approval is a second done publish row. live_run_metadata
--      published_at, checked under the day lock by AlgoLens and by the engine, is what keeps a
--      day from being approved twice (C3), as it already was. The open-publish index
--      (position_overrides_one_open_publish, 025) stays.
--   3. position_overrides_one_open_rescind: at most one pending or running rescind per
--      portfolio and day, as for publish and override requests (C2).
--
-- No grant changes: svc_algolens already inserts command rows and svc_trade_ngin already writes
-- live_run_metadata.

BEGIN;

DO $$
BEGIN
    IF to_regclass('trading.position_overrides') IS NULL THEN
        RAISE EXCEPTION 'trading.position_overrides does not exist (apply 023 first)';
    END IF;
    IF to_regclass('trading.position_overrides_one_open_publish') IS NULL THEN
        RAISE EXCEPTION 'position_overrides_one_open_publish does not exist (apply 025 first)';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = 'trading'
                   AND table_name = 'live_run_metadata' AND column_name = 'publish_source') THEN
        RAISE EXCEPTION 'trading.live_run_metadata has no publish_source (apply 027 first)';
    END IF;
END $$;

-- 1. The kind.
DO $$
DECLARE
    c record;
BEGIN
    -- 023 declared the CHECK inline, so its name is the server's choice: drop every CHECK on
    -- kind alone, then add the named one.
    FOR c IN
        SELECT con.conname FROM pg_constraint con
        WHERE con.conrelid = 'trading.position_overrides'::regclass AND con.contype = 'c'
          AND con.conname <> 'position_overrides_kind'
          AND pg_get_constraintdef(con.oid) ~ '^CHECK \(\(kind = ANY'
    LOOP
        EXECUTE format('ALTER TABLE trading.position_overrides DROP CONSTRAINT %I', c.conname);
    END LOOP;
    IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'position_overrides_kind'
                   AND conrelid = 'trading.position_overrides'::regclass) THEN
        ALTER TABLE trading.position_overrides ADD CONSTRAINT position_overrides_kind
            CHECK (kind IN ('save', 'override_request', 'override_decision', 'publish', 'rescind'));
    END IF;
END $$;

-- 2. A rescinded day may be approved again.
DROP INDEX IF EXISTS trading.position_overrides_one_publish;

-- 3. One open rescind per day.
CREATE UNIQUE INDEX IF NOT EXISTS position_overrides_one_open_rescind
    ON trading.position_overrides (portfolio_id, date)
    WHERE kind = 'rescind' AND status IN ('pending', 'running');

COMMENT ON TABLE trading.position_overrides IS
    'QT desk command log (docs/design/qt-contract.md section 4): one row per save, override request, override decision, publish and rescind. AlgoLens inserts; the engine moves status, started_at, finished_at, result and message. Never deleted. Migrations 023, 025, 029.';

COMMIT;
