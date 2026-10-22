-- Run ONLY on HD's UTC scratch database in pgAdmin Query Tool.
-- Replace both example literals for each run, execute this SELECT, then use
-- pgAdmin's CSV export with column headers. Keep the filename/run ID in the
-- manifest. Do not edit the query into an INSERT/UPDATE/DELETE.
SELECT id, run_id, portfolio_id,
       to_char("timestamp" AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"') AS timestamp,
       equity
FROM backtest.equity_curve
WHERE run_id = 'REPLACE_WITH_RUN_ID'
  AND portfolio_id = 'CONSERVATIVE_PORTFOLIO'
ORDER BY "timestamp", id;
