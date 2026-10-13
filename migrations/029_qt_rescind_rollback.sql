-- 029_qt_rescind_rollback.sql
--
-- Reverses 029: the kind CHECK loses 'rescind', position_overrides_one_open_rescind is dropped
-- and position_overrides_one_publish (023) comes back. Refuses while any rescind row exists (rows
-- are never deleted, so they would break the CHECK) or while a portfolio and day holds more than
-- one done publish row (a day approved again after a rescind: the index cannot be rebuilt). Roll
-- the engine and AlgoLens images back first.

BEGIN;

DO $$
DECLARE
    n bigint;
    dup text;
BEGIN
    IF to_regclass('trading.position_overrides') IS NULL THEN
        RETURN;
    END IF;
    SELECT count(*) INTO n FROM trading.position_overrides WHERE kind = 'rescind';
    IF n > 0 THEN
        RAISE EXCEPTION '029 rollback refused: % rescind row(s) in trading.position_overrides', n;
    END IF;
    SELECT string_agg(portfolio_id || ' ' || date, ', ') INTO dup FROM (
        SELECT portfolio_id, date FROM trading.position_overrides
        WHERE kind = 'publish' AND status = 'done'
        GROUP BY portfolio_id, date HAVING count(*) > 1) d;
    IF dup IS NOT NULL THEN
        RAISE EXCEPTION '029 rollback refused: days with more than one done publish row: %', dup;
    END IF;
END $$;

DROP INDEX IF EXISTS trading.position_overrides_one_open_rescind;

ALTER TABLE IF EXISTS trading.position_overrides DROP CONSTRAINT IF EXISTS position_overrides_kind;
ALTER TABLE IF EXISTS trading.position_overrides DROP CONSTRAINT IF EXISTS position_overrides_kind_check;
ALTER TABLE IF EXISTS trading.position_overrides ADD CONSTRAINT position_overrides_kind_check
    CHECK (kind IN ('save', 'override_request', 'override_decision', 'publish'));

CREATE UNIQUE INDEX IF NOT EXISTS position_overrides_one_publish
    ON trading.position_overrides (portfolio_id, date) WHERE kind = 'publish' AND status = 'done';

COMMENT ON TABLE trading.position_overrides IS
    'QT desk command log (docs/design/qt-contract.md section 4): one row per save, override request, override decision and publish. AlgoLens inserts; the engine moves status, started_at, finished_at, result and message. Never deleted. Migration 023.';

COMMIT;
