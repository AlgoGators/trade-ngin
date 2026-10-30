-- schema_fingerprint.sql : read only. Every column (schema, table, column, type, nullable) and every
-- constraint (schema, table, name, type, definition) of the schemas the engine and the data pulls use, one line each,
-- sorted. Two fingerprints of one database differ only where the schema changed. Comments are not part of it.
select 'column ' || table_schema || '.' || table_name || '.' || column_name || ' ' || data_type || ' ' || is_nullable
from information_schema.columns
where table_schema in ('trading', 'backtest', 'futures_data', 'metadata', 'ops', 'equities_data')
union all
select 'constraint ' || n.nspname || '.' || c.relname || '.' || k.conname || ' ' || k.contype::text || ' ' || pg_get_constraintdef(k.oid)
from pg_constraint k join pg_class c on c.oid = k.conrelid join pg_namespace n on n.oid = c.relnamespace
where n.nspname in ('trading', 'backtest', 'futures_data', 'metadata', 'ops', 'equities_data')
order by 1;
