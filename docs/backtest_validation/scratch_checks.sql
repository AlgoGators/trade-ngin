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

-- The futures loader reads futures_data.ohlcv_1d, deduplicating symbol/time.
-- Record coverage and duplicate counts for the proposed window. These queries
-- do not decide whether the .v.0 roll splice is economically continuous.
SELECT symbol, min(time) AS first_bar, max(time) AS last_bar,
       count(*) AS raw_rows, count(DISTINCT time) AS distinct_timestamps
FROM futures_data.ohlcv_1d
GROUP BY symbol ORDER BY symbol;

WITH copies AS (
    SELECT symbol, time, count(*) AS n
    FROM futures_data.ohlcv_1d
    GROUP BY symbol, time HAVING count(*) > 1
)
SELECT count(*) AS duplicated_symbol_times,
       coalesce(sum(n - 1), 0) AS extra_rows,
       min(time) AS first_duplicate,
       max(time) AS last_duplicate
FROM copies;

SELECT symbol, date_trunc('year', time AT TIME ZONE 'UTC') AS year_utc,
       count(DISTINCT time) AS distinct_bar_times
FROM futures_data.ohlcv_1d
WHERE symbol LIKE '%.v.0'
GROUP BY symbol, date_trunc('year', time AT TIME ZONE 'UTC')
ORDER BY symbol, year_utc;
