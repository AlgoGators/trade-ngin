-- SQL commands to drop columns from database schema
-- Execute these commands to remove the specified columns

-- 1. Drop position_pnl column from trading.executions table
-- Note: This command will fail if the column doesn't exist, which is safe
ALTER TABLE trading.executions DROP COLUMN IF EXISTS position_pnl;

-- 2. Drop previous_portfolio_value column from trading.live_results table  
-- Note: This command will fail if the column doesn't exist, which is safe
ALTER TABLE trading.live_results DROP COLUMN IF EXISTS previous_portfolio_value;

-- Optional: Add comments to document the changes
COMMENT ON TABLE trading.executions IS 'Trading executions table - position_pnl column removed';
COMMENT ON TABLE trading.live_results IS 'Live trading results table - previous_portfolio_value column removed';

-- Verification queries (optional - run these to confirm the columns are gone)
-- Check trading.executions table structure
-- SELECT column_name, data_type, is_nullable 
-- FROM information_schema.columns 
-- WHERE table_schema = 'trading' AND table_name = 'executions'
-- ORDER BY ordinal_position;

-- Check trading.live_results table structure  
-- SELECT column_name, data_type, is_nullable 
-- FROM information_schema.columns 
-- WHERE table_schema = 'trading' AND table_name = 'live_results'
-- ORDER BY ordinal_position;
