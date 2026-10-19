-- D-38 (2026-10-16): remove the end-to-end test book QT_E2E_PORTFOLIO from production. Run once on
-- new_algo_data by Dominick, after HD has read it. scripts/qt_e2e.sh now refuses production and
-- runs against a scratch database only, so nothing writes these rows again.
--
-- What it does: deletes QT_E2E_PORTFOLIO's rows (all three books, the published run record of
-- 2026-09-23 and its trading-days metadata) from the trading tables. Its command-log rows in
-- trading.position_overrides cannot be deleted (a trigger forbids it, C4) and stay as history; its
-- strategy_registry and strategy_config rows, if any, are left as they are.
--
-- It refuses any database but new_algo_data, prints the counts first, and runs in one
-- transaction: compare the counts with the DELETE results before COMMIT is reached.
\set ON_ERROR_STOP on
BEGIN;
DO $$ BEGIN
  IF current_database() <> 'new_algo_data' THEN
    RAISE EXCEPTION 'refusing: this script is for new_algo_data, not %', current_database();
  END IF;
END $$;
SELECT 'positions' AS tbl, count(*) FROM trading.positions WHERE portfolio_id = 'QT_E2E_PORTFOLIO'
UNION ALL SELECT 'executions', count(*) FROM trading.executions WHERE portfolio_id = 'QT_E2E_PORTFOLIO'
UNION ALL SELECT 'live_results', count(*) FROM trading.live_results WHERE portfolio_id = 'QT_E2E_PORTFOLIO'
UNION ALL SELECT 'equity_curve', count(*) FROM trading.equity_curve WHERE portfolio_id = 'QT_E2E_PORTFOLIO'
UNION ALL SELECT 'signals', count(*) FROM trading.signals WHERE portfolio_id = 'QT_E2E_PORTFOLIO'
UNION ALL SELECT 'live_run_metadata', count(*) FROM trading.live_run_metadata WHERE portfolio_id = 'QT_E2E_PORTFOLIO'
UNION ALL SELECT 'strategy_trading_days_metadata', count(*) FROM trading.strategy_trading_days_metadata WHERE portfolio_id = 'QT_E2E_PORTFOLIO';
DELETE FROM trading.positions WHERE portfolio_id = 'QT_E2E_PORTFOLIO';
DELETE FROM trading.executions WHERE portfolio_id = 'QT_E2E_PORTFOLIO';
DELETE FROM trading.live_results WHERE portfolio_id = 'QT_E2E_PORTFOLIO';
DELETE FROM trading.equity_curve WHERE portfolio_id = 'QT_E2E_PORTFOLIO';
DELETE FROM trading.signals WHERE portfolio_id = 'QT_E2E_PORTFOLIO';
DELETE FROM trading.live_run_metadata WHERE portfolio_id = 'QT_E2E_PORTFOLIO';
DELETE FROM trading.strategy_trading_days_metadata WHERE portfolio_id = 'QT_E2E_PORTFOLIO';
COMMIT;
