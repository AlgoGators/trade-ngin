-- Insert test positions for September 28th (previous day) for testing daily PnL calculations
-- According to CLAUDE.md, use today = 29th and previous day = 28th

-- First, clean up any existing test data for these dates
DELETE FROM trading.positions WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(last_update) IN ('2025-09-28', '2025-09-29');
DELETE FROM trading.executions WHERE DATE(execution_time) IN ('2025-09-28', '2025-09-29');
DELETE FROM trading.signals WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(timestamp) IN ('2025-09-28', '2025-09-29');
DELETE FROM trading.equity_curve WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(timestamp) IN ('2025-09-28', '2025-09-29');
DELETE FROM trading.live_results WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(date) IN ('2025-09-28', '2025-09-29');

-- Insert test positions for September 28th (previous day)
-- These positions will be used to test daily PnL calculations
INSERT INTO trading.positions
(symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl, last_update, updated_at, strategy_id)
VALUES
-- Some existing positions that will change
('CL.v.0', 10.0, 70.50, 0.0, 500.0, '2025-09-28 16:00:00-04', '2025-09-28 16:00:00-04', 'LIVE_TREND_FOLLOWING'),
('GC.v.0', 5.0, 1920.00, 0.0, 250.0, '2025-09-28 16:00:00-04', '2025-09-28 16:00:00-04', 'LIVE_TREND_FOLLOWING'),
('NQ.v.0', -3.0, 15500.00, 0.0, -300.0, '2025-09-28 16:00:00-04', '2025-09-28 16:00:00-04', 'LIVE_TREND_FOLLOWING'),
-- Position that will be closed today
('ES.v.0', 2.0, 4400.00, 0.0, 100.0, '2025-09-28 16:00:00-04', '2025-09-28 16:00:00-04', 'LIVE_TREND_FOLLOWING'),
-- Position that will remain unchanged
('ZN.v.0', 8.0, 110.25, 0.0, 50.0, '2025-09-28 16:00:00-04', '2025-09-28 16:00:00-04', 'LIVE_TREND_FOLLOWING');

-- Insert a previous live_results entry for the 28th to test portfolio value continuity
INSERT INTO trading.live_results
(strategy_id, date, total_return, volatility, total_pnl, unrealized_pnl, realized_pnl,
 current_portfolio_value, portfolio_var, gross_leverage, net_leverage, portfolio_leverage,
 max_correlation, jump_risk, risk_scale, gross_notional, net_notional, active_positions,
 config, daily_pnl, total_commissions, daily_realized_pnl, daily_unrealized_pnl)
VALUES
('LIVE_TREND_FOLLOWING', '2025-09-28 16:00:00', 0.12, 0.15, 600.0, 0.0, 600.0,
 500600.0, 0.15, 1.5, 1.2, 1.3, 0.4, 0.05, 1.0, 650000.0, 600000.0, 5,
 '{"test": true}', 600.0, 50.0, 600.0, 0.0);

-- Verify the inserted data
SELECT 'Previous day positions:' as info;
SELECT symbol, quantity, average_price, daily_realized_pnl, last_update
FROM trading.positions
WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(last_update) = '2025-09-28'
ORDER BY symbol;

SELECT 'Previous day live_results:' as info;
SELECT date, current_portfolio_value, total_pnl, daily_pnl
FROM trading.live_results
WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(date) = '2025-09-28';