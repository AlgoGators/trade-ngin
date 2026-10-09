-- tests/data/fixtures/qt_books_schema.sql
--
-- The four live trading tables in their stage-3 shape BEFORE migration 021 (001, 012, 013, 015,
-- 016, 017 and 020 applied), for the database-backed tests of the book-aware storage
-- (tests/data/test_qt_books_db.cpp) and the other *_db tests of these tables. Only the columns
-- the engine's storage code names are here. For a THROWAWAY database only:
--
--   createdb qtbooks
--   psql -d qtbooks -f tests/data/fixtures/qt_books_schema.sql
--   psql -d qtbooks -f migrations/021_qt_books.sql
--   TRADE_NGIN_TEST_DSN=postgresql://.../qtbooks TRADE_NGIN_REQUIRE_DB=1 \
--       ./trade_ngin_tests --gtest_filter='QtBooksDb.*'

DO $$ BEGIN
    IF to_regnamespace('trading') IS NOT NULL THEN
        RAISE EXCEPTION 'the target already has a trading schema: this fixture is for a throwaway database';
    END IF;
END $$;

CREATE SCHEMA trading;

CREATE TABLE trading.positions (
    symbol               VARCHAR(20)  NOT NULL,
    quantity             NUMERIC      NOT NULL,
    average_price        NUMERIC      NOT NULL,
    daily_unrealized_pnl NUMERIC      DEFAULT 0,
    daily_realized_pnl   NUMERIC      DEFAULT 0,
    last_update          TIMESTAMPTZ  NOT NULL,
    updated_at           TIMESTAMPTZ  DEFAULT CURRENT_TIMESTAMP,
    strategy_id          VARCHAR(100) NOT NULL,
    strategy_name        VARCHAR(100) NOT NULL,
    date                 DATE         NOT NULL,
    portfolio_id         VARCHAR(100) NOT NULL,
    portfolio_type       TEXT         NOT NULL DEFAULT 'system',
    instrument_id        TEXT,
    CONSTRAINT positions_portfolio_type_check CHECK (portfolio_type IN ('system', 'qt')),
    CONSTRAINT positions_pkey PRIMARY KEY (portfolio_id, strategy_id, strategy_name, date, symbol, portfolio_type));

CREATE TABLE trading.equity_curve (
    id             SERIAL PRIMARY KEY,
    strategy_id    VARCHAR(100)     NOT NULL,
    "timestamp"    TIMESTAMPTZ      NOT NULL,
    equity         DOUBLE PRECISION NOT NULL,
    portfolio_id   VARCHAR(100),
    portfolio_type TEXT             NOT NULL DEFAULT 'system',
    CONSTRAINT equity_curve_portfolio_type_check CHECK (portfolio_type IN ('system', 'qt')),
    CONSTRAINT trading_equity_curve_unique UNIQUE (portfolio_id, strategy_id, "timestamp", portfolio_type));

CREATE TABLE trading.executions (
    exec_id                 VARCHAR(50)  NOT NULL,
    order_id                VARCHAR(50)  NOT NULL,
    symbol                  VARCHAR(20)  NOT NULL,
    side                    VARCHAR(4)   NOT NULL,
    quantity                NUMERIC      NOT NULL,
    price                   NUMERIC      NOT NULL,
    execution_time          TIMESTAMPTZ  NOT NULL,
    commissions_fees        NUMERIC      NOT NULL,
    is_partial              BOOLEAN      NOT NULL,
    created_at              TIMESTAMPTZ  DEFAULT CURRENT_TIMESTAMP,
    strategy_id             VARCHAR(100) NOT NULL,
    strategy_name           VARCHAR(100) NOT NULL,
    date                    DATE         NOT NULL,
    portfolio_id            VARCHAR(100) NOT NULL,
    implicit_price_impact   NUMERIC      DEFAULT 0.0,
    slippage_market_impact  NUMERIC      DEFAULT 0.0,
    total_transaction_costs NUMERIC      DEFAULT 0.0,
    netting_adjustment      NUMERIC      DEFAULT 0,
    execution_type          TEXT         NOT NULL DEFAULT 'STRATEGY',
    instrument_id           TEXT,
    CONSTRAINT executions_pkey PRIMARY KEY (portfolio_id, strategy_id, strategy_name, date, exec_id),
    CONSTRAINT chk_executions_quantity CHECK (quantity > 0::numeric),
    CONSTRAINT chk_executions_side CHECK (side IN ('BUY', 'SELL')),
    CONSTRAINT executions_execution_type_check CHECK (execution_type IN ('STRATEGY', 'ROLL', 'BORROW')));

CREATE TABLE trading.live_results (
    id                       SERIAL PRIMARY KEY,
    strategy_id              VARCHAR(100) NOT NULL,
    portfolio_id             VARCHAR(100) NOT NULL,
    date                     DATE         NOT NULL,
    current_portfolio_value  NUMERIC,
    total_pnl                NUMERIC,
    total_realized_pnl       NUMERIC,
    total_unrealized_pnl     NUMERIC,
    daily_pnl                NUMERIC,
    daily_realized_pnl       NUMERIC,
    daily_unrealized_pnl     NUMERIC,
    daily_return             NUMERIC,
    daily_transaction_costs  NUMERIC,
    total_transaction_costs  NUMERIC(20,8),
    daily_roll_costs         NUMERIC      NOT NULL DEFAULT 0,
    total_roll_costs         NUMERIC(20,8) NOT NULL DEFAULT 0,
    portfolio_leverage       NUMERIC,
    equity_to_margin_ratio   DOUBLE PRECISION,
    risk_scale               NUMERIC,
    config                   JSONB,
    risk_detail              JSONB,
    created_at               TIMESTAMPTZ  DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT live_results_portfolio_strategy_date_key UNIQUE (portfolio_id, strategy_id, date));
