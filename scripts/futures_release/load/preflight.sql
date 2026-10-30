-- preflight.sql: read-only, before any part of load_micro_metals.sql. What
-- the four tables are on this server (plain table or TimescaleDB hypertable, compression), their unique keys, the
-- session's TimeZone, and what of MGC and SIL is stored already (which part comes next). Refuses a session that is
-- not UTC. the fill's preflight (the fill/sql/fill_preflight.sql) with the micro metals' tables and state.
\set ON_ERROR_STOP on
\echo == preflight: server, TimeZone, TimescaleDB
select current_database() as db, inet_server_addr() as host, inet_server_port() as port, current_setting('TimeZone') as tz,
       coalesce((select extversion from pg_extension where extname = 'timescaledb'), 'not installed') as timescaledb;
\echo == preflight: the tables (kind r = plain table, p = partitioned; hypertables listed below when installed)
select n.nspname || '.' || c.relname as table_name, c.relkind as kind, c.reltuples::bigint as est_rows,
       (select string_agg(pg_get_indexdef(i.indexrelid), '; ') from pg_index i where i.indrelid = c.oid and (i.indisunique or i.indisprimary)) as unique_keys
from pg_class c join pg_namespace n on n.oid = c.relnamespace
where (n.nspname = 'futures_data' and c.relname in ('ohlcv_1d', 'ohlcv_1d_raw', 'contract_ohlcv_1d'))
   or (n.nspname = 'metadata' and c.relname = 'contract_metadata') order by 1;
select exists (select 1 from pg_extension where extname = 'timescaledb') as has_tsdb \gset
\if :has_tsdb
select hypertable_schema, hypertable_name, num_chunks, compression_enabled from timescaledb_information.hypertables
where hypertable_schema in ('futures_data', 'metadata') order by 1, 2;
\echo 'TimescaleDB: a refused insert rolls the part back'
\endif
\echo == preflight: MGC and SIL already stored (part A: contract and metadata rows; part B: the continuous rows)
select 'contract_ohlcv_1d' as table_name, root as key, count(*) as rows, min(date)::text as first, max(date)::text as last
from futures_data.contract_ohlcv_1d where root in ('MGC', 'SIL') group by 2
union all
select 'ohlcv_1d', symbol, count(*), min((time at time zone 'UTC')::date)::text, max((time at time zone 'UTC')::date)::text
from futures_data.ohlcv_1d where symbol in ('MGC.v.0', 'SIL.v.0') group by 2
union all
select 'contract_metadata', "Databento Symbol", count(*), null, null
from metadata.contract_metadata where "Databento Symbol" in ('MGC', 'SIL') group by 2
order by 1, 2;
do $$ begin
  if current_setting('TimeZone') not in ('UTC', 'Etc/UTC') then
    raise exception 'micro metals preflight: session TimeZone is %, not UTC', current_setting('TimeZone');
  end if;
end $$;
\echo == preflight passed
