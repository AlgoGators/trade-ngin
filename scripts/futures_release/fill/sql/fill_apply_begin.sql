-- fill_apply_begin.sql: opens the one transaction; runs BEFORE fill_core.sql so the holes are computed under the lock.
\set ON_ERROR_STOP on
begin;
-- SHARE ROW EXCLUSIVE on the two continuous tables: readers continue; no other session can write them until commit.
lock table futures_data.ohlcv_1d, futures_data.ohlcv_1d_raw in share row exclusive mode;
-- SHARE on the contract table: readers continue; the daily pull cannot add a contract bar while the fill runs.
lock table futures_data.contract_ohlcv_1d in share mode;
