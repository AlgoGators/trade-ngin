-- readonly_checks.sql : the read-only checks on production before the data steps and between steps. Every
-- statement is a SELECT; run it only through 01_readonly_checks.sh, which forces default_transaction_read_only.
-- Each line: check | answer. The expected answers are in EXPECTED.md.
\pset footer off
select 'server' as check, current_database() || ' tz=' || current_setting('TimeZone') || ' pg ' || current_setting('server_version')
       || ' timescaledb ' || coalesce((select extversion from pg_extension where extname = 'timescaledb'), 'none')
       || ' size ' || pg_size_pretty(pg_database_size(current_database())) as answer;
-- the universe: .v.0 symbols, .c.0 symbols, the full-size equity roots (a micro stored beside its full-size root with no
-- pair refuses every runner, c58dd35b), the micros
select 'v0 symbols', count(distinct symbol) || ': ' || string_agg(distinct symbol, ' ' order by symbol) from futures_data.ohlcv_1d where symbol like '%.v.0';
select 'c0 symbols', count(distinct symbol) || coalesce(': ' || string_agg(distinct symbol, ' ' order by symbol), '') from futures_data.ohlcv_1d where symbol like '%.c.0';
select 'full-size equity v0 (ES NQ YM RTY)', count(*) || coalesce(' rows: ' || string_agg(distinct symbol, ' '), '') from futures_data.ohlcv_1d where symbol in ('ES.v.0', 'NQ.v.0', 'YM.v.0', 'RTY.v.0');
select 'ES rows (any table)', (select count(*) from futures_data.ohlcv_1d where symbol like 'ES.%') || ' ohlcv_1d, ' || (select count(*) from futures_data.ohlcv_1d_raw where symbol like 'ES.%') || ' raw, '
       || (select count(*) from futures_data.contract_ohlcv_1d where root = 'ES') || ' contract (root ES, read only through MES before 2026-02-22)';
select 'MGC/SIL stored', (select count(*) from futures_data.contract_ohlcv_1d where root in ('MGC', 'SIL')) || ' contract, ' || (select count(*) from futures_data.ohlcv_1d where symbol in ('MGC.v.0', 'SIL.v.0')) || ' ohlcv_1d, '
       || (select count(*) from futures_data.ohlcv_1d_raw where symbol in ('MGC.v.0', 'SIL.v.0')) || ' raw, ' || (select count(*) from metadata.contract_metadata where "Databento Symbol" in ('MGC', 'SIL')) || ' metadata';
-- the last bars: per table, and the roots whose last bar is older than the newest weekday bar
select 'last bar', 'ohlcv_1d ' || (select max(time)::date from futures_data.ohlcv_1d) || ', raw ' || (select max(ts_event)::date from futures_data.ohlcv_1d_raw) || ', contract ' || (select max(date) from futures_data.contract_ohlcv_1d);
with l as (select symbol, max(time)::date d from futures_data.ohlcv_1d where symbol like '%.v.0' group by 1),
     m as (select max(d) filter (where extract(isodow from d) < 6) w from l)
select 'symbols behind the newest weekday bar', count(*) || coalesce(': ' || string_agg(symbol || ' ' || d, ', ' order by symbol), '') from l, m where l.d < m.w;
select 'data_ready last', coalesce((select dataset || ' ' || bar_date || ' ' || status || ' ' || loaded || '/' || expected from ops.data_ready order by bar_date desc, dataset limit 1), 'none');
-- the books: CONSERVATIVE's stored live days and registry; every book's last-day GC/SI/MGC/SIL holdings
select 'CONSERVATIVE live_results', count(*) || ' rows, ' || coalesce(min(date)::text, '-') || ' .. ' || coalesce(max(date)::text, '-') from trading.live_results where portfolio_id = 'CONSERVATIVE_PORTFOLIO';
select 'CONSERVATIVE registry', coalesce(string_agg(id || ' ' || strategy_type || ' ' || name || ' active=' || is_active, '; '), 'none') from trading.strategy_registry where portfolio_id = 'CONSERVATIVE_PORTFOLIO';
select 'CONSERVATIVE live_start_date', coalesce(string_agg(strategy_id || ' ' || live_start_date, '; '), 'no row') from trading.strategy_trading_days_metadata where portfolio_id = 'CONSERVATIVE_PORTFOLIO';
select 'CONSERVATIVE position_overrides / strategy_config', (select count(*) from trading.position_overrides where portfolio_id = 'CONSERVATIVE_PORTFOLIO') || ' / ' || (select count(*) from trading.strategy_config where portfolio_id = 'CONSERVATIVE_PORTFOLIO');
with last as (select portfolio_id, max(date) d from trading.positions group by 1)
select 'held GC/SI/MGC/SIL on each book''s last day', coalesce(string_agg(p.portfolio_id || ' ' || p.date || ' ' || p.symbol || ' ' || p.quantity, '; ' order by p.portfolio_id, p.symbol), 'none')
from trading.positions p join last on last.portfolio_id = p.portfolio_id and last.d = p.date
where p.symbol in ('GC.v.0', 'SI.v.0', 'MGC.v.0', 'SIL.v.0') and p.quantity <> 0;
select 'QT books last live day', coalesce(string_agg(portfolio_id || ' ' || d, '; ' order by portfolio_id), 'none')
from (select portfolio_id, max(date) d from trading.live_results where portfolio_id like 'QT%' group by 1) q;
-- the precondition of 07 (part B): the QT books' last rows and the symbols each holds and signals on its last day.
-- Once MGC.v.0 and SIL.v.0 are stored, a QT run on code without the micros' pairs would take them in beside GC and SI.
with l as (select portfolio_id, max(date) d from trading.live_results where portfolio_id like 'QT%' group by 1)
select 'QT ' || l.portfolio_id || ' last ' || l.d,
       'held: ' || coalesce((select string_agg(portfolio_type || ':' || symbol || ' ' || round(quantity, 2), ', ' order by portfolio_type, symbol) from trading.positions p
                             where p.portfolio_id = l.portfolio_id and p.date = (select max(date) from trading.positions q where q.portfolio_id = l.portfolio_id) and p.quantity <> 0), 'none')
       || '; signals on ' || coalesce((select count(distinct symbol) || ' symbols, micros among them: ' || coalesce(string_agg(distinct symbol, ' ') filter (where symbol in ('MGC.v.0', 'SIL.v.0', 'MGC', 'SIL')), 'none')
                                       from trading.signals s where s.portfolio_id = l.portfolio_id and (s.timestamp at time zone 'UTC')::date = (select max((timestamp at time zone 'UTC')::date) from trading.signals t where t.portfolio_id = l.portfolio_id)), '-')
from l order by 1;
-- every book with live rows: rows, first and last day; and its stored backtests (for the list of books and seeds)
select 'book ' || coalesce(l.portfolio_id, b.portfolio_id),
       coalesce(l.n || ' live rows ' || l.f || ' .. ' || l.t, 'no live rows') || '; ' || coalesce(b.n, 0) || ' backtests, newest ' || coalesce(b.newest, '-')
from (select portfolio_id, count(*) n, min(date) f, max(date) t from trading.live_results group by 1) l
full join (select portfolio_id, count(*) n, max(run_id) newest from backtest.results group by 1) b using (portfolio_id)
where coalesce(l.portfolio_id, b.portfolio_id) in ('CONSERVATIVE_PORTFOLIO', 'BASE_PORTFOLIO', 'QT_CONSERVATIVE_PORTFOLIO', 'QT_CONSERVATIVE_MODEL_PORTFOLIO', 'QT_E2E_PORTFOLIO', 'EQUITY_MR_PORTFOLIO')
order by 1;
-- the stored backtests of the book (the seed adds four)
select 'CONSERVATIVE backtests stored', count(*) || ', newest ' || coalesce(max(run_id), '-') from backtest.results where portfolio_id = 'CONSERVATIVE_PORTFOLIO';
-- the fill's temp tables share the names fill_*: a permanent relation of that name makes its preflight refuse
select 'permanent fill_ relations', count(*) from pg_class c join pg_namespace n on n.oid = c.relnamespace
where c.relname like 'fill\_%' and n.nspname not like 'pg\_temp%' and n.nspname not like 'pg\_toast%';
-- 040 (step 03)
select '040', (select count(*) from information_schema.columns where column_name in ('roll_bar_move', 'roll_gap_points', 'roll_gap_value', 'roll_bar_moves', 'roll_gap_values') and table_schema in ('trading', 'backtest')) || ' of 8 columns, '
       || (select count(*) from pg_constraint where conname in ('chk_executions_roll_split', 'chk_backtest_executions_roll_split')) || ' of 2 CHECKs';
