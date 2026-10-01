\set ON_ERROR_STOP on

-- This intentionally resets the entire dedicated P2/P3 verification database.
-- It must never be pointed at a shared or production database.
DO $$
BEGIN
    IF current_database() <> 'trade_ngin_121124_p2_test'
       OR inet_server_addr() IS NOT NULL THEN
        RAISE EXCEPTION 'owned local trade_ngin_121124_p2_test database required';
    END IF;
END $$;

TRUNCATE TABLE
    trading.investor_book_publications,
    trading.investor_book_strategies,
    trading.investor_books,
    trading.positions,
    trading.executions,
    trading.signals,
    trading.live_results,
    trading.equity_curve,
    trading.live_run_metadata,
    trading.run_inputs,
    trading.risk_limits,
    trading.corp_action_applied,
    trading.strategy_trading_days_metadata
RESTART IDENTITY CASCADE;

DELETE FROM equities_data.ohlcv_1d WHERE symbol = 'SYN';
DELETE FROM equities_data.corporate_action WHERE ticker = 'SYN';
DELETE FROM equities_data.ticker_aliases
 WHERE historical_ticker = 'SYN' OR current_symbol = 'SYN';
