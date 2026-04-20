-- ============================================================================
-- Equity Mean Reversion Backtest Analysis Queries
-- Run ID: MEAN_REVERSION_20260405_035516_383
-- ============================================================================
-- Usage: Replace the run_id in each query, or set it once as a psql variable:
--   \set run_id 'MEAN_REVERSION_20260405_035516_383'
--   Then use :'run_id' in queries

-- ============================================================================
-- 1. SUMMARY METRICS (top-level performance)
-- ============================================================================
SELECT
    run_id,
    total_return,
    sharpe_ratio,
    sortino_ratio,
    max_drawdown,
    calmar_ratio,
    volatility,
    total_trades,
    win_rate,
    profit_factor,
    avg_win,
    avg_loss,
    max_win,
    max_loss,
    avg_holding_period,
    var_95,
    cvar_95
FROM backtest.results
WHERE run_id = 'MEAN_REVERSION_20260405_035516_383';

-- ============================================================================
-- 2. PNL ACCOUNTING VERIFICATION
-- Checks that our fix is working: average_price should reflect cost basis,
-- not bar.close. Unrealized PnL should be nonzero for open positions.
-- ============================================================================

-- 2a. Positions with nonzero unrealized PnL (should exist now -- was always ~0 before fix)
SELECT
    date,
    symbol,
    quantity,
    average_price,
    unrealized_pnl,
    realized_pnl
FROM backtest.final_positions
WHERE run_id = 'MEAN_REVERSION_20260405_035516_383'
  AND ABS(unrealized_pnl) > 0.01
ORDER BY date DESC
LIMIT 50;

-- 2b. Daily PnL breakdown: realized vs unrealized over time
SELECT
    date,
    SUM(realized_pnl) AS total_realized,
    SUM(unrealized_pnl) AS total_unrealized,
    SUM(realized_pnl + unrealized_pnl) AS total_pnl
FROM backtest.final_positions
WHERE run_id = 'MEAN_REVERSION_20260405_035516_383'
GROUP BY date
ORDER BY date;

-- 2c. PnL invariant check: portfolio_value should equal initial_capital + realized + unrealized
-- Compare equity curve values against position-level PnL
SELECT
    ec.timestamp::date AS date,
    ec.equity AS portfolio_value,
    fp.total_realized,
    fp.total_unrealized,
    fp.total_realized + fp.total_unrealized AS implied_pnl,
    ec.equity - 100000.0 AS actual_pnl,  -- adjust 100000 to your initial capital
    ABS((ec.equity - 100000.0) - (fp.total_realized + fp.total_unrealized)) AS invariant_error
FROM backtest.equity_curve ec
LEFT JOIN (
    SELECT
        date,
        SUM(realized_pnl) AS total_realized,
        SUM(unrealized_pnl) AS total_unrealized
    FROM backtest.final_positions
    WHERE run_id = 'MEAN_REVERSION_20260405_035516_383'
    GROUP BY date
) fp ON ec.timestamp::date = fp.date
WHERE ec.run_id = 'MEAN_REVERSION_20260405_035516_383'
ORDER BY ec.timestamp
LIMIT 100;

-- ============================================================================
-- 3. TRANSACTION COST ANALYSIS
-- Verify costs are reasonable (not 100x/300x inflated for unknown equities)
-- ============================================================================

-- 3a. Transaction cost summary per symbol
SELECT
    symbol,
    COUNT(*) AS num_trades,
    SUM(quantity) AS total_shares_traded,
    AVG(price) AS avg_price,
    SUM(commissions_fees) AS total_commissions,
    SUM(slippage_market_impact) AS total_slippage,
    SUM(total_transaction_costs) AS total_costs,
    AVG(total_transaction_costs / NULLIF(quantity * price, 0)) * 10000 AS avg_cost_bps
FROM backtest.executions
WHERE run_id = 'MEAN_REVERSION_20260405_035516_383'
GROUP BY symbol
ORDER BY total_costs DESC;

-- 3b. Commission per share (should be ~$0.005 for equities, NOT $1.50)
SELECT
    symbol,
    AVG(commissions_fees / NULLIF(quantity, 0)) AS avg_commission_per_share,
    MIN(commissions_fees / NULLIF(quantity, 0)) AS min_commission_per_share,
    MAX(commissions_fees / NULLIF(quantity, 0)) AS max_commission_per_share
FROM backtest.executions
WHERE run_id = 'MEAN_REVERSION_20260405_035516_383'
  AND quantity > 0
GROUP BY symbol
ORDER BY symbol;

-- 3c. Cost as percentage of trade value (should be under 0.5% for most trades)
SELECT
    CASE
        WHEN total_transaction_costs / NULLIF(quantity * price, 0) < 0.001 THEN '< 0.1%'
        WHEN total_transaction_costs / NULLIF(quantity * price, 0) < 0.005 THEN '0.1% - 0.5%'
        WHEN total_transaction_costs / NULLIF(quantity * price, 0) < 0.01 THEN '0.5% - 1.0%'
        ELSE '> 1.0%'
    END AS cost_bucket,
    COUNT(*) AS num_trades,
    AVG(total_transaction_costs) AS avg_cost,
    AVG(quantity * price) AS avg_trade_value
FROM backtest.executions
WHERE run_id = 'MEAN_REVERSION_20260405_035516_383'
GROUP BY cost_bucket
ORDER BY cost_bucket;

-- ============================================================================
-- 4. POSITION LIFECYCLE ANALYSIS
-- Check cost basis tracking, flip logic, and position evolution
-- ============================================================================

-- 4a. Average price stability: should NOT change daily (was the main bug)
-- Look for a single symbol to verify avg_price persists across days
SELECT
    date,
    symbol,
    quantity,
    average_price,
    unrealized_pnl,
    realized_pnl,
    LAG(average_price) OVER (PARTITION BY symbol ORDER BY date) AS prev_avg_price,
    CASE
        WHEN LAG(average_price) OVER (PARTITION BY symbol ORDER BY date) IS NOT NULL
         AND ABS(average_price - LAG(average_price) OVER (PARTITION BY symbol ORDER BY date)) > 0.01
         AND ABS(quantity) > 0.01
         AND ABS(LAG(quantity) OVER (PARTITION BY symbol ORDER BY date)) > 0.01
        THEN 'CHANGED'
        ELSE 'stable'
    END AS avg_price_status
FROM backtest.final_positions
WHERE run_id = 'MEAN_REVERSION_20260405_035516_383'
  AND symbol = 'AAPL'  -- pick any symbol
ORDER BY date;

-- 4b. Position flips (long to short or vice versa)
SELECT
    date,
    symbol,
    quantity,
    average_price,
    realized_pnl,
    LAG(quantity) OVER (PARTITION BY symbol ORDER BY date) AS prev_quantity
FROM backtest.final_positions
WHERE run_id = 'MEAN_REVERSION_20260405_035516_383'
  AND symbol IN (
    SELECT DISTINCT symbol FROM backtest.final_positions
    WHERE run_id = 'MEAN_REVERSION_20260405_035516_383'
    GROUP BY symbol
    HAVING MIN(quantity) < -0.01 AND MAX(quantity) > 0.01  -- had both long and short
  )
ORDER BY symbol, date;

-- ============================================================================
-- 5. EQUITY CURVE AND DRAWDOWN
-- ============================================================================

-- 5a. Monthly returns
SELECT
    DATE_TRUNC('month', timestamp) AS month,
    (LAST_VALUE(equity) OVER (PARTITION BY DATE_TRUNC('month', timestamp) ORDER BY timestamp
        ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) /
     FIRST_VALUE(equity) OVER (PARTITION BY DATE_TRUNC('month', timestamp) ORDER BY timestamp
        ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) - 1) * 100 AS monthly_return_pct
FROM backtest.equity_curve
WHERE run_id = 'MEAN_REVERSION_20260405_035516_383'
ORDER BY month;

-- 5b. Running drawdown
SELECT
    timestamp::date AS date,
    equity,
    MAX(equity) OVER (ORDER BY timestamp ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS peak,
    (equity / MAX(equity) OVER (ORDER BY timestamp ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) - 1) * 100 AS drawdown_pct
FROM backtest.equity_curve
WHERE run_id = 'MEAN_REVERSION_20260405_035516_383'
ORDER BY timestamp;

-- 5c. Worst drawdown periods
WITH drawdowns AS (
    SELECT
        timestamp::date AS date,
        equity,
        MAX(equity) OVER (ORDER BY timestamp ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS peak,
        (equity / MAX(equity) OVER (ORDER BY timestamp ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) - 1) AS dd
    FROM backtest.equity_curve
    WHERE run_id = 'MEAN_REVERSION_20260405_035516_383'
)
SELECT date, equity, peak, ROUND((dd * 100)::numeric, 2) AS drawdown_pct
FROM drawdowns
WHERE dd < -0.02  -- worse than 2%
ORDER BY dd ASC
LIMIT 20;

-- ============================================================================
-- 6. TRADE-LEVEL ANALYSIS
-- ============================================================================

-- 6a. Largest trades by notional value
SELECT
    timestamp::date AS date,
    symbol,
    side,
    quantity,
    price,
    quantity * price AS notional,
    commissions_fees,
    slippage_market_impact,
    total_transaction_costs
FROM backtest.executions
WHERE run_id = 'MEAN_REVERSION_20260405_035516_383'
ORDER BY quantity * price DESC
LIMIT 20;

-- 6b. Trade frequency by month
SELECT
    DATE_TRUNC('month', timestamp) AS month,
    COUNT(*) AS num_trades,
    SUM(CASE WHEN side = 'BUY' THEN 1 ELSE 0 END) AS buys,
    SUM(CASE WHEN side = 'SELL' THEN 1 ELSE 0 END) AS sells,
    SUM(total_transaction_costs) AS monthly_costs
FROM backtest.executions
WHERE run_id = 'MEAN_REVERSION_20260405_035516_383'
GROUP BY month
ORDER BY month;

-- 6c. Per-symbol P&L attribution (final realized PnL per symbol)
SELECT
    symbol,
    SUM(realized_pnl) AS total_realized_pnl,
    SUM(unrealized_pnl) AS final_unrealized_pnl,
    SUM(realized_pnl + unrealized_pnl) AS total_pnl
FROM backtest.final_positions
WHERE run_id = 'MEAN_REVERSION_20260405_035516_383'
  AND date = (SELECT MAX(date) FROM backtest.final_positions
              WHERE run_id = 'MEAN_REVERSION_20260405_035516_383')
GROUP BY symbol
ORDER BY total_pnl DESC;