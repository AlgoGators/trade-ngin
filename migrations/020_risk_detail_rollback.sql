-- 020_risk_detail_rollback.sql
--
-- Drops risk_detail from trading.live_results and backtest.equity_curve and removes the 020
-- COMMENT of trading.live_results.risk_scale. Refuses while any row carries a risk_detail value
-- unless the session sets migration.force_rollback = 'yes' (the values are then lost: they are
-- recomputable only by re-running the days). The risk_scale column and its values are untouched.

BEGIN;
DO $$
DECLARE n bigint := 0; m bigint := 0;
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = 'trading' AND table_name = 'live_results' AND column_name = 'risk_detail') THEN
        EXECUTE 'SELECT count(*) FROM trading.live_results WHERE risk_detail IS NOT NULL' INTO n;
    END IF;
    IF EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = 'backtest' AND table_name = 'equity_curve' AND column_name = 'risk_detail') THEN
        EXECUTE 'SELECT count(*) FROM backtest.equity_curve WHERE risk_detail IS NOT NULL' INTO m;
    END IF;
    IF n + m > 0 AND coalesce(current_setting('migration.force_rollback', true), '') <> 'yes' THEN
        RAISE EXCEPTION '020 rollback refused: % live_results row(s) and % equity_curve row(s) carry a risk_detail', n, m;
    END IF;
END $$;
ALTER TABLE trading.live_results DROP COLUMN IF EXISTS risk_detail;
ALTER TABLE backtest.equity_curve DROP COLUMN IF EXISTS risk_detail;
COMMENT ON COLUMN trading.live_results.risk_scale IS NULL;
COMMIT;
