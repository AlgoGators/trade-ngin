-- Setup positions for September 28th ONLY to test daily PnL calculations
-- The 29th will have live values as shown by the user
-- Testing scenarios per CLAUDE.md:
-- 1. Position that increases (NQ.v.0: -1 -> 2, so we had -1 on 28th)
-- 2. Position that reverses (6M.v.0: new position on 29th, none on 28th)
-- 3. Position that closes (ES.v.0: exists on 28th, closes on 29th)
-- 4. Position that decreases (YM.v.0: 7 -> 5)
-- 5. Position that stays same (if any)

-- First, clean up any existing test data for the 28th
DELETE FROM trading.positions WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(last_update) = '2025-09-28';
DELETE FROM trading.executions WHERE DATE(execution_time) = '2025-09-28';
DELETE FROM trading.signals WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(timestamp) = '2025-09-28';
DELETE FROM trading.equity_curve WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(timestamp) = '2025-09-28';
DELETE FROM trading.live_results WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(date) = '2025-09-28';

-- Insert positions for September 28th
-- These will be compared against the 29th live positions to generate executions and calculate daily PnL
INSERT INTO trading.positions
(symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl, last_update, updated_at, strategy_id)
VALUES
-- NQ.v.0: Was short 1, will become long 2 (reversal + increase) on 29th
('NQ.v.0', -1.0, 24500.00, 0.0, 0.0, '2025-09-28 16:00:00-04', '2025-09-28 16:00:00-04', 'LIVE_TREND_FOLLOWING'),
-- YM.v.0: Had 7, will become 5 (decrease) on 29th
('YM.v.0', 7.0, 46500.00, 0.0, 0.0, '2025-09-28 16:00:00-04', '2025-09-28 16:00:00-04', 'LIVE_TREND_FOLLOWING'),
-- ES.v.0: Position that will be closed completely (exists on 28th, not on 29th)
('ES.v.0', 3.0, 5750.00, 0.0, 0.0, '2025-09-28 16:00:00-04', '2025-09-28 16:00:00-04', 'LIVE_TREND_FOLLOWING'),
-- ZR.v.0: Was long 2, will become short 1 (reversal) on 29th
('ZR.v.0', 2.0, 11.20, 0.0, 0.0, '2025-09-28 16:00:00-04', '2025-09-28 16:00:00-04', 'LIVE_TREND_FOLLOWING'),
-- RTY.v.0: Same position (1 contract) - no change expected
('RTY.v.0', 1.0, 2449.20, 0.0, 0.0, '2025-09-28 16:00:00-04', '2025-09-28 16:00:00-04', 'LIVE_TREND_FOLLOWING');

-- Note: 6M.v.0, PL.v.0, ZM.v.0, RB.v.0, 6N.v.0, 6C.v.0 are NEW positions on the 29th (not on 28th)

-- Insert a live_results entry for the 28th to establish previous portfolio value
-- This is important for calculating daily return correctly
INSERT INTO trading.live_results
(strategy_id, date, total_return, volatility, total_pnl, unrealized_pnl, realized_pnl,
 current_portfolio_value, portfolio_var, gross_leverage, net_leverage, portfolio_leverage,
 max_correlation, jump_risk, risk_scale, gross_notional, net_notional, active_positions,
 config, daily_pnl, total_commissions, daily_realized_pnl, daily_unrealized_pnl)
VALUES
('LIVE_TREND_FOLLOWING', '2025-09-28 16:00:00', 5.2, 0.15, 26000.0, 0.0, 26000.0,
 526000.0, 0.15, 1.8, 1.5, 1.6, 0.45, 0.06, 1.0, 950000.0, 900000.0, 5,
 '{"test": true, "note": "Previous day for testing daily PnL"}', 2500.0, 125.0, 2500.0, 0.0);

-- Verify the setup
SELECT 'Positions on Sep 28th (will be compared with Sep 29th live positions):' as info;
SELECT symbol, quantity, average_price,
       CASE
           WHEN symbol = 'NQ.v.0' THEN 'Will reverse from -1 to +2 (buy 3)'
           WHEN symbol = 'YM.v.0' THEN 'Will decrease from 7 to 5 (sell 2)'
           WHEN symbol = 'ES.v.0' THEN 'Will close completely (sell 3)'
           WHEN symbol = 'ZR.v.0' THEN 'Will reverse from +2 to -1 (sell 3)'
           WHEN symbol = 'RTY.v.0' THEN 'No change (stays at 1)'
       END as expected_change
FROM trading.positions
WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(last_update) = '2025-09-28'
ORDER BY symbol;

SELECT 'Portfolio value on Sep 28th:' as info;
SELECT date, current_portfolio_value, total_pnl, daily_pnl
FROM trading.live_results
WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(date) = '2025-09-28';

-- Expected executions on Sep 29th when live_trend runs:
-- 1. NQ.v.0: BUY 3 contracts (close -1, open +2)
-- 2. YM.v.0: SELL 2 contracts
-- 3. ES.v.0: SELL 3 contracts (close position)
-- 4. ZR.v.0: SELL 3 contracts (close +2, open -1)
-- 5. RTY.v.0: No execution (unchanged)
-- 6. New positions: 6M.v.0 (BUY 2), PL.v.0 (BUY 1), ZM.v.0 (SELL 1), RB.v.0 (SELL 1), 6N.v.0 (SELL 1), 6C.v.0 (SELL 1)

-- Daily PnL calculation on 29th should show:
-- Daily PnL = (Current Portfolio Value on 29th) - (Portfolio Value on 28th = 526000)
-- Total PnL = (Current Portfolio Value on 29th) - (Initial Capital = 500000)