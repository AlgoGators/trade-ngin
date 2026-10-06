-- Rollback for 003_equity_query_indexes.sql. Dropping these restores the
-- pre-index query plans (seq scans); it loses no data.

BEGIN;

DROP INDEX IF EXISTS equities_data.idx_ohlcv_1d_delisting;
DROP INDEX IF EXISTS equities_data.idx_ohlcv_1d_corp_events;
DROP INDEX IF EXISTS equities_data.idx_corporate_action_ticker_date;

COMMIT;
