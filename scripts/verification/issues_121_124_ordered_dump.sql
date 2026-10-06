\set ON_ERROR_STOP on
\pset tuples_only on
\pset format unaligned

-- Required variables:
--   portfolio_id, strategy_id, source_day
-- Values are SQL literals, e.g. -v portfolio_id="'EQUITY_MR_PORTFOLIO'".
SELECT 'positions|' || coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), '[]')::text
FROM trading.positions t
WHERE portfolio_id = :portfolio_id AND strategy_id = :strategy_id;

SELECT 'executions|' || coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), '[]')::text
FROM trading.executions t
WHERE portfolio_id = :portfolio_id AND strategy_id = :strategy_id;

SELECT 'signals|' || coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), '[]')::text
FROM trading.signals t
WHERE portfolio_id = :portfolio_id AND strategy_id = :strategy_id;

SELECT 'live_results|' || coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), '[]')::text
FROM trading.live_results t
WHERE portfolio_id = :portfolio_id AND strategy_id = :strategy_id;

SELECT 'equity_curve|' || coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), '[]')::text
FROM trading.equity_curve t
WHERE portfolio_id = :portfolio_id AND strategy_id = :strategy_id;

SELECT 'live_run_metadata|' || coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), '[]')::text
FROM trading.live_run_metadata t
WHERE portfolio_id = :portfolio_id AND strategy_id = :strategy_id;

SELECT 'run_inputs|' || coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), '[]')::text
FROM trading.run_inputs t
WHERE portfolio_id = :portfolio_id AND strategy_id = :strategy_id;

SELECT 'risk_limits|' || coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), '[]')::text
FROM trading.risk_limits t
WHERE portfolio_id = :portfolio_id AND strategy_id = :strategy_id;

SELECT 'corp_action_applied|' || coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), '[]')::text
FROM trading.corp_action_applied t
WHERE portfolio_id = :portfolio_id AND strategy_id = :strategy_id;

-- A genuine P0 house run must not be admitted through the system-investor
-- bypass. Keep the absence itself in the byte-for-byte parity artifact.
SELECT 'investor_books|' || coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), '[]')::text
FROM trading.investor_books t
WHERE portfolio_id = :portfolio_id;

SELECT 'investor_book_strategies|' || coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), '[]')::text
FROM trading.investor_book_strategies t
WHERE portfolio_id = :portfolio_id AND strategy_id = :strategy_id;

SELECT 'investor_book_publications|' || coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), '[]')::text
FROM trading.investor_book_publications t
WHERE portfolio_id = :portfolio_id AND strategy_id = :strategy_id
  AND source_day = :source_day;

SELECT 'qt_model_seed_publications|' ||
       coalesce(jsonb_agg(to_jsonb(t) - 'created_at'
                          ORDER BY (to_jsonb(t) - 'created_at')::text), '[]')::text
FROM trading.qt_model_seed_publications t
WHERE portfolio_id = :portfolio_id AND strategy_id = :strategy_id
  AND source_day <= :source_day;

SELECT 'qt_empty_model_owner_publications|' ||
       coalesce(jsonb_agg(to_jsonb(t) - 'created_at'
                          ORDER BY (to_jsonb(t) - 'created_at')::text), '[]')::text
FROM trading.qt_empty_model_owner_publications t
WHERE portfolio_id = :portfolio_id AND strategy_id = :strategy_id
  AND source_day <= :source_day;

SELECT 'strategy_trading_days_metadata|' ||
       coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), '[]')::text
FROM trading.strategy_trading_days_metadata t
WHERE portfolio_id = :portfolio_id AND strategy_id = :strategy_id;
