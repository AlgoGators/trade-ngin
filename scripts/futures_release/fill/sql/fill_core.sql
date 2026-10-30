-- fill_core.sql: select every missing futures continuous row (<ROOT>.v.0) that the contract table can rebuild.
-- READ-ONLY. Builds temp tables only; fill_dryrun.sql and fill_apply.sql run after it in the same psql session.
--
-- A hole is a (symbol, UTC date) with:
--   - symbol a <ROOT>.v.0 already present in futures_data.ohlcv_1d;
--   - the date inside that symbol's own span (its first to its last existing row), not a Saturday;
--   - no row of that symbol on that UTC date in futures_data.ohlcv_1d nor in futures_data.ohlcv_1d_raw;
--   - at least one contract bar of the SOURCE ROOT on that date in futures_data.contract_ohlcv_1d.
-- The source root is the root of the contract the vendor named on the symbol's nearest earlier existing row
-- (ohlcv_1d_raw.instrument_id joined to contract_ohlcv_1d on instrument_id AND date: ids are reused across years).
-- So MES.v.0 reads ES before 2026-02-22 and MES after, as its existing rows do.
--
-- The contract (rule B, then rule C, then rule V):
--   B "bracket": the nearest existing rows before and after the hole name the same contract, and it has a bar on
--     the date: that contract.
--   C "carry", otherwise, when the contract the nearest existing row BEFORE the hole names has a bar on the date: that
--     contract. A filled day then never shows the roll rule a vendor switch the vendor did not make: the switch stays
--     on the next real row.
--   V "volume", otherwise. Weekday d: the busiest contract of the source root on the previous weekday that root has
--     contract rows (ties: earlier expiration, then lower instrument id); if it has no bar on d, the busiest of d.
--     Saturday or Sunday d: the previous weekday's V pick, if it has a bar on d; else the busiest of d.
-- The filled row is that contract's bar exactly: open, high, low, close, volume; time = the date at 00:00 UTC.
-- The raw row: the same bar, rtype and publisher_id copied from the symbol's nearest earlier existing raw row, and
-- instrument_id = the contract's for B and C; NULL for V: the vendor's contract on that day is not
-- known, and the roll rule reads a NULL id as no vendor information, so a V day never shows it a vendor switch.
\set ON_ERROR_STOP on
set timezone = 'UTC';

drop table if exists pg_temp.fill_ex;
create temp table fill_ex as
select distinct on (r.symbol, (r.ts_event at time zone 'UTC')::date)
       r.symbol, (r.ts_event at time zone 'UTC')::date as d, r.instrument_id as vid, c.root as vroot,
       r.rtype, r.publisher_id
from futures_data.ohlcv_1d_raw r
left join futures_data.contract_ohlcv_1d c on c.instrument_id = r.instrument_id and c.date = (r.ts_event at time zone 'UTC')::date
where r.symbol like '%.v.0'
order by r.symbol, (r.ts_event at time zone 'UTC')::date, r.ts_event;
create index on fill_ex (symbol, d);

-- every UTC date that has a continuous row (either table), per symbol
drop table if exists pg_temp.fill_have;
create temp table fill_have as
select symbol, (time at time zone 'UTC')::date as d from futures_data.ohlcv_1d where symbol like '%.v.0'
union
select symbol, d from fill_ex;
create index on fill_have (symbol, d);

drop table if exists pg_temp.fill_span;
create temp table fill_span as
select symbol, min(d) as first_d, max(d) as last_d from fill_have group by symbol;

-- the roots each symbol's existing rows were sourced from
drop table if exists pg_temp.fill_symroot;
create temp table fill_symroot as
select distinct symbol, vroot as root from fill_ex where vroot is not null;

drop table if exists pg_temp.fill_rootdates;
create temp table fill_rootdates as
select distinct root, date from futures_data.contract_ohlcv_1d;
create index on fill_rootdates (root, date);

-- candidate (symbol, date, root), then keep the root the nearest earlier existing row was sourced from
drop table if exists pg_temp.fill_hole;
create temp table fill_hole as
select h.symbol, h.d, h.root
from (
  select sr.symbol, rd.date as d, rd.root
  from fill_symroot sr
  join fill_span sp on sp.symbol = sr.symbol
  join fill_rootdates rd on rd.root = sr.root and rd.date between sp.first_d and sp.last_d
  where extract(isodow from rd.date) <> 6
    and not exists (select 1 from fill_have hv where hv.symbol = sr.symbol and hv.d = rd.date)
) h
where h.root = (select e.vroot from fill_ex e where e.symbol = h.symbol and e.d < h.d and e.vroot is not null
                order by e.d desc limit 1);
create index on fill_hole (symbol, d);

-- daily ranking of the contracts of the roots that have holes
drop table if exists pg_temp.fill_rank;
create temp table fill_rank as
select c.root, c.date, c.instrument_id, c.volume,
       row_number() over (partition by c.root, c.date order by c.volume desc, c.expiration asc nulls last, c.instrument_id asc) as rk
from futures_data.contract_ohlcv_1d c
where c.root in (select distinct root from fill_hole);
create index on fill_rank (root, date) where rk = 1;

-- rule V on a weekday date of a root
create or replace function pg_temp.fill_v_weekday(p_root text, p_d date, out pick text, out branch text) as $$
declare pw date; tp text; ts text;
begin
  select max(date) into pw from fill_rootdates where root = p_root and date < p_d and extract(isodow from date) < 6;
  select instrument_id into ts from fill_rank where root = p_root and date = p_d and rk = 1;
  if pw is not null then
    select instrument_id into tp from fill_rank where root = p_root and date = pw and rk = 1;
    if exists (select 1 from futures_data.contract_ohlcv_1d where instrument_id = tp and date = p_d) then
      pick := tp; branch := 'V_prev_weekday_busiest'; return;
    end if;
    pick := ts; branch := 'V_fallback_same_day_busiest'; return;
  end if;
  pick := ts; branch := 'V_first_date_same_day_busiest';
end $$ language plpgsql stable;

create or replace function pg_temp.fill_v(p_root text, p_d date, out pick text, out branch text) as $$
declare pw date; wp text; ts text;
begin
  if extract(isodow from p_d) < 6 then
    select x.pick, x.branch into pick, branch from pg_temp.fill_v_weekday(p_root, p_d) x; return;
  end if;
  select max(date) into pw from fill_rootdates where root = p_root and date < p_d and extract(isodow from date) < 6;
  select instrument_id into ts from fill_rank where root = p_root and date = p_d and rk = 1;
  if pw is not null then
    select x.pick into wp from pg_temp.fill_v_weekday(p_root, pw) x;
    if exists (select 1 from futures_data.contract_ohlcv_1d where instrument_id = wp and date = p_d) then
      pick := wp; branch := 'V_weekend_prev_weekday_pick'; return;
    end if;
    pick := ts; branch := 'V_fallback_same_day_busiest'; return;
  end if;
  pick := ts; branch := 'V_first_date_same_day_busiest';
end $$ language plpgsql stable;

drop table if exists pg_temp.fill_pick;
create temp table fill_pick as
select h.symbol, h.d, h.root,
       b.vid as before_id, b.d as before_d, b.rtype, b.publisher_id, a.vid as after_id, a.d as after_d,
       v.pick as v_pick, v.branch as v_branch
from fill_hole h
cross join lateral (select e.vid, e.d, e.rtype, e.publisher_id from fill_ex e where e.symbol = h.symbol and e.d < h.d order by e.d desc limit 1) b
left join lateral (select e.vid, e.d from fill_ex e where e.symbol = h.symbol and e.d > h.d order by e.d asc limit 1) a on true
cross join lateral pg_temp.fill_v(h.root, h.d) v;

drop table if exists pg_temp.fill_fill;
create temp table fill_fill as
select p.symbol, p.d, (p.d::timestamp at time zone 'UTC') as ts, p.root,
       case when bo.instrument_id is not null or co.instrument_id is not null then p.before_id else p.v_pick end as instrument_id,
       case when bo.instrument_id is not null then 'B_bracket' when co.instrument_id is not null then 'C_carry_previous_vendor'
            else p.v_branch end as branch,
       p.before_d, p.before_id, p.after_d, p.after_id, p.v_pick, p.rtype, p.publisher_id
from fill_pick p
left join futures_data.contract_ohlcv_1d bo
  on p.after_id is not null and p.before_id = p.after_id and bo.instrument_id = p.before_id and bo.date = p.d
left join futures_data.contract_ohlcv_1d co
  on co.instrument_id = p.before_id and co.date = p.d and co.root = p.root;
alter table fill_fill add column raw_symbol text, add column expiration date, add column open double precision,
  add column high double precision, add column low double precision, add column close double precision,
  add column volume bigint, add column klass text, add column holes_that_day int, add column raw_instrument_id text;
update fill_fill f set raw_symbol = c.raw_symbol, expiration = c.expiration, open = c.open, high = c.high,
  low = c.low, close = c.close, volume = c.volume
from futures_data.contract_ohlcv_1d c where c.instrument_id = f.instrument_id and c.date = f.d;
update fill_fill set raw_instrument_id = case when branch like 'V\_%' then null else instrument_id end;

-- the class (first that applies)
-- US exchange holidays 2010-2027 (include/trade_ngin/core/holidays.json); post_holiday = the previous
-- weekday is one of them (Good Friday included, so Easter Monday counts)
drop table if exists pg_temp.fill_holiday;
create temp table fill_holiday (h date);
insert into fill_holiday values (date '2010-01-01'),(date '2010-01-18'),(date '2010-02-15'),(date '2010-04-02'),(date '2010-05-31'),(date '2010-07-05'),(date '2010-09-06'),(date '2010-11-25'),(date '2010-12-24'),(date '2011-01-17'),(date '2011-02-21'),(date '2011-04-22'),(date '2011-05-30'),(date '2011-07-04'),(date '2011-09-05'),(date '2011-11-24'),(date '2011-12-26'),(date '2012-01-02'),(date '2012-01-16'),(date '2012-02-20'),(date '2012-04-06'),(date '2012-05-28'),(date '2012-07-04'),(date '2012-09-03'),(date '2012-10-29'),(date '2012-10-30'),(date '2012-11-22'),(date '2012-12-25'),(date '2013-01-01'),(date '2013-01-21'),(date '2013-02-18'),(date '2013-03-29'),(date '2013-05-27'),(date '2013-07-04'),(date '2013-09-02'),(date '2013-11-28'),(date '2013-12-25'),(date '2014-01-01'),(date '2014-01-20'),(date '2014-02-17'),(date '2014-04-18'),(date '2014-05-26'),(date '2014-07-04'),(date '2014-09-01'),(date '2014-11-27'),(date '2014-12-25'),(date '2015-01-01'),(date '2015-01-19'),(date '2015-02-16'),(date '2015-04-03'),(date '2015-05-25'),(date '2015-07-03'),(date '2015-09-07'),(date '2015-11-26'),(date '2015-12-25'),(date '2016-01-01'),(date '2016-01-18'),(date '2016-02-15'),(date '2016-03-25'),(date '2016-05-30'),(date '2016-07-04'),(date '2016-09-05'),(date '2016-11-24'),(date '2016-12-26'),(date '2017-01-02'),(date '2017-01-16'),(date '2017-02-20'),(date '2017-04-14'),(date '2017-05-29'),(date '2017-07-04'),(date '2017-09-04'),(date '2017-11-23'),(date '2017-12-25'),(date '2018-01-01'),(date '2018-01-15'),(date '2018-02-19'),(date '2018-03-30'),(date '2018-05-28'),(date '2018-07-04'),(date '2018-09-03'),(date '2018-11-22'),(date '2018-12-05'),(date '2018-12-25'),(date '2019-01-01'),(date '2019-01-21'),(date '2019-02-18'),(date '2019-04-19'),(date '2019-05-27'),(date '2019-07-04'),(date '2019-09-02'),(date '2019-11-28'),(date '2019-12-25'),(date '2020-01-01'),(date '2020-01-20'),(date '2020-02-17'),(date '2020-04-10'),(date '2020-05-25'),(date '2020-07-03'),(date '2020-09-07'),(date '2020-11-26'),(date '2020-12-25'),(date '2021-01-01'),(date '2021-01-18'),(date '2021-02-15'),(date '2021-04-02'),(date '2021-05-31'),(date '2021-07-05'),(date '2021-09-06'),(date '2021-11-25'),(date '2021-12-24'),(date '2022-01-17'),(date '2022-02-21'),(date '2022-04-15'),(date '2022-05-30'),(date '2022-06-20'),(date '2022-07-04'),(date '2022-09-05'),(date '2022-11-24'),(date '2022-12-26'),(date '2023-01-02'),(date '2023-01-16'),(date '2023-02-20'),(date '2023-04-07'),(date '2023-05-29'),(date '2023-06-19'),(date '2023-07-04'),(date '2023-09-04'),(date '2023-11-23'),(date '2023-12-25'),(date '2024-01-01'),(date '2024-01-15'),(date '2024-02-19'),(date '2024-03-29'),(date '2024-05-27'),(date '2024-06-19'),(date '2024-07-04'),(date '2024-09-02'),(date '2024-11-28'),(date '2024-12-25'),(date '2025-01-01'),(date '2025-01-09'),(date '2025-01-20'),(date '2025-02-17'),(date '2025-04-18'),(date '2025-05-26'),(date '2025-06-19'),(date '2025-07-04'),(date '2025-09-01'),(date '2025-11-27'),(date '2025-12-25'),(date '2026-01-01'),(date '2026-01-19'),(date '2026-02-16'),(date '2026-04-03'),(date '2026-05-25'),(date '2026-06-19'),(date '2026-07-03'),(date '2026-09-07'),(date '2026-11-26'),(date '2026-12-25'),(date '2027-01-01'),(date '2027-01-18'),(date '2027-02-15'),(date '2027-03-26'),(date '2027-05-31'),(date '2027-06-18'),(date '2027-07-05'),(date '2027-09-06'),(date '2027-11-25'),(date '2027-12-24');
update fill_fill f set holes_that_day = n.k from (select d, count(*) k from fill_fill group by d) n where n.d = f.d;
update fill_fill f set klass = case
  when extract(isodow from f.d) = 7 then 'sunday'
  when f.root in ('SI','ZC','ZS') and f.d between date '2025-03-24' and date '2025-04-04' then 'block_2025'
  when f.holes_that_day >= 10 then 'market_wide'
  when f.d in (select h from fill_holiday) then 'on_holiday'
  when (f.d - case when extract(isodow from f.d) = 1 then 3 else 1 end) in (select h from fill_holiday)
    then 'post_holiday'
  when f.volume < 100 then 'thin'
  else 'other' end;
create index on fill_fill (symbol, d);
