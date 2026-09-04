-- Rollback for 002_corp_action_applied.sql.
--
-- Dropping this table returns the applier to file-based dedup. That is safe
-- only if <state_dir>/applied_corp_actions.json still exists and is current;
-- otherwise the next run re-applies every event inside its window. Check
-- before running this.
--
-- Refuses to run silently against an un-migrated database.

BEGIN;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.tables
        WHERE table_schema = 'trading' AND table_name = 'corp_action_applied'
    ) THEN
        RAISE EXCEPTION 'migration 002 was never applied: trading.corp_action_applied does not exist';
    END IF;
END $$;

DROP TABLE IF EXISTS trading.corp_action_applied;

COMMIT;
