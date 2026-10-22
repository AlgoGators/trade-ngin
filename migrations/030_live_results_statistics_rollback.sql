-- 030_live_results_statistics_rollback.sql
--
-- Drops the twenty columns migration 030 added to trading.live_results (their comments go with
-- them). Refuses while any row carries a value in one of them unless the session sets
-- migration.force_rollback = 'yes': the values are then lost (the fourteen statistics are
-- recomputed by the next run after 030 is applied again; the six overlay columns of past rows
-- are not). No other column, value, key or index is touched. Idempotent.

BEGIN;
DO $$
DECLARE
    cols CONSTANT text[] := ARRAY['worst_day_date', 'worst_day_symbol', 'max_drawdown_sizing', 'volatility_sizing', 'worst_day_sizing', 'worst_day_sizing_date', 'worst_day_sizing_symbol', 'monthly_skew', 'monthly_tail_ratio', 'calendar_year_returns', 'losing_years', 'total_trades', 'total_strategy_fills', 'total_roll_fills', 'overlay_risk', 'overlay_risk_jump', 'overlay_risk_shock', 'overlay_gross_leverage', 'overlay_net_leverage', 'var_95_1d'];
    c text;
    n bigint;
    carried bigint := 0;
BEGIN
    FOREACH c IN ARRAY cols LOOP
        IF EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = 'trading' AND table_name = 'live_results' AND column_name = c) THEN
            EXECUTE format('SELECT count(*) FROM trading.live_results WHERE %I IS NOT NULL', c) INTO n;
            carried := carried + n;
        END IF;
    END LOOP;
    IF carried > 0 AND coalesce(current_setting('migration.force_rollback', true), '') <> 'yes' THEN
        RAISE EXCEPTION '030 rollback refused: % cell(s) of the migration 030 columns carry a value', carried;
    END IF;
END $$;
ALTER TABLE trading.live_results
    DROP COLUMN IF EXISTS worst_day_date,
    DROP COLUMN IF EXISTS worst_day_symbol,
    DROP COLUMN IF EXISTS max_drawdown_sizing,
    DROP COLUMN IF EXISTS volatility_sizing,
    DROP COLUMN IF EXISTS worst_day_sizing,
    DROP COLUMN IF EXISTS worst_day_sizing_date,
    DROP COLUMN IF EXISTS worst_day_sizing_symbol,
    DROP COLUMN IF EXISTS monthly_skew,
    DROP COLUMN IF EXISTS monthly_tail_ratio,
    DROP COLUMN IF EXISTS calendar_year_returns,
    DROP COLUMN IF EXISTS losing_years,
    DROP COLUMN IF EXISTS total_trades,
    DROP COLUMN IF EXISTS total_strategy_fills,
    DROP COLUMN IF EXISTS total_roll_fills,
    DROP COLUMN IF EXISTS overlay_risk,
    DROP COLUMN IF EXISTS overlay_risk_jump,
    DROP COLUMN IF EXISTS overlay_risk_shock,
    DROP COLUMN IF EXISTS overlay_gross_leverage,
    DROP COLUMN IF EXISTS overlay_net_leverage,
    DROP COLUMN IF EXISTS var_95_1d;
COMMIT;
