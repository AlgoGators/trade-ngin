-- Explicit public synthetic substrate. Empty tables; no authentic market/action claim.
-- This is a fixture prerequisite for new complete raw inventories, never a production migration.
BEGIN;
CREATE SCHEMA equities_data;
CREATE TABLE equities_data.ohlcv_1d (
    symbol TEXT NOT NULL,
    time TIMESTAMPTZ NOT NULL,
    open DOUBLE PRECISION NOT NULL,
    high DOUBLE PRECISION NOT NULL,
    low DOUBLE PRECISION NOT NULL,
    close DOUBLE PRECISION NOT NULL,
    volume DOUBLE PRECISION NOT NULL,
    div_cash DOUBLE PRECISION,
    split_factor DOUBLE PRECISION,
    delisting_date TEXT,
    PRIMARY KEY (symbol,time)
);
CREATE TABLE equities_data.corporate_action (
    date TEXT NOT NULL,
    action TEXT NOT NULL,
    ticker TEXT NOT NULL,
    value TEXT,
    contraticker TEXT,
    contraname TEXT,
    name TEXT
);
CREATE TABLE equities_data.ticker_aliases (
    historical_ticker TEXT PRIMARY KEY,
    current_symbol TEXT NOT NULL,
    effective_until TEXT,
    note TEXT
);
COMMIT;
