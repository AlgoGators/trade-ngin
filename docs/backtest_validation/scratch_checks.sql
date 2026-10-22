-- Read-only facts to record before any runner uses the scratch database.
SELECT current_database() AS database_name,
       current_setting('TimeZone') AS session_timezone,
       now() AT TIME ZONE 'UTC' AS inspected_at_utc;

SELECT table_schema, table_name, column_name, data_type
FROM information_schema.columns
WHERE table_schema = 'backtest'
  AND table_name IN ('equity_curve', 'results', 'executions')
ORDER BY table_name, ordinal_position;

SELECT 'equity_curve' AS table_name, count(*) AS rows FROM backtest.equity_curve
UNION ALL SELECT 'results', count(*) FROM backtest.results
UNION ALL SELECT 'executions', count(*) FROM backtest.executions;
