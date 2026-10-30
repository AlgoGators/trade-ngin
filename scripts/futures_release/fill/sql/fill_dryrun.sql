-- fill_dryrun.sql: after fill_core.sql in the same session. Read-only: prints what fill_apply.sql would insert.
\set ON_ERROR_STOP on
\echo == DRY RUN: rows the fill would insert (each into futures_data.ohlcv_1d AND futures_data.ohlcv_1d_raw)
select count(*) as holes from fill_fill;
\echo == per class
select klass, count(*) as holes from fill_fill group by 1 order by 2 desc;
\echo == per root (source root) and class
select root, count(*) as holes,
       count(*) filter (where klass = 'sunday') as sunday, count(*) filter (where klass = 'thin') as thin,
       count(*) filter (where klass = 'market_wide') as market_wide, count(*) filter (where klass = 'post_holiday') as post_holiday,
       count(*) filter (where klass = 'on_holiday') as on_holiday, count(*) filter (where klass = 'block_2025') as block_2025,
       count(*) filter (where klass = 'other') as other
from fill_fill group by 1 order by 2 desc, 1;
\echo == per rule branch
select branch, count(*) from fill_fill group by 1 order by 2 desc;
\echo == rtype and publisher_id copied from the nearest earlier real raw row
select rtype, publisher_id, count(*) from fill_fill group by 1, 2;
\echo == refusals (must all be 0): no contract, a bar not found, a volume over the integer column
select count(*) filter (where instrument_id is null) as no_contract, count(*) filter (where close is null) as no_bar,
       count(*) filter (where volume > 2147483647) as volume_overflow from fill_fill;
