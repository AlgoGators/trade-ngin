-- Declare the columns the engine writes and AlgoLens reads.
--
-- WHY THIS EXISTS
-- ---------------
-- No migration in this repository creates trading.live_results or
-- trading.executions, and none ever added a column to either. Their shape
-- exists only as whatever was done by hand on the box that runs the engine.
-- Migration 001 ALTERs trading.positions and trading.equity_curve; these two
-- tables it does not touch.
--
-- That was survivable while the engine was the only reader of its own writes.
-- It stopped being survivable when AlgoLens started reading ten published
-- metrics out of live_results instead of recomputing them -- sharpe_ratio,
-- sortino_ratio, downside_deviation, max_drawdown, win_rate, avg_win, avg_loss,
-- profit_factor, best_day and worst_day. Deploying that build against a
-- database without those columns is a 500 on the dashboard, and nothing in
-- either repository would have added them.
--
-- Building a database from this directory's migrations and checking it against
-- AlgoLens's schema contract reports fourteen mismatches. This closes twelve of
-- them. The other two are futures_data.ohlcv_1d and metadata.contract_metadata,
-- which belong to data-ngin and are correctly not created here.
--
-- Migration 010 could not run on such a database either: it UPDATEs
-- profit_factor, which did not exist. Apply this one first, or 010's guard --
-- added in the same change -- will skip it and it can be re-run afterwards.
--
--
-- WHAT IS DECLARED, AND FROM WHERE
-- --------------------------------
-- Every key in the metric maps the live runner assembles
-- (apps/strategies/live_portfolio_runner.cpp), because
-- store_live_results_complete builds its INSERT column list from those maps and
-- a missing column fails the whole statement -- not just that metric. Plus the
-- fixed column list of the executions INSERT
-- (src/data/postgres_database.cpp, store_execution).
--
-- NUMERIC without precision throughout, matching what migrations 004, 005 and
-- 009 use for money and ratios in this schema.
--
--
-- SAFETY
-- ------
-- ADD COLUMN IF NOT EXISTS, every one nullable with no default. On a database
-- that already has these -- which is what a working production box is -- this
-- migration does nothing at all. It cannot lose data and it cannot change a
-- value. Idempotent and transactional.
--
-- It does NOT create the tables. A database with no trading.live_results has
-- never run the engine, and inventing the table here would guess at the primary
-- key, the uniqueness constraint and the identity column. This migration
-- declares columns on a table the engine already made.

BEGIN;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.tables
         WHERE table_schema = 'trading' AND table_name = 'live_results'
    ) THEN
        RAISE EXCEPTION 'trading.live_results does not exist. This migration adds columns to it; it does not create it. Run the engine once, or restore the table, before applying this.';
    END IF;
END
$$;

-- Identity. The engine keys rows on (portfolio_id, strategy_id, date) and
-- store_live_results_complete always supplies strategy_id.
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS strategy_id TEXT;

-- Risk-adjusted performance, since inception. These ten are the ones AlgoLens
-- reads instead of recomputing.
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS sharpe_ratio NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS sortino_ratio NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS downside_deviation NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS max_drawdown NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS win_rate NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS avg_win NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS avg_loss NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS best_day NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS worst_day NUMERIC;

-- profit_factor is nullable for a reason, not by omission: a book with no
-- losing day has no denominator and therefore no profit factor. The engine used
-- to write 999.99 there; migration 010 clears what is left of that.
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS profit_factor NUMERIC;

-- Gross profit and loss, which are what a reader should use when the ratio
-- above is absent.
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS gross_profit NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS gross_loss NUMERIC;

-- Day counts. total_days is the authoritative trading-day count the runner
-- overrides; flat days are total - winning - losing and are deliberately not
-- stored.
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS winning_days INTEGER;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS losing_days INTEGER;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS total_days INTEGER;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS active_positions INTEGER;

-- P&L and costs, daily and cumulative.
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS total_pnl NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS daily_pnl NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS daily_realized_pnl NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS daily_unrealized_pnl NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS daily_transaction_costs NUMERIC;

-- Exposure and risk.
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS net_notional NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS portfolio_var NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS max_correlation NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS jump_risk NUMERIC;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS risk_scale NUMERIC;

-- portfolio_leverage is where gross leverage goes now. gross_leverage is left
-- alone: the engine stopped writing it and dropping it would break a reader
-- nobody has audited, but nothing should read it in preference to this.
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS portfolio_leverage NUMERIC;


DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.tables
         WHERE table_schema = 'trading' AND table_name = 'executions'
    ) THEN
        RAISE EXCEPTION 'trading.executions does not exist. This migration adds columns to it; it does not create it.';
    END IF;
END
$$;

-- The fixed column list of PostgresDatabase::store_execution. AlgoLens reads
-- symbol, side, quantity, price, execution_time, commissions_fees, strategy_id
-- and portfolio_id; the rest are declared because the engine's INSERT names
-- them and would fail on the whole row without them.
ALTER TABLE trading.executions ADD COLUMN IF NOT EXISTS exec_id TEXT;
ALTER TABLE trading.executions ADD COLUMN IF NOT EXISTS order_id TEXT;
ALTER TABLE trading.executions ADD COLUMN IF NOT EXISTS execution_time TIMESTAMPTZ;
ALTER TABLE trading.executions ADD COLUMN IF NOT EXISTS commissions_fees NUMERIC;
ALTER TABLE trading.executions ADD COLUMN IF NOT EXISTS implicit_price_impact NUMERIC;
ALTER TABLE trading.executions ADD COLUMN IF NOT EXISTS slippage_market_impact NUMERIC;
ALTER TABLE trading.executions ADD COLUMN IF NOT EXISTS total_transaction_costs NUMERIC;
ALTER TABLE trading.executions ADD COLUMN IF NOT EXISTS is_partial BOOLEAN;
ALTER TABLE trading.executions ADD COLUMN IF NOT EXISTS strategy_name TEXT;
ALTER TABLE trading.executions ADD COLUMN IF NOT EXISTS date DATE;

COMMIT;
