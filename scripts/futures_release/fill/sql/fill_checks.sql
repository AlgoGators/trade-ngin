-- fill_checks.sql: after fill_core.sql in the same session. The rule's invariants on pg_temp.fill_fill, re-derived
-- from the tables independently of the core's own columns; any violation raises (and rolls the transaction back).
-- Run in the dry run and again under the lock, before any insert.
\set ON_ERROR_STOP on
do $$
declare n_total int; n_bad int; msg text;
begin
  select count(*) into n_total from pg_temp.fill_fill;
  -- 1. every row: a contract bar of the source root on the date, the bar's values copied exactly
  select count(*) into n_bad from pg_temp.fill_fill f
  where not exists (select 1 from futures_data.contract_ohlcv_1d c where c.instrument_id = f.instrument_id and c.date = f.d
                    and c.root = f.root and c.open is not distinct from f.open and c.high is not distinct from f.high
                    and c.low is not distinct from f.low and c.close is not distinct from f.close and c.volume = f.volume);
  if n_bad > 0 then raise exception 'fill check 1: % rows are not their contract''s bar of the source root', n_bad; end if;
  -- 2. the branch follows from the neighbours: B when both real neighbours name the same contract with a bar on the date;
  --    else C when the earlier neighbour's contract (of the source root) has a bar on the date; else V
  with x as (
    select f.*,
      (f.after_id is not null and f.before_id = f.after_id and exists (select 1 from futures_data.contract_ohlcv_1d c
         where c.instrument_id = f.before_id and c.date = f.d)) as b_ok,
      exists (select 1 from futures_data.contract_ohlcv_1d c where c.instrument_id = f.before_id and c.date = f.d
              and c.root = f.root) as c_ok
    from pg_temp.fill_fill f)
  select count(*) into n_bad from x
  where not ((b_ok and branch = 'B_bracket' and instrument_id = before_id)
          or (not b_ok and c_ok and branch = 'C_carry_previous_vendor' and instrument_id = before_id)
          or (not b_ok and not c_ok and branch like 'V\_%'));
  if n_bad > 0 then raise exception 'fill check 2: % rows do not follow B, then C, then V', n_bad; end if;
  -- 3. the raw id: the contract's on B and C, NULL on V
  select count(*) into n_bad from pg_temp.fill_fill
  where (branch like 'V\_%' and raw_instrument_id is not null)
     or (branch not like 'V\_%' and raw_instrument_id is distinct from instrument_id);
  if n_bad > 0 then raise exception 'fill check 3: % rows have the wrong raw instrument_id (contract on B/C, NULL on V)', n_bad; end if;
  -- 4. never a Saturday, never before the symbol's first real row, never a date either table holds
  select count(*) into n_bad from pg_temp.fill_fill f
  where extract(isodow from f.d) = 6
     or not exists (select 1 from futures_data.ohlcv_1d_raw r where r.symbol = f.symbol and r.ts_event < f.ts)
     or exists (select 1 from futures_data.ohlcv_1d o where o.symbol = f.symbol and o.time >= f.ts and o.time < f.ts + interval '1 day')
     or exists (select 1 from futures_data.ohlcv_1d_raw r where r.symbol = f.symbol and r.ts_event >= f.ts and r.ts_event < f.ts + interval '1 day');
  if n_bad > 0 then raise exception 'fill check 4: % rows on a Saturday, before a series start, or on a date already held', n_bad; end if;
  -- 5. no volume over the integer column, rtype and publisher_id present
  select count(*) into n_bad from pg_temp.fill_fill where volume > 2147483647 or rtype is null or publisher_id is null;
  if n_bad > 0 then raise exception 'fill check 5: % rows overflow the volume column or lack rtype/publisher_id', n_bad; end if;
  raise notice 'fill checks 1-5 passed on % rows', n_total;
end $$;
\echo == rule checks 1-5 passed
