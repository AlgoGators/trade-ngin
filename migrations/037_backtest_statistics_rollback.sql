-- 037_backtest_statistics_rollback.sql
-- Numbered 031 until 2026-10-21; renumbered 037 when main-nonQT joined main (main has its own 028 and 029).
-- The text below is unchanged, so it still calls its migration 031.
--
-- Drops the thirteen columns migration 031 added to backtest.results and the six it added to
-- backtest.equity_curve (their comments go with them), and restores the fourteen comments 031
-- restated to migration 028's text, byte for byte (the table comment of backtest.results, its
-- eleven return statistics and total_trades, and trading.live_results.total_dividend_income).
-- Refuses while any row carries a value in one of the nineteen columns unless the session sets
-- migration.force_rollback = 'yes': the values are then lost (a run's statistics and readings
-- are recomputable only by re-running it). No other column, value, key or index is touched.
-- Idempotent.

BEGIN;
DO $$
DECLARE
    results_cols CONSTANT text[] := ARRAY['worst_day', 'worst_day_date', 'worst_day_symbol', 'max_drawdown_sizing', 'volatility_sizing', 'worst_day_sizing', 'worst_day_sizing_date', 'worst_day_sizing_symbol', 'monthly_skew', 'monthly_tail_ratio', 'calendar_year_returns', 'losing_years', 'total_strategy_fills'];
    curve_cols CONSTANT text[] := ARRAY['overlay_risk', 'overlay_risk_jump', 'overlay_risk_shock', 'overlay_gross_leverage', 'overlay_net_leverage', 'var_95_1d'];
    c text;
    n bigint;
    carried bigint := 0;
BEGIN
    FOREACH c IN ARRAY results_cols LOOP
        IF EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = 'backtest' AND table_name = 'results' AND column_name = c) THEN
            EXECUTE format('SELECT count(*) FROM backtest.results WHERE %I IS NOT NULL', c) INTO n;
            carried := carried + n;
        END IF;
    END LOOP;
    FOREACH c IN ARRAY curve_cols LOOP
        IF EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = 'backtest' AND table_name = 'equity_curve' AND column_name = c) THEN
            EXECUTE format('SELECT count(*) FROM backtest.equity_curve WHERE %I IS NOT NULL', c) INTO n;
            carried := carried + n;
        END IF;
    END LOOP;
    IF carried > 0 AND coalesce(current_setting('migration.force_rollback', true), '') <> 'yes' THEN
        RAISE EXCEPTION '031 rollback refused: % cell(s) of the migration 031 columns carry a value', carried;
    END IF;
END $$;
ALTER TABLE backtest.results
    DROP COLUMN IF EXISTS worst_day,
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
    DROP COLUMN IF EXISTS total_strategy_fills;
ALTER TABLE backtest.equity_curve
    DROP COLUMN IF EXISTS overlay_risk,
    DROP COLUMN IF EXISTS overlay_risk_jump,
    DROP COLUMN IF EXISTS overlay_risk_shock,
    DROP COLUMN IF EXISTS overlay_gross_leverage,
    DROP COLUMN IF EXISTS overlay_net_leverage,
    DROP COLUMN IF EXISTS var_95_1d;
COMMENT ON TABLE backtest.results IS $c028$One row per run: the run's performance statistics. THE RETURN STATISTICS (total_return, volatility, sharpe_ratio, sortino_ratio, downside_volatility, max_drawdown, calmar_ratio, var_95, cvar_95, beta, correlation) are FRACTIONS (trading.live_results stores percent). Taken on the run's own equity-curve rows after the warm-up (one row per UTC date, the last), annualised with 252: at migration 028 the backtest statistics are not on the live statistics grid or its K (trading.live_results); that alignment belongs to a later migration. THE TRADE STATISTICS (total_trades, win_rate, profit_factor, avg_win, avg_loss, max_win, max_loss, avg_holding_period) are the book's account fills after netting between sleeves, in dollars. A trade is an ACCOUNT fill that reduces, closes or flips a non-zero net position in a contract. Account fills are the book's: the STRATEGY rows of one contract on one bar netted across the sleeves (a full cross is no fill, a partial offset is one fill of the net size); ROLL and BORROW rows are never a trade. A trade's P&L is in DOLLARS: closed quantity x (fill price - average entry price) x the side held x the point value, less the closing fill's cost after netting; the opening fills' costs are not inside a trade, with one exception: a fill through zero (a reversal) is ONE account fill with one cost, and its whole cost, the part that opens the new position included, is charged to the trade it closes. Rows written before migration 028 keep the earlier definitions.$c028$;
COMMENT ON COLUMN backtest.results."total_return" IS $c028$Fraction. (last level - first level) / first level of the equity curve after the warm-up; not annualised. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."volatility" IS $c028$Fraction, annualised: the population standard deviation (n) of the curve's returns x sqrt(252). Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."sharpe_ratio" IS $c028$The mean return x 252 over volatility, risk-free rate 0; 0 when volatility is 0. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."downside_volatility" IS $c028$Fraction, annualised: sqrt(the sum of min(r, 0)^2 over EVERY return / n) x sqrt(252), target 0; a return at or above 0 counts as a zero. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table). Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN backtest.results."sortino_ratio" IS $c028$The mean return x 252 over downside_volatility; 999 when there is no downside and the mean is not negative. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table). Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN backtest.results."max_drawdown" IS $c028$Fraction, positive: the largest fall from a running peak of the equity curve after the warm-up, the peak starting at the first level after the warm-up. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."calmar_ratio" IS $c028$The mean return x 252 over max_drawdown; 999 when there is no drawdown and the mean is not negative. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."var_95" IS $c028$Fraction of one day, positive for a loss: minus the return at position floor(0.05 x n) of the returns sorted ascending. Historical, not parametric. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."cvar_95" IS $c028$Fraction of one day, positive for a loss: minus the mean of the floor(0.05 x n) lowest returns (at least one); the var_95 observation itself is not among them. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."beta" IS $c028$NOT a market beta: the slope of the regression of a day's return on the previous day's return, of the book's own returns (lag-1 autocorrelation of daily returns). No benchmark enters it. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."correlation" IS $c028$NOT a correlation with a market: the lag-1 autocorrelation of the book's own daily returns. No benchmark enters it. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."total_trades" IS $c028$The count of trades (round trips), not the count of backtest.executions rows. A trade is an ACCOUNT fill that reduces, closes or flips a non-zero net position in a contract. Account fills are the book's: the STRATEGY rows of one contract on one bar netted across the sleeves (a full cross is no fill, a partial offset is one fill of the net size); ROLL and BORROW rows are never a trade. A trade's P&L is in DOLLARS: closed quantity x (fill price - average entry price) x the side held x the point value, less the closing fill's cost after netting; the opening fills' costs are not inside a trade, with one exception: a fill through zero (a reversal) is ONE account fill with one cost, and its whole cost, the part that opens the new position included, is charged to the trade it closes. A position under 1e-9 contracts or shares after a fill is flat. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."total_dividend_income" IS $c028$Dollars, cumulative: the dividend cash of the book through this date. Each dividend event on record (trading.corp_action_applied) counts its dividend per share x the shares of the trading.positions row dated the ex-date, and is dated on the ex-date row: that row carries it once the next run has settled it, and every later row carries it. It need not equal the sum of corp_action_applied.total_cash, and the two differ for a reason: total_cash is computed on qty_held, the holding of the row dated the day BEFORE the ex-date, which does not yet hold the shares the run of the ex-date buys; that run fills at the close before the ex-date, the cum-dividend close, so the shares it buys carry the dividend and this column counts them. An event with no position row on its ex-date is counted on qty_held. A run that cannot read the dividend record or an ex-date position row logs an ERROR and writes no new figure: its own row carries the figure of the Day T-1 row and the Day T-1 row is left as it is. THIS COLUMN IS THE DIVIDEND INCOME (T-8D-2 R44 (b)): the cash a holder of the ex-date row's shares is paid. Informational ONLY and never added to total_pnl. NO DIVIDEND CASH IS CREDITED ANYWHERE in the equity book: no cash row is written and no P&L, value or equity-curve column has a dividend term. What a dividend does to the book is one thing: the run after the ex-date divides the average_price of the ex-date row by 1 + dividend / the raw close of the ex-date, and the bars are total-return adjusted, so the ex-date row's unrealised P&L rises by shares x average_price x dividend / (close + dividend). That amount is basis-weighted, not cash: it is a third figure, equal neither to this column's nor to total_cash (META, ex-date 2026-06-15: 3.606652 in the P&L; 3.752281 here, 7.147202 shares x 0.525; 3.742124 in total_cash, 7.127855 shares x 0.525). Written by the equity runner; the futures runners do not write it. Rows written before migration 028 keep the earlier figure: the sum of total_cash, first carried by the row after the ex-date.$c028$;
COMMIT;
