-- fill_counts.sql: table counts (pre or post), read-only.
\set ON_ERROR_STOP on
select 'ohlcv_1d .v.0 rows' as what, count(*) as n from futures_data.ohlcv_1d where symbol like '%.v.0'
union all select 'ohlcv_1d_raw .v.0 rows', count(*) from futures_data.ohlcv_1d_raw where symbol like '%.v.0'
union all select 'ohlcv_1d (symbol, time) pairs held more than once', count(*) from (select 1 from futures_data.ohlcv_1d group by symbol, time having count(*) > 1) x
union all select '.v.0 rows without a raw row', count(*) from futures_data.ohlcv_1d o where o.symbol like '%.v.0' and not exists (select 1 from futures_data.ohlcv_1d_raw r where r.symbol = o.symbol and r.ts_event = o.time);
