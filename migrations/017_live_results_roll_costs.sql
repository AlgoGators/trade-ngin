-- 017_live_results_roll_costs.sql
--
-- Add trading.live_results.daily_roll_costs (numeric NOT NULL DEFAULT 0) and total_roll_costs
-- (numeric(20,8) NOT NULL DEFAULT 0): the ROLL subset of the day's and the cumulative
-- transaction costs (LOOP_SPEC v6.1 sections 6.5 and 7; T-ROLLX commit 3). The pair is the
-- SUPERSET form: daily_transaction_costs / total_transaction_costs keep their meaning (every
-- fill's cost, STRATEGY + ROLL + BORROW) and the roll columns are the part of them paid on ROLL
-- legs; the strategy-only cost is the difference. No existing read moves. The types mirror
-- daily_transaction_costs (numeric) and total_transaction_costs (numeric(20,8)).
--
-- SAFETY: ADD COLUMN with a constant DEFAULT is metadata-only; every existing row reads 0 (no
-- roll was booked before this column); no key, index or view is touched; type-guarded;
-- transactional and idempotent.

BEGIN;
DO $$
DECLARE t text;
BEGIN
    IF to_regclass('trading.live_results') IS NULL THEN RAISE EXCEPTION 'trading.live_results does not exist'; END IF;
    FOR t IN SELECT data_type FROM information_schema.columns WHERE table_schema = 'trading' AND table_name = 'live_results'
              AND column_name IN ('daily_roll_costs', 'total_roll_costs') AND data_type <> 'numeric' LOOP
        RAISE EXCEPTION 'a live_results roll cost column exists with type %, not numeric', t;
    END LOOP;
END $$;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS daily_roll_costs numeric NOT NULL DEFAULT 0;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS total_roll_costs numeric(20,8) NOT NULL DEFAULT 0;
COMMENT ON COLUMN trading.live_results.daily_roll_costs IS
    'The part of daily_transaction_costs paid on ROLL legs (execution_type ROLL) on this date; an upper bound (rolls trade as spreads, modelled as two outright legs). daily_transaction_costs stays the superset. LOOP_SPEC v6.1 section 6.5; migration 017.';
COMMENT ON COLUMN trading.live_results.total_roll_costs IS
    'The cumulative daily_roll_costs (the previous row''s total plus this date''s); the part of total_transaction_costs paid on ROLL legs. Migration 017.';
COMMIT;
