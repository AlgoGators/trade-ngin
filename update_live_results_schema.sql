-- Update trading.live_results table schema
-- Run these commands to add the required columns for proper PnL tracking

-- Add daily PnL breakdown columns
ALTER TABLE trading.live_results ADD COLUMN daily_realized_pnl NUMERIC DEFAULT 0.0;
ALTER TABLE trading.live_results ADD COLUMN daily_unrealized_pnl NUMERIC DEFAULT 0.0;

-- Break down total notional into gross and net
ALTER TABLE trading.live_results ADD COLUMN gross_notional NUMERIC DEFAULT 0.0;
ALTER TABLE trading.live_results ADD COLUMN net_notional NUMERIC DEFAULT 0.0;

-- Update column comments for clarity
COMMENT ON COLUMN trading.live_results.total_return IS 'Daily return percentage for this day';
COMMENT ON COLUMN trading.live_results.total_pnl IS 'Cumulative total PnL since inception';
COMMENT ON COLUMN trading.live_results.unrealized_pnl IS 'Cumulative unrealized PnL';
COMMENT ON COLUMN trading.live_results.realized_pnl IS 'Cumulative realized PnL';
COMMENT ON COLUMN trading.live_results.daily_pnl IS 'Total daily PnL (realized + unrealized)';
COMMENT ON COLUMN trading.live_results.daily_realized_pnl IS 'Daily realized PnL for this day';
COMMENT ON COLUMN trading.live_results.daily_unrealized_pnl IS 'Daily unrealized PnL for this day';
COMMENT ON COLUMN trading.live_results.gross_notional IS 'Sum of absolute notional values';
COMMENT ON COLUMN trading.live_results.net_notional IS 'Net notional (long - short)';
COMMENT ON COLUMN trading.live_results.total_notional IS 'Deprecated - use gross_notional instead';