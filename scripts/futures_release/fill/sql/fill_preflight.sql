-- fill_preflight.sql: read-only. What the three tables are on this server (plain table or TimescaleDB hypertable),
-- their keys, and the session's TimeZone; refuses when a permanent relation named fill_* exists.
\set ON_ERROR_STOP on
\echo == preflight: server, TimeZone, TimescaleDB
select current_database() as db, inet_server_addr() as host, inet_server_port() as port, current_setting('TimeZone') as tz,
       coalesce((select extversion from pg_extension where extname = 'timescaledb'), 'not installed') as timescaledb;
\echo == preflight: the three tables (kind r = plain table; hypertable per timescaledb_information when installed)
select c.relname as table_name, c.relkind as kind, c.reltuples::bigint as est_rows,
       (select string_agg(pg_get_indexdef(i.indexrelid), '; ') from pg_index i where i.indrelid = c.oid and (i.indisunique or i.indisprimary)) as unique_keys
from pg_class c join pg_namespace n on n.oid = c.relnamespace
where n.nspname = 'futures_data' and c.relname in ('ohlcv_1d', 'ohlcv_1d_raw', 'contract_ohlcv_1d') order by 1;
select exists (select 1 from pg_extension where extname = 'timescaledb') as has_tsdb \gset
\if :has_tsdb
select hypertable_name, num_chunks, compression_enabled from timescaledb_information.hypertables
where hypertable_schema = 'futures_data' order by 1;
\endif
do $$
declare bad text;
begin
  if current_setting('TimeZone') not in ('UTC', 'Etc/UTC') then
    raise exception 'fill preflight: session TimeZone is %, not UTC', current_setting('TimeZone');
  end if;
  select string_agg(n.nspname || '.' || c.relname, ', ') into bad
  from pg_class c join pg_namespace n on n.oid = c.relnamespace
  where c.relname like 'fill\_%' and n.nspname not like 'pg\_temp%' and n.nspname not like 'pg\_toast%';
  if bad is not null then raise exception 'fill preflight: permanent relations named fill_* exist (%): refused', bad; end if;
end $$;
\echo == preflight passed
