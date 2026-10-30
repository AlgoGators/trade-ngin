-- fill_apply.sql: the insert, in the transaction opened by fill_apply_begin.sql, after fill_core.sql (the holes
-- recomputed under the lock), the load of pg_temp.fill_expected and
-- fill_checks.sql (the rule's invariants). psql variables: tested_end (date), include_new (true/false).
-- INSERT ONLY: never updates or deletes a row; skips a (symbol, UTC date) either table already holds. Every check that
-- can fail runs before COMMIT and raises, which rolls everything back.
\set ON_ERROR_STOP on

-- Holes after the tested range are inserted only with include_new (the scripts always set it).
delete from pg_temp.fill_fill where d > :'tested_end'::date and not :include_new;

-- 1. The rows under the lock must be exactly the dry run's, key by key: symbol, date, contract, branch and raw id.
do $$
declare n_more int; n_less int;
begin
  select count(*) into n_more from (select symbol, d, instrument_id, branch, raw_instrument_id from pg_temp.fill_fill
    except select symbol, d, instrument_id, branch, raw_instrument_id from pg_temp.fill_expected) x;
  select count(*) into n_less from (select symbol, d, instrument_id, branch, raw_instrument_id from pg_temp.fill_expected
    except select symbol, d, instrument_id, branch, raw_instrument_id from pg_temp.fill_fill) x;
  if n_more + n_less > 0 then
    raise exception 'fill: refused, the rows to insert differ from the dry run (% new, % gone): the data moved; nothing inserted', n_more, n_less;
  end if;
end $$;

-- 2. The baseline: md5 per symbol of every .v.0 row of both tables, every column, before the insert, in this transaction.
create temp table fill_md5_before as
select 'ohlcv_1d' as tbl, o.symbol, count(*) as n,
       md5(string_agg(row(o.*)::text, E'\n' order by o.time, o.volume, o.open, o.high, o.low, o.close)) as md5
from futures_data.ohlcv_1d o where o.symbol like '%.v.0' group by o.symbol
union all
select 'ohlcv_1d_raw', r.symbol, count(*),
       md5(string_agg(row(r.*)::text, E'\n' order by r.ts_event, r.instrument_id, r.volume, r.open, r.high, r.low, r.close))
from futures_data.ohlcv_1d_raw r where r.symbol like '%.v.0' group by r.symbol;
do $$
declare n_keys int; n_sym int;
begin
  select count(*) into n_keys from pg_temp.fill_md5_before;
  select count(distinct symbol) into n_sym from futures_data.ohlcv_1d where symbol like '%.v.0';
  if n_keys = 0 or n_keys < 2 * n_sym then
    raise exception 'fill: the md5 baseline is incomplete (% keys for % symbols): nothing inserted', n_keys, n_sym;
  end if;
end $$;
create temp table fill_rows_before as
select (select count(*) from futures_data.ohlcv_1d where symbol like '%.v.0') as o,
       (select count(*) from futures_data.ohlcv_1d_raw where symbol like '%.v.0') as r;

-- 3. The insert: plain INSERT ... SELECT into both tables (no COPY, no DDL on the tables).
create temp table fill_ins_o as
with ins as (
  insert into futures_data.ohlcv_1d (time, symbol, volume, open, high, low, close)
  select f.ts, f.symbol, f.volume::integer, f.open, f.high, f.low, f.close
  from pg_temp.fill_fill f
  where not exists (select 1 from futures_data.ohlcv_1d o where o.symbol = f.symbol
                    and o.time >= f.ts and o.time < f.ts + interval '1 day')
  returning symbol, time)
select * from ins;
create temp table fill_ins_r as
with ins as (
  insert into futures_data.ohlcv_1d_raw (ts_event, symbol, open, high, low, close, volume, rtype, publisher_id, instrument_id)
  select f.ts, f.symbol, f.open, f.high, f.low, f.close, f.volume, f.rtype, f.publisher_id, f.raw_instrument_id
  from pg_temp.fill_fill f
  where not exists (select 1 from futures_data.ohlcv_1d_raw r where r.symbol = f.symbol
                    and r.ts_event >= f.ts and r.ts_event < f.ts + interval '1 day')
  returning symbol, ts_event)
select * from ins;
\echo == inserted: ohlcv_1d, ohlcv_1d_raw, rows expected
select (select count(*) from pg_temp.fill_ins_o) as ohlcv_1d, (select count(*) from pg_temp.fill_ins_r) as ohlcv_1d_raw,
       (select count(*) from pg_temp.fill_expected) as expected;

-- 4. The checks, before COMMIT; any failure raises and rolls back.
do $$
declare n_exp int; n_o int; n_r int; n_bad int; o0 bigint; r0 bigint; o1 bigint; r1 bigint;
begin
  select count(*) into n_exp from pg_temp.fill_expected;
  select count(*) into n_o from pg_temp.fill_ins_o;
  select count(*) into n_r from pg_temp.fill_ins_r;
  if n_o <> n_exp or n_r <> n_exp then
    raise exception 'fill: inserted % / % rows for % expected: rolled back', n_o, n_r, n_exp;
  end if;
  -- every expected row is in both tables, once, equal to its contract bar; the raw id is the expected one
  select count(*) into n_bad from pg_temp.fill_expected e
  where (select count(*) from futures_data.ohlcv_1d o join futures_data.contract_ohlcv_1d c
           on c.instrument_id = e.instrument_id and c.date = e.d
         where o.symbol = e.symbol and o.time = e.ts and o.open is not distinct from c.open and o.high is not distinct from c.high
           and o.low is not distinct from c.low and o.close is not distinct from c.close and o.volume = c.volume) <> 1
     or (select count(*) from futures_data.ohlcv_1d_raw r join futures_data.contract_ohlcv_1d c
           on c.instrument_id = e.instrument_id and c.date = e.d
         where r.symbol = e.symbol and r.ts_event = e.ts and r.instrument_id is not distinct from e.raw_instrument_id
           and (e.branch like 'V\_%') = (r.instrument_id is null)
           and r.open is not distinct from c.open and r.high is not distinct from c.high and r.low is not distinct from c.low
           and r.close is not distinct from c.close and r.volume = c.volume) <> 1;
  if n_bad > 0 then raise exception 'fill: % filled rows differ from their contract bar or expected raw id: rolled back', n_bad; end if;
  -- the pre-existing rows are unchanged: md5 per symbol of every row except the inserted ones
  create temp table fill_md5_after as
  select 'ohlcv_1d' as tbl, o.symbol, count(*) as n,
         md5(string_agg(row(o.*)::text, E'\n' order by o.time, o.volume, o.open, o.high, o.low, o.close)) as md5
  from futures_data.ohlcv_1d o where o.symbol like '%.v.0'
    and not exists (select 1 from pg_temp.fill_expected e where e.symbol = o.symbol and e.ts = o.time) group by o.symbol
  union all
  select 'ohlcv_1d_raw', r.symbol, count(*),
         md5(string_agg(row(r.*)::text, E'\n' order by r.ts_event, r.instrument_id, r.volume, r.open, r.high, r.low, r.close))
  from futures_data.ohlcv_1d_raw r where r.symbol like '%.v.0'
    and not exists (select 1 from pg_temp.fill_expected e where e.symbol = r.symbol and e.ts = r.ts_event) group by r.symbol;
  select count(*) into n_bad from pg_temp.fill_md5_before b full join pg_temp.fill_md5_after a using (tbl, symbol)
  where b.md5 is distinct from a.md5 or b.n is distinct from a.n;
  if n_bad > 0 then raise exception 'fill: the pre-existing rows changed on % symbol-table keys: rolled back', n_bad; end if;
  select o, r into o0, r0 from pg_temp.fill_rows_before;
  select count(*) into o1 from futures_data.ohlcv_1d where symbol like '%.v.0';
  select count(*) into r1 from futures_data.ohlcv_1d_raw where symbol like '%.v.0';
  if o1 <> o0 + n_exp or r1 <> r0 + n_exp then
    raise exception 'fill: row counts % -> %, % -> % for % inserted: rolled back', o0, o1, r0, r1, n_exp;
  end if;
  raise notice 'fill: % rows into each table; pre-existing rows unchanged on % keys', n_exp, (select count(*) from pg_temp.fill_md5_before);
end $$;
\echo == all checks passed before COMMIT
select tbl, count(*) as symbol_keys, sum(n) as pre_existing_rows from pg_temp.fill_md5_before group by tbl order by tbl;
commit;
\echo == COMMITTED
