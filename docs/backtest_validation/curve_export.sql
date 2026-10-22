-- Issue #113: SELECT-only export against HD's scratch copy.
-- Run with psql --csv -X -v run_id=... -v portfolio_id=... -f curve_export.sql
-- and redirect stdout to a run-specific CSV. The :'name' form quotes psql
-- variables as SQL literals. Never run this against production. The role
-- should have SELECT and no write grants.
SELECT run_id, portfolio_id,
       to_char("timestamp" AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"') AS timestamp,
       equity
FROM backtest.equity_curve
WHERE run_id = :'run_id' AND portfolio_id = :'portfolio_id'
ORDER BY "timestamp", id;
