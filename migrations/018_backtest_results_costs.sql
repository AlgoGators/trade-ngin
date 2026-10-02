-- 018_backtest_results_costs.sql
--
-- Add backtest.results.transaction_costs (double precision NOT NULL DEFAULT 0), roll_costs
-- (double precision NOT NULL DEFAULT 0) and total_roll_fills (integer NOT NULL DEFAULT 0): the
-- run's cost totals, the first cost columns of this table (LOOP_SPEC v6.1 section 7; T-ROLLX
-- commit 3). transaction_costs is the SUPERSET (every fill's cost: STRATEGY + ROLL + BORROW, the
-- sum the equity curve charged), roll_costs the ROLL subset, total_roll_fills the COUNT of ROLL
-- rows (two per held contract per sleeve per confirmed roll).
--
-- SAFETY: ADD COLUMN with a constant DEFAULT is metadata-only; existing rows read 0; no key,
-- index or view is touched; type-guarded; transactional and idempotent.

BEGIN;
DO $$
DECLARE t text;
BEGIN
    IF to_regclass('backtest.results') IS NULL THEN RAISE EXCEPTION 'backtest.results does not exist'; END IF;
    FOR t IN SELECT column_name || ' ' || data_type FROM information_schema.columns WHERE table_schema = 'backtest' AND table_name = 'results'
              AND ((column_name IN ('transaction_costs', 'roll_costs') AND data_type <> 'double precision')
                   OR (column_name = 'total_roll_fills' AND data_type <> 'integer')) LOOP
        RAISE EXCEPTION 'a backtest.results cost column exists with another type: %', t;
    END LOOP;
END $$;
ALTER TABLE backtest.results ADD COLUMN IF NOT EXISTS transaction_costs double precision NOT NULL DEFAULT 0;
ALTER TABLE backtest.results ADD COLUMN IF NOT EXISTS roll_costs double precision NOT NULL DEFAULT 0;
ALTER TABLE backtest.results ADD COLUMN IF NOT EXISTS total_roll_fills integer NOT NULL DEFAULT 0;
COMMENT ON COLUMN backtest.results.transaction_costs IS
    'The run''s transaction costs: every fill''s total_transaction_costs (STRATEGY + ROLL + BORROW), the sum the equity curve charged. Migration 018.';
COMMENT ON COLUMN backtest.results.roll_costs IS
    'The part of transaction_costs paid on ROLL legs (an upper bound: two outright legs per roll). Migration 018.';
COMMENT ON COLUMN backtest.results.total_roll_fills IS
    'The count of ROLL execution rows of the run (two per held contract per sleeve per confirmed roll). Migration 018.';
COMMIT;
