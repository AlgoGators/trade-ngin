-- fill_dryrun_guard.sql: last statement of the dry-run session (BEGIN ... ROLLBACK). Proves the session inserted,
-- updated or deleted no row of any permanent table (only its own temp tables), then rolls back.
\set ON_ERROR_STOP on
\echo == dry-run guard: rows written to permanent tables by this session (must be 0)
select coalesce(sum(n_tup_ins + n_tup_upd + n_tup_del), 0) as permanent_rows_written
from pg_stat_xact_user_tables where schemaname not like 'pg_temp%';
do $$ declare n bigint; begin
  select coalesce(sum(n_tup_ins + n_tup_upd + n_tup_del), 0) into n from pg_stat_xact_user_tables where schemaname not like 'pg_temp%';
  if n <> 0 then raise exception 'fill dry run wrote % permanent rows', n; end if;
end $$;
rollback;
