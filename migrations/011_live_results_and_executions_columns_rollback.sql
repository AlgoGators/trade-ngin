-- Rollback for 011_live_results_and_executions_columns.sql
--
-- THIS ONE DESTROYS DATA, AND SO IT REFUSES BY DEFAULT.
--
-- 011 only ever ADDs columns, so reverting it means DROPping them -- and by the
-- time anyone reverts, the engine has been writing into them. Dropping
-- sharpe_ratio discards every published Sharpe ratio in the table's history.
-- There is no way to put those back: they are computed from an equity curve
-- that is itself only stored to the precision the run wrote.
--
-- Reverting is also almost never what is wanted. 011 is idempotent and additive:
-- on a database that already had these columns it did nothing, so there is
-- nothing to undo. On a database that did not, the columns it added are the
-- ones the engine needs to write and AlgoLens needs to read, and removing them
-- breaks both. Downgrading the applications does not require removing them --
-- an older binary simply does not write them.
--
-- If a column really must go, set the guard below to FALSE, read the list, and
-- take a dump first.

BEGIN;

DO $$
DECLARE
    -- Set to FALSE, deliberately and with a backup in hand, to allow the drops.
    refuse boolean := TRUE;
    populated int;
BEGIN
    SELECT count(*) INTO populated
      FROM trading.live_results
     WHERE sharpe_ratio IS NOT NULL OR profit_factor IS NOT NULL
        OR max_drawdown IS NOT NULL OR win_rate IS NOT NULL;

    IF refuse THEN
        RAISE EXCEPTION
            'Refusing to drop % populated metric row(s). 011 is additive and idempotent; reverting it discards published metrics that cannot be recomputed. Edit the guard in this file only with a dump in hand.',
            populated;
    END IF;

    RAISE NOTICE 'Dropping columns added by 011 across % row(s) of published metrics.', populated;

    EXECUTE 'ALTER TABLE trading.live_results
        DROP COLUMN IF EXISTS sharpe_ratio,
        DROP COLUMN IF EXISTS sortino_ratio,
        DROP COLUMN IF EXISTS downside_deviation,
        DROP COLUMN IF EXISTS max_drawdown,
        DROP COLUMN IF EXISTS win_rate,
        DROP COLUMN IF EXISTS avg_win,
        DROP COLUMN IF EXISTS avg_loss,
        DROP COLUMN IF EXISTS best_day,
        DROP COLUMN IF EXISTS worst_day,
        DROP COLUMN IF EXISTS profit_factor,
        DROP COLUMN IF EXISTS gross_profit,
        DROP COLUMN IF EXISTS gross_loss,
        DROP COLUMN IF EXISTS winning_days,
        DROP COLUMN IF EXISTS losing_days,
        DROP COLUMN IF EXISTS total_days,
        DROP COLUMN IF EXISTS active_positions,
        DROP COLUMN IF EXISTS total_pnl,
        DROP COLUMN IF EXISTS daily_pnl,
        DROP COLUMN IF EXISTS daily_realized_pnl,
        DROP COLUMN IF EXISTS daily_unrealized_pnl,
        DROP COLUMN IF EXISTS daily_transaction_costs,
        DROP COLUMN IF EXISTS net_notional,
        DROP COLUMN IF EXISTS portfolio_var,
        DROP COLUMN IF EXISTS max_correlation,
        DROP COLUMN IF EXISTS jump_risk,
        DROP COLUMN IF EXISTS risk_scale';

    -- strategy_id and portfolio_leverage are NOT dropped. Both predate 011 on
    -- any database the engine has run against; 011 declares them only so a
    -- database built purely from this directory has them at all.

    EXECUTE 'ALTER TABLE trading.executions
        DROP COLUMN IF EXISTS exec_id,
        DROP COLUMN IF EXISTS order_id,
        DROP COLUMN IF EXISTS execution_time,
        DROP COLUMN IF EXISTS commissions_fees,
        DROP COLUMN IF EXISTS implicit_price_impact,
        DROP COLUMN IF EXISTS slippage_market_impact,
        DROP COLUMN IF EXISTS total_transaction_costs,
        DROP COLUMN IF EXISTS is_partial,
        DROP COLUMN IF EXISTS strategy_name,
        DROP COLUMN IF EXISTS date';
END
$$;

COMMIT;
