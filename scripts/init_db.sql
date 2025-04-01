-- Init script for Trade-Ngin database

-- Create schemas
CREATE SCHEMA IF NOT EXISTS markets;
CREATE SCHEMA IF NOT EXISTS trading;
CREATE SCHEMA IF NOT EXISTS backtest;

-- Create market data tables
CREATE TABLE IF NOT EXISTS markets.ohlcv (
    symbol VARCHAR(20) NOT NULL,
    timestamp TIMESTAMP NOT NULL,
    open DOUBLE PRECISION NOT NULL,
    high DOUBLE PRECISION NOT NULL,
    low DOUBLE PRECISION NOT NULL,
    close DOUBLE PRECISION NOT NULL,
    volume DOUBLE PRECISION NOT NULL,
    asset_class VARCHAR(20) NOT NULL,
    frequency VARCHAR(20) NOT NULL,
    PRIMARY KEY (symbol, timestamp, frequency)
);

CREATE TABLE IF NOT EXISTS markets.instruments (
    symbol VARCHAR(20) PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    asset_class VARCHAR(20) NOT NULL,
    multiplier DOUBLE PRECISION NOT NULL DEFAULT 1.0,
    tick_size DOUBLE PRECISION NOT NULL DEFAULT 0.01,
    exchange VARCHAR(50) NOT NULL,
    currency VARCHAR(10) NOT NULL DEFAULT 'USD',
    active BOOLEAN NOT NULL DEFAULT TRUE
);

-- Create trading tables
CREATE TABLE IF NOT EXISTS trading.executions (
    id SERIAL PRIMARY KEY,
    strategy_id VARCHAR(50) NOT NULL,
    symbol VARCHAR(20) NOT NULL,
    timestamp TIMESTAMP NOT NULL,
    price DOUBLE PRECISION NOT NULL,
    quantity DOUBLE PRECISION NOT NULL,
    side VARCHAR(10) NOT NULL,
    order_id VARCHAR(50) NOT NULL,
    execution_id VARCHAR(50) NOT NULL,
    commission DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    slippage DOUBLE PRECISION NOT NULL DEFAULT 0.0
);

CREATE TABLE IF NOT EXISTS trading.positions (
    id SERIAL PRIMARY KEY,
    strategy_id VARCHAR(50) NOT NULL,
    symbol VARCHAR(20) NOT NULL,
    timestamp TIMESTAMP NOT NULL,
    quantity DOUBLE PRECISION NOT NULL,
    cost_basis DOUBLE PRECISION NOT NULL,
    market_value DOUBLE PRECISION NOT NULL,
    unrealized_pnl DOUBLE PRECISION NOT NULL,
    realized_pnl DOUBLE PRECISION NOT NULL
);

CREATE TABLE IF NOT EXISTS trading.signals (
    id SERIAL PRIMARY KEY,
    strategy_id VARCHAR(50) NOT NULL,
    symbol VARCHAR(20) NOT NULL,
    timestamp TIMESTAMP NOT NULL,
    signal_value DOUBLE PRECISION NOT NULL,
    processed BOOLEAN NOT NULL DEFAULT FALSE
);

-- Create backtest tables
CREATE TABLE IF NOT EXISTS backtest.runs (
    id VARCHAR(50) PRIMARY KEY,
    strategy_id VARCHAR(50) NOT NULL,
    start_date TIMESTAMP NOT NULL,
    end_date TIMESTAMP NOT NULL,
    initial_capital DOUBLE PRECISION NOT NULL,
    final_capital DOUBLE PRECISION NOT NULL,
    total_return DOUBLE PRECISION NOT NULL,
    sharpe_ratio DOUBLE PRECISION NOT NULL,
    max_drawdown DOUBLE PRECISION NOT NULL,
    config JSONB NOT NULL,
    timestamp TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    description TEXT
);

CREATE TABLE IF NOT EXISTS backtest.equity_curves (
    run_id VARCHAR(50) NOT NULL,
    timestamp TIMESTAMP NOT NULL,
    equity DOUBLE PRECISION NOT NULL,
    drawdown DOUBLE PRECISION NOT NULL,
    PRIMARY KEY (run_id, timestamp)
);

CREATE TABLE IF NOT EXISTS backtest.trade_results (
    id SERIAL PRIMARY KEY,
    run_id VARCHAR(50) NOT NULL,
    symbol VARCHAR(20) NOT NULL,
    entry_time TIMESTAMP NOT NULL,
    exit_time TIMESTAMP,
    entry_price DOUBLE PRECISION NOT NULL,
    exit_price DOUBLE PRECISION,
    quantity DOUBLE PRECISION NOT NULL,
    pnl DOUBLE PRECISION,
    return_pct DOUBLE PRECISION,
    duration_days INTEGER,
    status VARCHAR(20) NOT NULL DEFAULT 'OPEN'
);

-- Create indexes
CREATE INDEX IF NOT EXISTS idx_ohlcv_timestamp ON markets.ohlcv(timestamp);
CREATE INDEX IF NOT EXISTS idx_ohlcv_symbol ON markets.ohlcv(symbol);
CREATE INDEX IF NOT EXISTS idx_ohlcv_asset_class ON markets.ohlcv(asset_class);
CREATE INDEX IF NOT EXISTS idx_ohlcv_frequency ON markets.ohlcv(frequency);

CREATE INDEX IF NOT EXISTS idx_instruments_asset_class ON markets.instruments(asset_class);
CREATE INDEX IF NOT EXISTS idx_instruments_exchange ON markets.instruments(exchange);

CREATE INDEX IF NOT EXISTS idx_executions_strategy ON trading.executions(strategy_id);
CREATE INDEX IF NOT EXISTS idx_executions_symbol ON trading.executions(symbol);
CREATE INDEX IF NOT EXISTS idx_executions_timestamp ON trading.executions(timestamp);

CREATE INDEX IF NOT EXISTS idx_positions_strategy ON trading.positions(strategy_id);
CREATE INDEX IF NOT EXISTS idx_positions_symbol ON trading.positions(symbol);
CREATE INDEX IF NOT EXISTS idx_positions_timestamp ON trading.positions(timestamp);

CREATE INDEX IF NOT EXISTS idx_signals_strategy ON trading.signals(strategy_id);
CREATE INDEX IF NOT EXISTS idx_signals_symbol ON trading.signals(symbol);
CREATE INDEX IF NOT EXISTS idx_signals_timestamp ON trading.signals(timestamp);
CREATE INDEX IF NOT EXISTS idx_signals_processed ON trading.signals(processed);

CREATE INDEX IF NOT EXISTS idx_runs_strategy ON backtest.runs(strategy_id);
CREATE INDEX IF NOT EXISTS idx_runs_timestamp ON backtest.runs(timestamp);

CREATE INDEX IF NOT EXISTS idx_equity_curves_run_id ON backtest.equity_curves(run_id);
CREATE INDEX IF NOT EXISTS idx_equity_curves_timestamp ON backtest.equity_curves(timestamp);

CREATE INDEX IF NOT EXISTS idx_trade_results_run_id ON backtest.trade_results(run_id);
CREATE INDEX IF NOT EXISTS idx_trade_results_symbol ON backtest.trade_results(symbol);
CREATE INDEX IF NOT EXISTS idx_trade_results_entry_time ON backtest.trade_results(entry_time);
CREATE INDEX IF NOT EXISTS idx_trade_results_exit_time ON backtest.trade_results(exit_time);
CREATE INDEX IF NOT EXISTS idx_trade_results_status ON backtest.trade_results(status);

-- Create a user for the app
CREATE USER IF NOT EXISTS trade_ngin_app WITH PASSWORD 'password';
GRANT ALL PRIVILEGES ON SCHEMA markets TO trade_ngin_app;
GRANT ALL PRIVILEGES ON SCHEMA trading TO trade_ngin_app;
GRANT ALL PRIVILEGES ON SCHEMA backtest TO trade_ngin_app;
GRANT ALL PRIVILEGES ON ALL TABLES IN SCHEMA markets TO trade_ngin_app;
GRANT ALL PRIVILEGES ON ALL TABLES IN SCHEMA trading TO trade_ngin_app;
GRANT ALL PRIVILEGES ON ALL TABLES IN SCHEMA backtest TO trade_ngin_app;
GRANT ALL PRIVILEGES ON ALL SEQUENCES IN SCHEMA markets TO trade_ngin_app;
GRANT ALL PRIVILEGES ON ALL SEQUENCES IN SCHEMA trading TO trade_ngin_app;
GRANT ALL PRIVILEGES ON ALL SEQUENCES IN SCHEMA backtest TO trade_ngin_app; 