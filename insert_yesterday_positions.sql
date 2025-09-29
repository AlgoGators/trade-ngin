-- SQL commands to insert positions for yesterday (2025-01-27) with different values
-- This will allow you to track changes and see how the system works

-- First, clear any existing positions for yesterday for the strategy
DELETE FROM trading.positions 
WHERE strategy_id = 'trend_following' 
AND DATE(last_update) = '2025-01-27';

-- Insert positions for yesterday with subtle changes to track strategy behavior
INSERT INTO trading.positions 
(symbol, quantity, average_price, unrealized_pnl, realized_pnl, last_update, updated_at, strategy_id) 
VALUES 
-- NQ position: Same quantity, slightly different price
('NQ.v.0', 1, 24750.25, 23.75, 0.00, '2025-01-27 16:00:00', '2025-01-27 16:00:00', 'trend_following'),

-- ZR position: CLOSED OUT - quantity set to 0 (position cancelled out)
('ZR.v.0', 0, 11.28, 0.00, 0.00, '2025-01-27 16:00:00', '2025-01-27 16:00:00', 'trend_following'),

-- 6M position: Increased quantity from 2 to 3
('6M.v.0', 3, 0.05, 0.00, 0.00, '2025-01-27 16:00:00', '2025-01-27 16:00:00', 'trend_following'),

-- PL position: Same quantity, different price
('PL.v.0', 1, 1595.30, 5.50, 0.00, '2025-01-27 16:00:00', '2025-01-27 16:00:00', 'trend_following'),

-- ZM position: Same quantity, slightly different price
('ZM.v.0', -1, 275.25, -0.35, 0.00, '2025-01-27 16:00:00', '2025-01-27 16:00:00', 'trend_following'),

-- RB position: Same quantity, different price
('RB.v.0', -1, 2.05, -0.07, 0.00, '2025-01-27 16:00:00', '2025-01-27 16:00:00', 'trend_following'),

-- RTY position: Same quantity, slightly different price
('RTY.v.0', 1, 2455.80, 6.60, 0.00, '2025-01-27 16:00:00', '2025-01-27 16:00:00', 'trend_following'),

-- 6N position: Same quantity, different price
('6N.v.0', -1, 0.61, -0.03, 0.00, '2025-01-27 16:00:00', '2025-01-27 16:00:00', 'trend_following'),

-- 6C position: Same quantity, different price
('6C.v.0', -1, 0.74, 0.02, 0.00, '2025-01-27 16:00:00', '2025-01-27 16:00:00', 'trend_following'),

-- YM position: Increased quantity from 4 to 5
('YM.v.0', 5, 46555.00, 0.00, 0.00, '2025-01-27 16:00:00', '2025-01-27 16:00:00', 'trend_following'),

-- NEW POSITION: ES.v.0 - New position opened (not in today's list)
('ES.v.0', -2, 4895.50, 210.00, 0.00, '2025-01-27 16:00:00', '2025-01-27 16:00:00', 'trend_following');

-- Query to verify the inserted positions
SELECT 
    symbol,
    quantity,
    average_price,
    unrealized_pnl,
    realized_pnl,
    last_update,
    strategy_id
FROM trading.positions 
WHERE strategy_id = 'trend_following' 
AND DATE(last_update) = '2025-01-27'
ORDER BY symbol;

-- Summary query to compare with today's positions
SELECT 
    COUNT(*) as active_positions,
    SUM(quantity * average_price) as total_notional,
    SUM(unrealized_pnl) as total_unrealized_pnl,
    SUM(realized_pnl) as total_realized_pnl
FROM trading.positions 
WHERE strategy_id = 'trend_following' 
AND DATE(last_update) = '2025-01-27';
