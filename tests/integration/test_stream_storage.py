"""Real typed-writer isolation against an owned, network-none PostgreSQL fixture."""
import os
import subprocess
import json
from pathlib import Path
from tests.qt_test_artifacts import artifact
import pytest
from test_runtime_control_schema import connection

PROBE=artifact("stream_storage_probe")

@pytest.fixture()
def streams(connection):
    with connection.cursor() as cur:
        cur.execute("""
          ALTER TABLE trading.live_results ADD total_transaction_costs numeric DEFAULT 0, ADD created_at timestamptz DEFAULT now();
          ALTER TABLE trading.executions ADD strategy_name text, ADD date date, ADD exec_id varchar(50), ADD order_id varchar(50),
            ADD symbol text, ADD side text, ADD quantity numeric, ADD price numeric,
            ADD execution_time timestamptz, ADD commissions_fees numeric,
            ADD implicit_price_impact numeric, ADD slippage_market_impact numeric,
            ADD total_transaction_costs numeric, ADD is_partial boolean;
          ALTER TABLE trading.live_results ADD id bigserial PRIMARY KEY,
            ALTER strategy_id SET NOT NULL, ALTER date TYPE date, ALTER date SET NOT NULL,
            ADD CONSTRAINT live_results_portfolio_strategy_date_key UNIQUE(portfolio_id,strategy_id,date);
          ALTER TABLE trading.executions ALTER portfolio_id SET NOT NULL, ALTER strategy_id SET NOT NULL,
            ALTER strategy_name SET NOT NULL, ALTER date SET NOT NULL, ALTER exec_id SET NOT NULL,
            ALTER order_id SET NOT NULL,
            ADD PRIMARY KEY(portfolio_id,strategy_id,strategy_name,date,exec_id),
            ADD CONSTRAINT chk_executions_quantity CHECK(quantity>0);
        """)
        cur.execute((Path(__file__).parents[2]/'migrations/014_execution_result_streams.sql').read_text())
        cur.execute("""
          INSERT INTO trading.live_results (strategy_id,portfolio_id,date,portfolio_type,total_pnl,current_portfolio_value)
            SELECT 'LIVE_TREND','BOOK','2026-09-22',s,99,9900 FROM unnest(ARRAY['system','qt']) s;
          INSERT INTO trading.live_results (strategy_id,portfolio_id,date,portfolio_type,total_pnl,current_portfolio_value)
            VALUES ('LIVE_TREND','BOOK','2026-09-20','system',10,1000),
                   ('LIVE_TREND','BOOK','2026-09-21','qt',999,99000);
          INSERT INTO trading.equity_curve VALUES
            ('LIVE_TREND','BOOK','2026-09-22',9900,'system'),('LIVE_TREND','BOOK','2026-09-22',9900,'qt');
          INSERT INTO trading.executions (strategy_id,portfolio_id,strategy_name,date,exec_id,order_id,execution_time,portfolio_type,quantity)
            SELECT 'LIVE_TREND','BOOK','TREND','2026-09-22','old','DAILY_ES_20260922','2026-09-22',s,99
            FROM unnest(ARRAY['system','qt']) s;
        """)
        columns='daily_pnl daily_realized_pnl daily_unrealized_pnl daily_return portfolio_leverage equity_to_margin_ratio gross_notional margin_posted cash_available daily_transaction_costs sharpe_ratio sortino_ratio max_drawdown win_rate avg_win avg_loss profit_factor best_day worst_day downside_deviation gross_profit gross_loss total_return total_unrealized_pnl total_realized_pnl portfolio_var net_leverage margin_leverage margin_cushion max_correlation jump_risk risk_scale net_notional'.split()
        for column in columns: cur.execute(f'ALTER TABLE trading.live_results ADD {column} numeric DEFAULT 0')
        for column in ('active_positions','winning_days','losing_days','total_days'):
            cur.execute(f'ALTER TABLE trading.live_results ADD {column} integer DEFAULT 0')
    return connection

@pytest.mark.parametrize('mode,table,column',[
    ('delete_results','live_results','total_pnl'),('update_results','live_results','total_pnl'),
    ('delete_equity','equity_curve','equity'),('update_equity','equity_curve','equity'),
    ('executions','executions','quantity')])
def test_default_system_writer_preserves_same_scope_qt_sentinel(streams,mode,table,column):
    ran=subprocess.run([str(PROBE),mode],capture_output=True,text=True,env=os.environ.copy())
    assert ran.returncode==0,ran.stdout+ran.stderr
    with streams.cursor() as cur:
        day='timestamp' if table=='equity_curve' else 'date'
        cur.execute(f"SELECT {column} FROM trading.{table} WHERE portfolio_type='qt' AND {day}::date='2026-09-22'")
        assert cur.fetchall()==[(9900 if table=='equity_curve' else 99,)], 'system writer changed QT payload'
        cur.execute(f"SELECT {column} FROM trading.{table} WHERE portfolio_type='system' AND {day}::date='2026-09-22'")
        assert cur.fetchall()==([] if mode.startswith('delete') else [(2 if mode=='executions' else 4200 if table=='equity_curve' else 42,)])

def test_previous_system_aggregates_ignore_newer_qt_day(streams):
    ran=subprocess.run([str(PROBE),'previous'],capture_output=True,text=True,env=os.environ.copy())
    assert ran.returncode==0,ran.stdout+ran.stderr
    assert 'PREVIOUS_VALUE=1000\n' in ran.stdout

def run(mode):
    ran=subprocess.run([str(PROBE),mode],capture_output=True,text=True,env=os.environ.copy())
    assert ran.returncode==0,ran.stdout+ran.stderr
    return ran.stdout

@pytest.mark.parametrize('mode,table,column,want',[
    ('delete_results','live_results','total_pnl',[]),('update_results','live_results','total_pnl',[(42,)]),
    ('delete_equity','equity_curve','equity',[]),('update_equity','equity_curve','equity',[(4200,)]),
    ('complete','live_results','total_pnl',[(42,)]),('numeric','live_results','total_pnl',[(42,)]),
    ('single_equity','equity_curve','equity',[(4200,)]),('batch_equity','equity_curve','equity',[(4200,)]),
    ('executions','executions','quantity',[(2,),(3,)]),('cleanup','executions','quantity',[]),
    ('rollback','executions','quantity',[(99,)])])
def test_qt_write_changes_only_selected_full_scope(streams,mode,table,column,want):
    with streams.cursor() as cur:
        cur.execute("INSERT INTO trading.strategy_registry(id,strategy_type,portfolio_id) VALUES ('otherbook','LIVE_TREND','OTHER'),('otherengine','LIVE_OTHER','BOOK')")
        day='timestamp' if table=='equity_curve' else 'date'
        # Same order id and payload across books, engines, components and adjacent days.
        if table=='executions':
            cur.execute("""INSERT INTO trading.executions(strategy_id,portfolio_id,strategy_name,date,exec_id,order_id,execution_time,portfolio_type,quantity)
              VALUES ('LIVE_TREND','OTHER','TREND','2026-09-22','old','DAILY_ES_20260922','2026-09-22','qt',77),
                     ('LIVE_OTHER','BOOK','TREND','2026-09-22','old','DAILY_ES_20260922','2026-09-22','qt',78),
                     ('LIVE_TREND','BOOK','OTHER','2026-09-22','old','DAILY_ES_20260922','2026-09-22','qt',79),
                     ('LIVE_TREND','BOOK','TREND','2026-09-21','old','DAILY_ES_20260922','2026-09-21','qt',80)""")
        else:
            cur.execute(f"INSERT INTO trading.{table}(strategy_id,portfolio_id,{day},portfolio_type,{column}) VALUES ('LIVE_TREND','OTHER','2026-09-22','qt',77),('LIVE_OTHER','BOOK','2026-09-22','qt',78)")
        selected=f"strategy_id='LIVE_TREND' AND portfolio_id='BOOK' AND {day}::date='2026-09-22' AND portfolio_type='qt'"
        if table=='executions': selected+=" AND strategy_name='TREND'"
        cur.execute(f"SELECT to_jsonb(t)::text FROM trading.{table} t WHERE NOT ({selected}) ORDER BY to_jsonb(t)::text")
        untouched=cur.fetchall()
    run(mode+'_qt')
    with streams.cursor() as cur:
        cur.execute(f"SELECT {column} FROM trading.{table} WHERE {selected} ORDER BY {column}")
        assert cur.fetchall()==want
        cur.execute(f"SELECT to_jsonb(t)::text FROM trading.{table} t WHERE NOT ({selected}) ORDER BY to_jsonb(t)::text")
        assert cur.fetchall()==untouched

def test_qt_manager_forwards_full_scope_to_actual_writers(streams):
    run('manager_qt')
    with streams.cursor() as cur:
        cur.execute("SELECT total_pnl FROM trading.live_results WHERE date::date='2026-09-22' AND portfolio_type='qt'");assert cur.fetchall()==[(43,)]
        cur.execute("SELECT equity FROM trading.equity_curve WHERE portfolio_type='qt'");assert cur.fetchall()==[(4300,)]
        cur.execute("SELECT quantity,strategy_name FROM trading.positions WHERE portfolio_type='qt'");assert cur.fetchall()==[(2,'LIVE_TREND')]
        cur.execute("SELECT quantity,strategy_name FROM trading.executions WHERE exec_id='fresh'");assert cur.fetchall()==[(2,'LIVE_TREND')]
        cur.execute("SELECT total_pnl FROM trading.live_results WHERE date::date='2026-09-22' AND portfolio_type='system'");assert cur.fetchall()==[(99,)]

def test_fallback_never_borrows_future_or_other_stream_equity(streams):
    with streams.cursor() as cur:
        cur.execute("DELETE FROM trading.equity_curve WHERE portfolio_type='qt'")
        cur.execute("INSERT INTO trading.equity_curve VALUES ('LIVE_TREND','BOOK','2026-09-20',2000,'qt'),('LIVE_TREND','BOOK','2026-09-23',5000,'qt')")
    run('fallback_qt')
    with streams.cursor() as cur:
        cur.execute("SELECT equity FROM trading.equity_curve WHERE portfolio_type='qt' AND timestamp::date='2026-09-22'")
        assert cur.fetchall()==[(2000,)]

def test_invalid_stream_writes_fail_without_payload_changes(streams):
    with streams.cursor() as cur:
        cur.execute("SELECT to_jsonb(t)::text FROM trading.live_results t ORDER BY to_jsonb(t)::text");before=cur.fetchall()
    run('invalid')
    with streams.cursor() as cur:
        cur.execute("SELECT to_jsonb(t)::text FROM trading.live_results t ORDER BY to_jsonb(t)::text");assert cur.fetchall()==before


@pytest.fixture()
def approved_system_run(streams):
    from test_proposal_storage_migration import (
        MIGRATION as PROPOSAL_MIGRATION,
        apply,
        normalize_positions_shape,
    )
    normalize_positions_shape(streams)
    apply(streams, PROPOSAL_MIGRATION)
    apply(streams, Path(__file__).parents[2] / 'migrations/016_qt_exact_precision_and_seed_provenance.sql')
    snapshot=json.loads(run('pending_snapshot').split('PENDING_SNAPSHOT=',1)[1].splitlines()[0])
    with streams.cursor() as cur:
        cur.execute("""INSERT INTO trading.runtime_intents
          (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,config_snapshot,
           status,requested_by,request_reason,approved_by,approval_reason,approved_at)
          SELECT 'trend','BOOK','LIVE_TREND','run',runtime_revision,%s::jsonb,
                 'approved','1','synthetic request','2','synthetic approval',now()
          FROM trading.strategy_registry WHERE id='trend' RETURNING id""",(json.dumps(snapshot),))
        intent_id=cur.fetchone()[0]
    return streams,intent_id


def publication_payload(connection):
    """Observe every queued payload table, excluding the separate attempt ledger."""
    with connection.cursor() as cur:
        payload={}
        for table in ('live_results','executions','equity_curve','positions','risk_limits',
                      'live_run_metadata','run_inputs'):
            cur.execute(f'SELECT to_jsonb(t)::text FROM trading.{table} t ORDER BY to_jsonb(t)::text')
            payload[table]=cur.fetchall()
        return payload


def test_complete_controlled_system_publication_positive_control(approved_system_run):
    connection,intent_id=approved_system_run
    assert 'PENDING_PUBLISHED=1\n' in run('pending_control')
    with connection.cursor() as cur:
        for table,column,day,expected in (
            ('live_results','total_pnl','date',42),
            ('executions','quantity','date',2),
            ('equity_curve','equity','timestamp',4200)):
            cur.execute(f"SELECT portfolio_type,{column} FROM trading.{table} WHERE {day}::date='2026-09-22' ORDER BY portfolio_type")
            assert cur.fetchall()==[('qt',9900 if table=='equity_curve' else 99),('system',expected)]
        cur.execute("SELECT portfolio_type,quantity FROM trading.positions ORDER BY portfolio_type")
        assert cur.fetchall()==[('qt',12),('system',12)]
        cur.execute("""SELECT intent_id,status,outcome,publication_id=id,finished_at IS NOT NULL
                       FROM trading.runtime_attempts""")
        assert cur.fetchall()==[(intent_id,'applied','published',True,True)]


@pytest.mark.parametrize('operation',[
    'complete','numeric','executions','cleanup','single_equity','batch_equity',
    'delete_results','update_results','delete_equity','update_equity'])
def test_each_qt_writer_invalidates_otherwise_complete_controlled_publication(approved_system_run,operation):
    connection,intent_id=approved_system_run
    before=publication_payload(connection)
    assert 'PENDING_PUBLISHED=0\n' in run('pending_'+operation)
    assert publication_payload(connection)==before, 'failed run committed queued or forbidden payload'
    with connection.cursor() as cur:
        cur.execute("""SELECT intent_id,status,failure_code,publication_id,finished_at IS NOT NULL
                       FROM trading.runtime_attempts""")
        # Exactly one real controlled attempt exists, fails, and cannot be acknowledged.
        assert cur.fetchall()==[(intent_id,'failed','publication_incomplete',None,True)]

def test_model_reader_operations_ignore_qt_values_counts_and_dates(streams):
    with streams.cursor() as cur:
        cur.execute("UPDATE trading.live_results SET daily_return=1,daily_pnl=2,daily_transaction_costs=3,portfolio_leverage=4 WHERE portfolio_type='system'")
        cur.execute("UPDATE trading.live_results SET daily_return=91,daily_pnl=92,daily_transaction_costs=93,portfolio_leverage=94,current_portfolio_value=95000 WHERE portfolio_type='qt'")
    output=json.loads(run('loader').split('LOADER=',1)[1].splitlines()[0])
    assert output=={'prior':1000,'current':9900,'previous':1000,'row_pnl':2,'exists':False,'count':2,
        'returns':[1,1],'pnls':[2,2],'equities':[9900],'executions':1,'costs':3,'margin':4,
        'email_metrics':{'Daily Return':1,'Daily Unrealized PnL':0,'Daily Realized PnL':0,'Daily Total PnL':2,'Daily Transaction Costs':3}}

def test_every_model_reader_treats_qt_only_history_as_missing(streams):
    with streams.cursor() as cur:
        for table in ('live_results','equity_curve','executions'):
            cur.execute(f"DELETE FROM trading.{table} WHERE portfolio_type='system'")
    run('loader_qt_only')

def test_qt_missing_history_does_not_fall_back_to_system_or_future(streams):
    with streams.cursor() as cur:
        cur.execute("DELETE FROM trading.equity_curve WHERE portfolio_type='qt'")
        cur.execute("INSERT INTO trading.equity_curve VALUES ('LIVE_TREND','BOOK','2026-09-23',5000,'qt')")
        cur.execute("DELETE FROM trading.live_results WHERE portfolio_type='qt' AND date<'2026-09-22'")
    run('missing_history_qt')
    with streams.cursor() as cur:
        cur.execute("SELECT equity FROM trading.equity_curve WHERE portfolio_type='qt' AND timestamp::date='2026-09-22'")
        assert cur.fetchall()==[]
