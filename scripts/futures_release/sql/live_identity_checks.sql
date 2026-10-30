-- live_identity_checks.sql : every identity of one live book over :s .. :e. Read only. One line per check: name | PASS or
-- FAIL | detail. Run: psql -At -F' | ' -v pid=CONSERVATIVE_PORTFOLIO -v s=2025-10-05 -v e=<last date> -f this file
      with r as (select date::date d, * from trading.live_results where portfolio_id=:'pid' and date::date between :'s' and :'e'),
      cal as (select generate_series(:'s'::date, :'e'::date, interval '1 day')::date d),
      x as (select d, current_portfolio_value - lag(current_portfolio_value) over w - daily_pnl e_value,
                   daily_pnl - (daily_realized_pnl + daily_unrealized_pnl - daily_transaction_costs) e_pnl,
                   total_transaction_costs - lag(total_transaction_costs) over w - daily_transaction_costs e_tc,
                   total_roll_costs - lag(total_roll_costs) over w - daily_roll_costs e_rc,
                   current_portfolio_value - total_pnl base, settled_at is not null st from r window w as (order by d)),
      ex as (select date::date d, sum(total_transaction_costs) c, coalesce(sum(total_transaction_costs) filter (where execution_type='ROLL'),0) rc
             from trading.executions where portfolio_id=:'pid' and date::date between :'s' and :'e' group by 1),
      rl as (select date::date d, symbol, count(*) n, sum(case when side='BUY' then quantity else -quantity end) q
             from trading.executions where portfolio_id=:'pid' and execution_type='ROLL' and date::date between :'s' and :'e' group by 1,2),
      ss as (select distinct symbol from trading.positions where portfolio_id=:'pid' union select distinct symbol from trading.executions where portfolio_id=:'pid'),
      g as (select r.d, ss.symbol, coalesce(p.quantity,0) q from r cross join ss
            left join trading.positions p on p.portfolio_id=:'pid' and p.symbol=ss.symbol and p.date::date=r.d),
      gq as (select d, symbol, q - lag(q) over (partition by symbol order by d) dq from g),
      fl as (select date::date d, symbol, sum(case when side='BUY' then quantity else -quantity end) q
             from trading.executions where portfolio_id=:'pid' and execution_type<>'ROLL' group by 1,2)
      select 'one row per calendar date', case when (select count(*) from cal where d not in (select d from r))=0 and (select count(*) from r)=(select count(distinct d) from r) then 'PASS' else 'FAIL' end,
             (select count(*) from cal where d not in (select d from r))||' missing, '||((select count(*) from r)-(select count(distinct d) from r))||' duplicate'
      union all select 'value moves by the day P&L', case when max(abs(e_value))<0.01 then 'PASS' else 'FAIL' end, 'largest gap '||round(coalesce(max(abs(e_value)),0),4) from x
      union all select 'day P&L = realized + unrealized - costs', case when max(abs(e_pnl))<0.01 then 'PASS' else 'FAIL' end, 'largest gap '||round(coalesce(max(abs(e_pnl)),0),4) from x
      union all select 'total P&L = value - starting capital', case when max(base)-min(base)<0.01 then 'PASS' else 'FAIL' end, 'starting capital '||round(min(base),2)||' .. '||round(max(base),2) from x
      union all select 'cumulative costs add up', case when max(abs(e_tc))<0.01 and max(abs(e_rc))<0.01 then 'PASS' else 'FAIL' end, 'largest gaps '||round(coalesce(max(abs(e_tc)),0),4)||' and '||round(coalesce(max(abs(e_rc)),0),4)||' (roll)' from x
      union all select 'day costs = the day''s fills', case when count(*) filter (where abs(coalesce(r.daily_transaction_costs,0)-coalesce(ex.c,0))>0.005 or abs(coalesce(r.daily_roll_costs,0)-coalesce(ex.rc,0))>0.005)=0 then 'PASS' else 'FAIL' end,
             count(*) filter (where abs(coalesce(r.daily_transaction_costs,0)-coalesce(ex.c,0))>0.005 or abs(coalesce(r.daily_roll_costs,0)-coalesce(ex.rc,0))>0.005)||' days differ, '||count(*) filter (where r.d is null)||' days of fills with no day row' from r full join ex using (d)
      union all select 'each roll is two legs that net to zero', case when count(*) filter (where n<>2 or abs(q)>1e-9)=0 then 'PASS' else 'FAIL' end, count(*)||' rolls, '||count(*) filter (where n<>2 or abs(q)>1e-9)||' bad' from rl
      union all select 'positions change only by the day''s trades', case when count(*)=0 then 'PASS' else 'FAIL' end, count(*)||' symbol-days differ' from gq left join fl using (d,symbol) where dq is not null and abs(dq-coalesce(fl.q,0))>1e-9
      union all select 'every day settled but the last', case when count(*) filter (where not st and d<:'e')=0 then 'PASS' else 'FAIL' end, count(*) filter (where not st and d<:'e')||' unsettled before the last day' from x;
