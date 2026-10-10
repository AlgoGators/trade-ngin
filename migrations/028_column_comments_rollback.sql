-- 028_column_comments_rollback.sql
--
-- Restores, for every table and column migration 028 comments, the comment it had before 028:
-- NULL where it had none (73 objects), the earlier text where it had one (14 objects: the texts
-- of migrations 013 and 020 and the texts the tables were created with), byte for byte.
-- trading.live_results.risk_scale gets migration 020's text back exactly. COMMENT ON only: no
-- value moves. Transactional and idempotent.

BEGIN;
DO $$
DECLARE o text; p text[];
BEGIN
    FOREACH o IN ARRAY ARRAY[
        'trading.live_results',
        'trading.live_results.date',
        'trading.live_results.created_at',
        'trading.live_results.total_days',
        'trading.live_results.total_annualized_return',
        'trading.live_results.volatility',
        'trading.live_results.sharpe_ratio',
        'trading.live_results.downside_deviation',
        'trading.live_results.sortino_ratio',
        'trading.live_results.max_drawdown',
        'trading.live_results.win_rate',
        'trading.live_results.winning_days',
        'trading.live_results.losing_days',
        'trading.live_results.avg_win',
        'trading.live_results.avg_loss',
        'trading.live_results.best_day',
        'trading.live_results.worst_day',
        'trading.live_results.gross_profit',
        'trading.live_results.gross_loss',
        'trading.live_results.profit_factor',
        'trading.live_results.total_cumulative_return',
        'trading.live_results.current_portfolio_value',
        'trading.live_results.total_pnl',
        'trading.live_results.daily_pnl',
        'trading.live_results.total_realized_pnl',
        'trading.live_results.daily_realized_pnl',
        'trading.live_results.total_unrealized_pnl',
        'trading.live_results.daily_unrealized_pnl',
        'trading.live_results.daily_transaction_costs',
        'trading.live_results.total_transaction_costs',
        'trading.live_results.active_positions',
        'trading.live_results.margin_posted',
        'trading.live_results.cash_available',
        'trading.live_results.equity_to_margin_ratio',
        'trading.live_results.margin_cushion',
        'trading.live_results.portfolio_leverage',
        'trading.live_results.net_leverage',
        'trading.live_results.gross_leverage',
        'trading.live_results.total_dividend_income',
        'trading.live_results.portfolio_var',
        'trading.live_results.max_correlation',
        'trading.live_results.jump_risk',
        'trading.live_results.risk_scale',
        'trading.positions.average_price',
        'trading.positions.daily_unrealized_pnl',
        'trading.positions.last_update',
        'trading.positions.updated_at',
        'trading.executions.execution_time',
        'trading.executions.implicit_price_impact',
        'trading.executions.total_transaction_costs',
        'trading.signals.signal_value',
        'trading.signals.timestamp',
        'trading.signals.created_at',
        'trading.equity_curve',
        'trading.equity_curve.timestamp',
        'trading.live_run_metadata.created_at',
        'trading.live_run_metadata.strategy_configs',
        'trading.corp_action_applied.qty_held',
        'backtest.results',
        'backtest.results.start_date',
        'backtest.results.end_date',
        'backtest.results.total_return',
        'backtest.results.volatility',
        'backtest.results.sharpe_ratio',
        'backtest.results.downside_volatility',
        'backtest.results.sortino_ratio',
        'backtest.results.max_drawdown',
        'backtest.results.calmar_ratio',
        'backtest.results.var_95',
        'backtest.results.cvar_95',
        'backtest.results.beta',
        'backtest.results.correlation',
        'backtest.results.total_trades',
        'backtest.results.win_rate',
        'backtest.results.profit_factor',
        'backtest.results.avg_win',
        'backtest.results.avg_loss',
        'backtest.results.max_win',
        'backtest.results.max_loss',
        'backtest.results.avg_holding_period',
        'backtest.run_metadata.start_date',
        'backtest.run_metadata.end_date',
        'backtest.final_positions',
        'backtest.final_positions.average_price',
        'backtest.final_positions.last_update',
        'backtest.final_positions.updated_at',
        'backtest.executions.implicit_price_impact'
    ] LOOP
        p := string_to_array(o, '.');
        IF to_regclass(p[1] || '.' || p[2]) IS NULL THEN RAISE EXCEPTION '028: % does not exist', p[1] || '.' || p[2]; END IF;
        IF array_length(p, 1) = 3 AND NOT EXISTS (SELECT 1 FROM information_schema.columns
                WHERE table_schema = p[1] AND table_name = p[2] AND column_name = p[3]) THEN
            RAISE EXCEPTION '028: column % does not exist', o;
        END IF;
    END LOOP;
END $$;
COMMENT ON TABLE trading.live_results IS NULL;
COMMENT ON COLUMN trading.live_results."date" IS NULL;
COMMENT ON COLUMN trading.live_results."created_at" IS NULL;
COMMENT ON COLUMN trading.live_results."total_days" IS NULL;
COMMENT ON COLUMN trading.live_results."total_annualized_return" IS $c028$Annualized return using formula: ((current_value/initial_value)^(252/days) - 1) * 100$c028$;
COMMENT ON COLUMN trading.live_results."volatility" IS NULL;
COMMENT ON COLUMN trading.live_results."sharpe_ratio" IS NULL;
COMMENT ON COLUMN trading.live_results."downside_deviation" IS NULL;
COMMENT ON COLUMN trading.live_results."sortino_ratio" IS NULL;
COMMENT ON COLUMN trading.live_results."max_drawdown" IS NULL;
COMMENT ON COLUMN trading.live_results."win_rate" IS NULL;
COMMENT ON COLUMN trading.live_results."winning_days" IS NULL;
COMMENT ON COLUMN trading.live_results."losing_days" IS NULL;
COMMENT ON COLUMN trading.live_results."avg_win" IS NULL;
COMMENT ON COLUMN trading.live_results."avg_loss" IS NULL;
COMMENT ON COLUMN trading.live_results."best_day" IS NULL;
COMMENT ON COLUMN trading.live_results."worst_day" IS NULL;
COMMENT ON COLUMN trading.live_results."gross_profit" IS NULL;
COMMENT ON COLUMN trading.live_results."gross_loss" IS NULL;
COMMENT ON COLUMN trading.live_results."profit_factor" IS NULL;
COMMENT ON COLUMN trading.live_results."total_cumulative_return" IS $c028$Total cumulative return since inception: ((current_value/initial_value) - 1) * 100$c028$;
COMMENT ON COLUMN trading.live_results."current_portfolio_value" IS NULL;
COMMENT ON COLUMN trading.live_results."total_pnl" IS NULL;
COMMENT ON COLUMN trading.live_results."daily_pnl" IS NULL;
COMMENT ON COLUMN trading.live_results."total_realized_pnl" IS NULL;
COMMENT ON COLUMN trading.live_results."daily_realized_pnl" IS NULL;
COMMENT ON COLUMN trading.live_results."total_unrealized_pnl" IS NULL;
COMMENT ON COLUMN trading.live_results."daily_unrealized_pnl" IS NULL;
COMMENT ON COLUMN trading.live_results."daily_transaction_costs" IS NULL;
COMMENT ON COLUMN trading.live_results."total_transaction_costs" IS NULL;
COMMENT ON COLUMN trading.live_results."active_positions" IS NULL;
COMMENT ON COLUMN trading.live_results."margin_posted" IS $c028$Sum of initial margin per contract × contracts across all open positions (daily posted margin).$c028$;
COMMENT ON COLUMN trading.live_results."cash_available" IS $c028$Current portfolio value minus margin_posted (cash on hand).$c028$;
COMMENT ON COLUMN trading.live_results."equity_to_margin_ratio" IS $c028$Current portfolio value divided by total posted margin, computed at the book level: one portfolio value over the sum of every position's posted margin, so a mixed futures-plus-equity book sums both margin kinds under one portfolio value (HD 2026-09-10). The futures runners compute this; the equity runner writes gross notional over posted margin until T-8 moves it to this definition (migration 013).$c028$;
COMMENT ON COLUMN trading.live_results."margin_cushion" IS $c028$(Current portfolio value – today's maintenance margin requirement) ÷ current portfolio value.$c028$;
COMMENT ON COLUMN trading.live_results."portfolio_leverage" IS $c028$Gross notional divided by current portfolio value (live-time portfolio leverage).$c028$;
COMMENT ON COLUMN trading.live_results."net_leverage" IS $c028$Absolute net notional divided by configured capital (risk manager).$c028$;
COMMENT ON COLUMN trading.live_results."gross_leverage" IS $c028$Sum of absolute notionals divided by configured capital (risk manager).$c028$;
COMMENT ON COLUMN trading.live_results."total_dividend_income" IS $c028$Cumulative dividend cash income for (strategy, portfolio) as of this date. Informational ONLY -- NOT added to total_pnl. closeadj captures dividend total-return via price continuity (Phase 4 avg_price frame-alignment fix); adding dividend cash on top would double-count. Source: in-process sum of CorporateActionsAuditLog dividend_events at daily finalization. Decomposition view: total_return = capital_appreciation + total_dividend_income.$c028$;
COMMENT ON COLUMN trading.live_results."portfolio_var" IS NULL;
COMMENT ON COLUMN trading.live_results."max_correlation" IS NULL;
COMMENT ON COLUMN trading.live_results."jump_risk" IS NULL;
COMMENT ON COLUMN trading.live_results."risk_scale" IS $c028$The DELIVERED scale of the day's rebalance: the stored book's gross notional over the capped target's gross notional, both at the raw signal closes (held rows counted in both); it can exceed 1. The requested multiplier m_t is never stored here: it is risk_detail.risk_requested. Rows written before migration 020's binary hold the reporter's recommended scale. LOOP_SPEC v6.2 sections 7.2 and 10; migration 020.$c028$;
COMMENT ON COLUMN trading.positions."average_price" IS NULL;
COMMENT ON COLUMN trading.positions."daily_unrealized_pnl" IS NULL;
COMMENT ON COLUMN trading.positions."last_update" IS NULL;
COMMENT ON COLUMN trading.positions."updated_at" IS NULL;
COMMENT ON COLUMN trading.executions."execution_time" IS NULL;
COMMENT ON COLUMN trading.executions."implicit_price_impact" IS NULL;
COMMENT ON COLUMN trading.executions."total_transaction_costs" IS NULL;
COMMENT ON COLUMN trading.signals."signal_value" IS NULL;
COMMENT ON COLUMN trading.signals."timestamp" IS NULL;
COMMENT ON COLUMN trading.signals."created_at" IS NULL;
COMMENT ON TABLE trading.equity_curve IS $c028$Daily equity curve tracking for live strategies$c028$;
COMMENT ON COLUMN trading.equity_curve."timestamp" IS NULL;
COMMENT ON COLUMN trading.live_run_metadata."created_at" IS NULL;
COMMENT ON COLUMN trading.live_run_metadata."strategy_configs" IS NULL;
COMMENT ON COLUMN trading.corp_action_applied."qty_held" IS NULL;
COMMENT ON TABLE backtest.results IS $c028$Main backtest performance metrics and results$c028$;
COMMENT ON COLUMN backtest.results."start_date" IS NULL;
COMMENT ON COLUMN backtest.results."end_date" IS NULL;
COMMENT ON COLUMN backtest.results."total_return" IS NULL;
COMMENT ON COLUMN backtest.results."volatility" IS NULL;
COMMENT ON COLUMN backtest.results."sharpe_ratio" IS NULL;
COMMENT ON COLUMN backtest.results."downside_volatility" IS NULL;
COMMENT ON COLUMN backtest.results."sortino_ratio" IS NULL;
COMMENT ON COLUMN backtest.results."max_drawdown" IS NULL;
COMMENT ON COLUMN backtest.results."calmar_ratio" IS NULL;
COMMENT ON COLUMN backtest.results."var_95" IS NULL;
COMMENT ON COLUMN backtest.results."cvar_95" IS NULL;
COMMENT ON COLUMN backtest.results."beta" IS NULL;
COMMENT ON COLUMN backtest.results."correlation" IS NULL;
COMMENT ON COLUMN backtest.results."total_trades" IS NULL;
COMMENT ON COLUMN backtest.results."win_rate" IS NULL;
COMMENT ON COLUMN backtest.results."profit_factor" IS NULL;
COMMENT ON COLUMN backtest.results."avg_win" IS NULL;
COMMENT ON COLUMN backtest.results."avg_loss" IS NULL;
COMMENT ON COLUMN backtest.results."max_win" IS NULL;
COMMENT ON COLUMN backtest.results."max_loss" IS NULL;
COMMENT ON COLUMN backtest.results."avg_holding_period" IS NULL;
COMMENT ON COLUMN backtest.run_metadata."start_date" IS NULL;
COMMENT ON COLUMN backtest.run_metadata."end_date" IS NULL;
COMMENT ON TABLE backtest.final_positions IS $c028$Final positions at end of backtest$c028$;
COMMENT ON COLUMN backtest.final_positions."average_price" IS NULL;
COMMENT ON COLUMN backtest.final_positions."last_update" IS NULL;
COMMENT ON COLUMN backtest.final_positions."updated_at" IS NULL;
COMMENT ON COLUMN backtest.executions."implicit_price_impact" IS NULL;
COMMIT;
