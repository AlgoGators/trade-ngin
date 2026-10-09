-- 023_qt_command_log_rollback.sql
--
-- Reverses 023 except the four dropped tables (risk_limits, portfolios, strategy_book_memberships,
-- portfolio_assignments), which nothing on main reads and which ruling 28 retires; recreate them from
-- the branch that introduced them if ever needed. Refuses if the command log holds rows.

BEGIN;

DO $$
BEGIN
    IF to_regclass('trading.position_overrides') IS NOT NULL AND EXISTS (SELECT 1 FROM trading.position_overrides) THEN
        RAISE EXCEPTION 'trading.position_overrides holds rows; refusing to roll 023 back';
    END IF;
END $$;

DROP TABLE IF EXISTS trading.position_overrides CASCADE;
DROP FUNCTION IF EXISTS trading.position_overrides_guard();

DELETE FROM trading.strategy_registry WHERE id IN ('qt_conservative', 'qt_conservative_model');
ALTER TABLE trading.strategy_registry DROP COLUMN IF EXISTS desk_editable;
ALTER TABLE trading.strategy_registry DROP COLUMN IF EXISTS portfolio_group;

ALTER TABLE trading.live_results DROP CONSTRAINT IF EXISTS live_results_book_source_check;
ALTER TABLE trading.live_results DROP COLUMN IF EXISTS book_source;

COMMIT;
