-- data_fingerprint.sql: read only. Per table: rows up to the cut
-- (UTC), last date, md5 of every row in order, for the four tables the engine reads its data from; then the same for the
-- .v.0 rows of MGC and SIL alone, and the count of .v.0 symbols. Run: psql -At -v cut=2026-10-23 -f this file.
select 'futures_data.ohlcv_1d ' || count(*) || ' ' || max(time)::date || ' ' || md5(string_agg(t::text, E'\n' order by time, symbol))
from futures_data.ohlcv_1d t where time < (:'cut'::date + 1);
select 'futures_data.ohlcv_1d_raw ' || count(*) || ' ' || max(ts_event)::date || ' ' || md5(string_agg(t::text, E'\n' order by ts_event, symbol))
from futures_data.ohlcv_1d_raw t where ts_event < (:'cut'::date + 1);
select 'futures_data.contract_ohlcv_1d ' || count(*) || ' ' || max(date) || ' ' || md5(string_agg(t::text, E'\n' order by instrument_id, date))
from futures_data.contract_ohlcv_1d t where date <= :'cut'::date;
select 'metadata.contract_metadata ' || count(*) || ' ' || md5(string_agg(t::text, E'\n' order by "Databento Symbol"))
from metadata.contract_metadata t;
select 'micros ohlcv_1d ' || count(*) || ' ' || coalesce(md5(string_agg(t::text, E'\n' order by time, symbol)), '-')
from futures_data.ohlcv_1d t where symbol in ('MGC.v.0', 'SIL.v.0') and time < (:'cut'::date + 1);
select 'micros ohlcv_1d_raw ' || count(*) || ' ' || coalesce(md5(string_agg(t::text, E'\n' order by ts_event, symbol)), '-')
from futures_data.ohlcv_1d_raw t where symbol in ('MGC.v.0', 'SIL.v.0') and ts_event < (:'cut'::date + 1);
select 'micros contract_ohlcv_1d ' || count(*) || ' ' || coalesce(md5(string_agg(t::text, E'\n' order by instrument_id, date)), '-')
from futures_data.contract_ohlcv_1d t where root in ('MGC', 'SIL') and date <= :'cut'::date;
select 'v0 symbols ' || count(distinct symbol) || ' ' || string_agg(distinct symbol, ' ' order by symbol)
from futures_data.ohlcv_1d where symbol like '%.v.0';
-- the contract table without loaded_at (the insert clock: a reload of the same bars differs only there), every row in
-- the order of all its other columns; this is the line compared after the data steps
select 'contract_ohlcv_1d_less_loaded_at ' || count(*) || ' ' || md5(string_agg(concat_ws(',', instrument_id, date, root, raw_symbol, expiration, open, high, low, close, volume), E'\n' order by instrument_id, date, root, raw_symbol, expiration, open, high, low, close, volume))
from futures_data.contract_ohlcv_1d t where date <= :'cut'::date;
