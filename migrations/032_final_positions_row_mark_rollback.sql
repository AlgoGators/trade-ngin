-- 032_final_positions_row_mark_rollback.sql
--
-- Drops row_mark from backtest.final_positions (its CHECK and its comment go with it) and
-- restores the six comments 032 restated to the texts they carried before it, byte for byte:
-- trading.live_results.profit_factor and backtest.results.profit_factor (migration 028's texts),
-- backtest.results.sharpe_ratio, sortino_ratio and calmar_ratio (migration 031's texts), and the
-- table comment of backtest.signals.
-- Refuses while any row carries a mark unless the session sets migration.force_rollback = 'yes':
-- the marks are then lost (a run's marks are recomputable only by re-running it, or from the
-- bars: a row is a no-bar row when the run's bars hold no row of its symbol on its date).
-- The binary that ships with 032 names row_mark in every INSERT of the table: roll the binary
-- back first. No other column, value, key or index is touched. Idempotent.

BEGIN;
DO $$
DECLARE n bigint := 0;
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = 'backtest' AND table_name = 'final_positions' AND column_name = 'row_mark') THEN
        EXECUTE 'SELECT count(*) FROM backtest.final_positions WHERE row_mark IS NOT NULL' INTO n;
    END IF;
    IF n > 0 AND coalesce(current_setting('migration.force_rollback', true), '') <> 'yes' THEN
        RAISE EXCEPTION '032 rollback refused: % backtest.final_positions row(s) carry a row_mark', n;
    END IF;
END $$;
ALTER TABLE backtest.final_positions DROP CONSTRAINT IF EXISTS final_positions_row_mark_check;
ALTER TABLE backtest.final_positions DROP COLUMN IF EXISTS row_mark;
COMMENT ON COLUMN trading.live_results."profit_factor" IS $c028$gross_profit / gross_loss; 999.99 when there is profit and no loss; 0 with neither. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN backtest.results."profit_factor" IS $c028$The summed P&L of the winning trades over the summed loss of the others; 999 when there is profit and no loss. A trade and its P&L in dollars are defined on total_trades. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN backtest.results."sharpe_ratio" IS $c031$The mean grid return x K over volatility, risk-free rate 0; 0 when volatility is 0. Taken on the run's statistics grid with its sessions a year K (see the comment on the table). Rows written before migration 031 keep the earlier definition.$c031$;
COMMENT ON COLUMN backtest.results."sortino_ratio" IS $c031$The mean grid return x K over downside_volatility; 999 when there is no downside and the mean is not negative. Taken on the run's statistics grid with its sessions a year K (see the comment on the table). Rows written before migration 031 keep the earlier definition.$c031$;
COMMENT ON COLUMN backtest.results."calmar_ratio" IS $c031$The annualised return on the grid, the CAGR (1 + R)^(K / n) - 1 with R the return from the base to the last grid level and n the count of grid returns, over max_drawdown; 999 when there is no drawdown and the CAGR is not negative. Taken on the run's statistics grid with its sessions a year K (see the comment on the table). Rows written before migration 031 keep the earlier definition.$c031$;
COMMENT ON TABLE backtest.signals IS $c032$Strategy signals generated during backtest$c032$;
COMMIT;
