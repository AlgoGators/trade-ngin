-- EXPLICITLY SYNTHETIC public baseline substrate. Owned empty scratch DB only.
-- This is not a production export or an authentic market fixture.
CREATE SCHEMA trading;
CREATE SCHEMA metadata;
CREATE SCHEMA futures_data;
CREATE SCHEMA auth;
-- Empty public auth substrate for the exact governance migration. No users/grants.
CREATE TABLE auth.users (id bigint PRIMARY KEY,role text NOT NULL);
CREATE TABLE trading.strategy_registry (
 id text PRIMARY KEY,strategy_type text NOT NULL,portfolio_id text NOT NULL,
 lifecycle text NOT NULL DEFAULT 'live',is_active boolean NOT NULL DEFAULT true);
CREATE TABLE trading.strategy_book_memberships (
 strategy_id text REFERENCES trading.strategy_registry(id),portfolio_id text,
 PRIMARY KEY(strategy_id,portfolio_id));
INSERT INTO trading.strategy_registry VALUES
 ('synthetic-model-base','LIVE_TREND_FOLLOWING','BASE_PORTFOLIO','live',true),
 ('synthetic-model-conservative','LIVE_TREND_FOLLOWING','CONSERVATIVE_PORTFOLIO','live',true);
CREATE TABLE trading.positions (
 symbol varchar(20) NOT NULL,quantity numeric(20,6) NOT NULL,average_price numeric(20,6) NOT NULL,
 daily_unrealized_pnl numeric(20,6) NOT NULL,daily_realized_pnl numeric(20,6) NOT NULL,
 last_update timestamptz NOT NULL,updated_at timestamptz DEFAULT CURRENT_TIMESTAMP,
 strategy_id varchar(50) NOT NULL,strategy_name varchar(100) NOT NULL,date date NOT NULL,
 portfolio_id varchar(100) NOT NULL,portfolio_type text NOT NULL DEFAULT 'system',
 CONSTRAINT positions_pkey PRIMARY KEY(portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type),
 CONSTRAINT positions_portfolio_type_check CHECK(portfolio_type IN
 ('system','qt','benchmark','benchmark_rebench','benchmark_frozen_shadow')));
CREATE TABLE trading.executions (
 exec_id text,order_id text,symbol text,side text,quantity numeric,price numeric,
 execution_time timestamptz,commissions_fees numeric,implicit_price_impact numeric,
 slippage_market_impact numeric,total_transaction_costs numeric,is_partial boolean,
 strategy_id text, strategy_name text,date date,portfolio_id text,portfolio_type text NOT NULL DEFAULT 'system',
 PRIMARY KEY(exec_id,portfolio_id,strategy_id,strategy_name,portfolio_type));
CREATE TABLE trading.signals (
 strategy_id text,symbol text,signal_value numeric,timestamp timestamptz,portfolio_id text,strategy_name text,
 PRIMARY KEY(portfolio_id,strategy_id,strategy_name,symbol,timestamp));
CREATE TABLE trading.live_results (
 strategy_id text,portfolio_id text,date timestamptz,portfolio_type text NOT NULL DEFAULT 'system',
 total_cumulative_return numeric,total_annualized_return numeric,volatility numeric,downside_deviation numeric,
 max_drawdown numeric,win_rate numeric,avg_win numeric,avg_loss numeric,best_day numeric,worst_day numeric,
 gross_profit numeric,gross_loss numeric,total_pnl numeric,total_unrealized_pnl numeric,total_realized_pnl numeric,
 current_portfolio_value numeric,portfolio_var numeric,net_leverage numeric,portfolio_leverage numeric,
 equity_to_margin_ratio numeric,margin_cushion numeric,max_correlation numeric,jump_risk numeric,risk_scale numeric,
 gross_notional numeric,net_notional numeric,daily_return numeric,daily_pnl numeric,total_transaction_costs numeric,
 daily_realized_pnl numeric,daily_unrealized_pnl numeric,daily_transaction_costs numeric,margin_posted numeric,
 cash_available numeric,sharpe_ratio numeric,sortino_ratio numeric,profit_factor numeric,
 active_positions integer,winning_days integer,losing_days integer,total_days integer,config jsonb,
 PRIMARY KEY(portfolio_id,strategy_id,date,portfolio_type));
CREATE TABLE trading.equity_curve (
 strategy_id text,portfolio_id text,timestamp timestamptz,equity numeric,portfolio_type text NOT NULL DEFAULT 'system',
 PRIMARY KEY(portfolio_id,strategy_id,timestamp,portfolio_type));
CREATE TABLE trading.live_run_metadata (
 date date,strategy_id text,portfolio_id text,strategy_allocations jsonb,portfolio_config jsonb,strategy_configs jsonb,
 created_at timestamptz DEFAULT now(),PRIMARY KEY(date,strategy_id,portfolio_id));
CREATE TABLE trading.run_inputs (
 portfolio_id text,strategy_id text,date date,trade_ngin_sha text,config_snapshot jsonb,universe jsonb,
 data_window jsonb,risk_limits_id bigint,engine_flags jsonb,recorded_at timestamptz DEFAULT now(),
 PRIMARY KEY(portfolio_id,strategy_id,date));
CREATE TABLE trading.strategy_trading_days_metadata (
 strategy_id text PRIMARY KEY,live_start_date date NOT NULL);
INSERT INTO trading.strategy_trading_days_metadata VALUES ('LIVE_TREND_FOLLOWING','2026-09-25');
-- Explicit synthetic weekday-count policy fulfills this fixture's DB contract.
-- It is not asserted equivalent to an unavailable production holiday function.
CREATE FUNCTION trading.get_trading_days(identity text,until_day date) RETURNS integer LANGUAGE SQL STABLE AS $$
 SELECT count(*)::integer FROM trading.strategy_trading_days_metadata m,
 generate_series(m.live_start_date,until_day,'1 day') d WHERE m.strategy_id=identity AND extract(isodow FROM d)<6
$$;
CREATE TABLE metadata.contract_metadata (
 "Databento Symbol" text,"IB Symbol" text,"Name" text,"Exchange" text,
 "Intraday Initial Margin" double precision,"Intraday Maintenance Margin" double precision,
 "Overnight Initial Margin" double precision,"Overnight Maintenance Margin" double precision,
 "Asset Type" text,"Sector" text,"Contract Size" double precision,"Units" text,
 "Minimum Price Fluctuation" double precision,"Tick Size" text,"Settlement Type" text,
 "Trading Hours (EST)" text,"Data Provider" text,"Dataset" text,
 "Newest Month Additions" text,"Contract Months" text,"Time of Expiry" text);
-- Exact 21-column PostgreSQL order consumed by convert_metadata_to_arrow.
-- The loader projects these into 18 Arrow fields; no fallback sizes are needed.
INSERT INTO metadata.contract_metadata VALUES
 ('ES','ES','Synthetic ES','CME',15000,13000,15000,13000,'FUTURE','Equity Index',50,'USD',
  0.25,'0.25','cash','synthetic','synthetic','synthetic','none','HMUZ','synthetic');
CREATE TABLE futures_data.ohlcv_1d (
 time timestamptz NOT NULL,symbol text NOT NULL,open double precision,high double precision,
 low double precision,close double precision,volume double precision,PRIMARY KEY(time,symbol));
-- Nondegenerate prices with a real warm-up history, explicitly artificial.
INSERT INTO futures_data.ohlcv_1d
 SELECT d,'ES.v.0',4000+i*0.8+(i%7)*0.13,4002+i*0.8+(i%7)*0.13,
 3998+i*0.8+(i%7)*0.13,4001+i*0.8+(i%7)*0.13,100000+i*10
 FROM generate_series('2024-09-25'::timestamptz,'2026-09-24'::timestamptz,'1 day') WITH ORDINALITY AS rows(d,i)
 WHERE extract(isodow FROM d)<6;
