"""Confirmed native futures accounting against the owned, route-less PG fixture.

All prices/cost inputs/finalization anchors here are explicitly synthetic.
"""
from copy import deepcopy
from datetime import timedelta
from decimal import Decimal
from hashlib import sha256
import os
import subprocess

import pytest
from psycopg2.extras import Json
from test_qt_desk_storage import (desk, DECISION, ATTEMPT, BINARY, ROOT, BOOK,
                                 table_state, canonical_qt_input_bytes)
from test_runtime_control_schema import connection

INPUT="90000000-0000-4000-8000-000000000001"
pytestmark=pytest.mark.parametrize("desk",["futures"],indirect=True)

def digest(value):
    return sha256(canonical_qt_input_bytes(value)).hexdigest()

def run():
    return subprocess.run([str(BINARY),"--accounting",DECISION,ATTEMPT,INPUT],
        capture_output=True,text=True,timeout=30,env=os.environ.copy())

def all_state(conn):
    with conn.cursor() as cur:
        rows=[]
        for table in ("positions","executions","live_results","equity_curve","qt_execution_observations",
                      "qt_desk_receipts","qt_desk_results","position_overrides","desk_run_results"):
            cur.execute("SELECT to_jsonb(t)::text FROM trading."+table+" t ORDER BY to_jsonb(t)::text")
            rows.append(cur.fetchall())
        return rows

@pytest.fixture()
def accounting(desk,request):
    conn,observed=desk
    with conn.cursor() as cur:
        cur.execute((ROOT/"migrations/017_desk_accounting_inputs.sql").read_text())
        cur.execute("ALTER TABLE trading.executions ADD strategy_name text,ADD date date,ADD symbol text,"
            "ADD exec_id text,ADD order_id text,ADD side text,ADD quantity numeric,ADD price numeric,"
            "ADD execution_time timestamptz,ADD commissions_fees numeric,ADD implicit_price_impact numeric,"
            "ADD slippage_market_impact numeric,ADD total_transaction_costs numeric,ADD is_partial boolean;"
            "ALTER TABLE trading.live_results ADD daily_pnl numeric,ADD daily_realized_pnl numeric,"
            "ADD daily_unrealized_pnl numeric,ADD daily_transaction_costs numeric")
        cur.execute("SELECT source_day,clock_timestamp() FROM trading.qt_decisions WHERE decision_id=%s",(DECISION,))
        day,now=cur.fetchone();prior=day-timedelta(days=1)
        cur.execute("INSERT INTO trading.executions(portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,exec_id,order_id,side,quantity,price,execution_time,commissions_fees,implicit_price_impact,slippage_market_impact,total_transaction_costs,is_partial) "
            "VALUES('BOOK','LIVE_TREND','synthetic-alpha',%s,'SYN','system','system-sentinel','system-sentinel','BUY',9,99,%s,1,1,1,2,false)",(day,now))
        cur.execute("INSERT INTO trading.live_results(portfolio_id,strategy_id,date,portfolio_type,total_pnl,current_portfolio_value) "
            "VALUES('BOOK','LIVE_TREND',%s,'system',99,9900)",(day,))
        cur.execute("INSERT INTO trading.equity_curve(portfolio_id,strategy_id,timestamp,portfolio_type,equity) "
            "VALUES('BOOK','LIVE_TREND',%s,'system',9900)",(day,))
        mode=getattr(request,"param","")
        cur.execute("INSERT INTO trading.positions(portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,"
            "quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update) "
            "SELECT portfolio_id,strategy_id,strategy_name,%s,symbol,'qt',quantity,average_price,0,0,%s "
            "FROM trading.positions WHERE portfolio_type='qt' AND date=%s",(prior,now,day))
        if mode=="quiet":
            cur.execute("UPDATE trading.positions SET quantity=CASE strategy_name WHEN 'synthetic-alpha' THEN 5 ELSE 1 END "
                        "WHERE portfolio_type='qt' AND date=%s",(prior,))
        cur.execute("INSERT INTO trading.live_results(portfolio_id,strategy_id,date,portfolio_type,total_pnl,current_portfolio_value) "
            "VALUES('BOOK','LIVE_TREND',%s,'qt',20,1000)",(prior,))
        cur.execute("SELECT strategy_name,symbol,quantity::text,average_price::text FROM trading.positions "
            "WHERE portfolio_type='qt' AND date=%s ORDER BY strategy_name,symbol",(prior,))
        rows=cur.fetchall()
        def exact(v):return format(Decimal(v).normalize(),'f')
        previous=[dict(key=dict(portfolio_id=BOOK,strategy_id="LIVE_TREND",strategy_name=owner,date=str(prior),
            symbol=symbol,portfolio_type="qt"),quantity_exact=exact(q),average_price_exact=exact(p)) for owner,symbol,q,p in rows]
        totals=[dict(strategy_id="LIVE_TREND",equity_exact="1000",total_pnl_exact="20")]
        final=dict(schema_version="qt-finalized-accounting/v1",book_id=BOOK,source_day=str(prior),previous_totals=totals)
        cur.execute("INSERT INTO trading.qt_desk_finalization_sources(source_id,book_id,source_day,producer_id,policy_version,"
            "source_version,content_digest,payload) VALUES('synthetic-final',%s,%s,'synthetic-execution','execution-policy-v1','final-v1',%s,%s)",
            (BOOK,prior,digest(final),Json(final)))
        instrument=dict(symbol=rows[0][1],price_exact="101",adv_exact="100000",volatility_multiplier_exact="1",source_id="synthetic-prior-close",
            baseline_spread_ticks="1",min_spread_ticks="1",max_spread_ticks="10",spread_cost_multiplier="0.5",
            max_impact_bps="100",tick_size="0.01",point_value="5",max_total_implicit_bps="200")
        payload=dict(schema_version="qt-futures-accounting-input/v1",decision_id=DECISION,book_id=BOOK,source_day=str(day),
            previous_day=str(prior),accounting_source_id="synthetic-accounting",prior_finalization_source_id="synthetic-final",
            timestamp=str(day)+"T00:00:00Z",currency="USD",previous_positions=previous,previous_totals=totals,
            cost_config=dict(explicit_fee_per_contract="1.5",min_adv="100",min_participation="0",max_participation="0.1"),instruments=[instrument])
        if mode=="missing_price":payload["instruments"][0].pop("price_exact")
        if mode=="missing_finalization":payload["prior_finalization_source_id"]="absent"
        if mode=="wrong_previous":payload["previous_positions"][0]["quantity_exact"]="999"
        if mode=="wrong_equity":payload["previous_totals"][0]["equity_exact"]="9000"
        if mode!="no_input":
            cur.execute("INSERT INTO trading.qt_desk_accounting_inputs(input_id,decision_id,producer_id,policy_version,source_version,"
                "as_of,valid_until,content_digest,payload) VALUES(%s,%s,'synthetic-execution','execution-policy-v1','accounting-v1',%s,%s,%s,%s)",
                (INPUT,DECISION,now-timedelta(seconds=1),now+timedelta(hours=1),digest(payload),Json(payload)))
    return conn,payload

def test_real_producer_writes_all_financial_outputs_and_replays(accounting):
    conn,_=accounting
    before=all_state(conn)
    def system_rows():
        with conn.cursor() as cur:
            rows=[]
            for table in ('positions','executions','live_results','equity_curve'):
                cur.execute("SELECT to_jsonb(t)::text FROM trading."+table+" t WHERE portfolio_type='system' ORDER BY to_jsonb(t)::text")
                rows.append(cur.fetchall())
            return rows
    original_system=system_rows()
    r=run();assert r.returncode==0,r.stdout+r.stderr
    after=all_state(conn);assert before!=after
    assert system_rows()==original_system
    with conn.cursor() as cur:
        cur.execute("SELECT strategy_name,side,quantity,price,total_transaction_costs FROM trading.executions WHERE portfolio_type='qt' ORDER BY strategy_name")
        executions=cur.fetchall();assert len(executions)==2
        assert [(r[1],r[2],r[3]) for r in executions]==[("BUY",Decimal(1),Decimal(101)),("SELL",Decimal(1),Decimal(101))]
        cost=sum(r[4] for r in executions)
        cur.execute("SELECT daily_pnl,current_portfolio_value FROM trading.live_results WHERE portfolio_type='qt' ORDER BY date DESC LIMIT 1")
        assert cur.fetchone()==(-cost,Decimal(1000)-cost)
        cur.execute("SELECT equity FROM trading.equity_curve WHERE portfolio_type='qt'")
        assert cur.fetchall()==[(Decimal(1000)-cost,)]
        cur.execute("SELECT count(*) FROM trading.desk_run_results");assert cur.fetchone()==(1,)
    r=run();assert r.returncode==0 and "REPLAYED=1" in r.stdout,r.stdout+r.stderr
    assert all_state(conn)==after

@pytest.mark.parametrize("accounting",["missing_price","missing_finalization","wrong_previous","wrong_equity"],indirect=True)
def test_unavailable_accounting_rolls_back_every_output(accounting):
    conn,_=accounting;before=all_state(conn);r=run()
    assert r.returncode!=0
    assert all_state(conn)==before

def test_replay_requires_linked_finalization_evidence(accounting):
    conn,_=accounting;r=run();assert r.returncode==0,r.stdout+r.stderr
    with conn.cursor() as cur:
        cur.execute("ALTER TABLE trading.qt_desk_finalization_sources RENAME TO unavailable_finalization")
    before=all_state(conn);r=run();assert r.returncode!=0
    assert all_state(conn)==before

def test_stored_diagnostics_keep_model_choice_execution_and_controls_distinct(accounting):
    conn,_=accounting;r=run();assert r.returncode==0,r.stdout+r.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT payload FROM trading.desk_run_results WHERE decision_id=%s",(DECISION,))
        output=cur.fetchone()[0]
    diagnostic=output['desk_diagnostics']
    assert diagnostic['schema_version']=='qt-desk-diagnostics/v1'
    assert diagnostic['controls_applied']==output['layers_applied']==[]
    assert len(diagnostic['model_to_choice'])==2
    assert {row['key']['strategy_name'] for row in diagnostic['model_to_choice']}=={'synthetic-alpha','synthetic-beta'}
    assert all(row['price_source_id']=='synthetic-prior-close' for row in diagnostic['model_to_choice'])
    assert diagnostic['risk']['metrics']==output['evaluation_diagnostics']['selected_risk']['metrics']
    assert diagnostic['optimizer']['status']==output['evaluation_diagnostics']['optimizer']['status']
    assert diagnostic['per_owner_solver_recommendation']['status']=='unavailable'
    assert diagnostic['execution_distance']==output['distance']

def test_failure_after_positions_and_executions_rolls_back_everything(accounting):
    conn,_=accounting
    with conn.cursor() as cur:
        cur.execute("CREATE FUNCTION trading.refuse_equity() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'synthetic failure'; END $$;"
            "CREATE TRIGGER refuse_equity BEFORE INSERT ON trading.equity_curve FOR EACH ROW EXECUTE FUNCTION trading.refuse_equity()")
    before=all_state(conn);r=run();assert r.returncode!=0
    assert all_state(conn)==before

@pytest.mark.parametrize("column",["total_pnl","current_portfolio_value"])
def test_overprecision_anchor_is_never_silently_rounded(accounting,column):
    conn,payload=accounting
    with conn.cursor() as cur:
        cur.execute("UPDATE trading.live_results SET "+column+"="+column+"+0.000000001 WHERE portfolio_type='qt' AND date=%s",(payload["previous_day"],))
    before=all_state(conn);r=run();assert r.returncode!=0
    assert all_state(conn)==before

def test_consumer_receipt_cannot_masquerade_as_accounting_replay(accounting):
    from test_qt_desk_storage import run as consumer_run,OBSERVATION
    conn,_=accounting;r=consumer_run();assert r.returncode==0,r.stdout+r.stderr
    before=all_state(conn)
    r=subprocess.run([str(BINARY),"--accounting",DECISION,ATTEMPT,OBSERVATION],
        capture_output=True,text=True,timeout=30,env=os.environ.copy())
    assert r.returncode!=0
    assert all_state(conn)==before

@pytest.mark.parametrize("table,column",[("executions","price"),("live_results","current_portfolio_value"),("equity_curve","equity")])
def test_accounting_replay_checks_real_financial_rows(accounting,table,column):
    conn,payload=accounting;r=run();assert r.returncode==0,r.stdout+r.stderr
    date_column="timestamp" if table=="equity_curve" else "date"
    with conn.cursor() as cur:
        cur.execute("UPDATE trading."+table+" SET "+column+"="+column+"+1 WHERE portfolio_type='qt' AND "+date_column+"=%s",(payload["source_day"],))
    before=all_state(conn);r=run();assert r.returncode!=0
    assert all_state(conn)==before

@pytest.mark.parametrize("table",["qt_desk_accounting_inputs","qt_desk_finalization_sources"])
def test_input_evidence_is_immutable(accounting,table):
    conn,_=accounting
    with conn.cursor() as cur:
        for sql in ("UPDATE trading."+table+" SET source_version='changed'","DELETE FROM trading."+table,"TRUNCATE trading."+table+" CASCADE"):
            with pytest.raises(Exception,match="immutable"):cur.execute(sql)

@pytest.mark.parametrize("accounting",["quiet"],indirect=True)
def test_distinct_prior_book_quiet_day_has_no_fake_execution(accounting):
    conn,payload=accounting;r=run();assert r.returncode==0,r.stdout+r.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT count(*) FROM trading.executions WHERE portfolio_type='qt'");assert cur.fetchone()==(0,)
        cur.execute("SELECT current_portfolio_value FROM trading.live_results WHERE portfolio_type='qt' AND date=%s",(payload['source_day'],))
        assert cur.fetchone()==(Decimal(1000),)
    before=all_state(conn);r=run();assert r.returncode==0,r.stdout+r.stderr
    assert all_state(conn)==before

def _metadata_closure_state(conn):
    state = all_state(conn)
    with conn.cursor() as cur:
        for table in ('qt_desk_accounting_inputs', 'qt_desk_finalization_sources'):
            cur.execute('SELECT to_jsonb(t)::text FROM trading.' + table + ' t ORDER BY to_jsonb(t)::text')
            state.append(cur.fetchall())
    return state


@pytest.mark.parametrize('path', ['replay', 'current_report'])
@pytest.mark.parametrize('column', ['as_of', 'valid_until', 'source_version'])
def test_current_copied_metadata_contradiction_refuses_without_writes(accounting, path, column):
    from test_qt_desk_storage import report_proof
    conn, _ = accounting
    first = run()
    assert first.returncode == 0, first.stdout + first.stderr
    current = report_proof(conn)
    assert current.returncode == 0, current.stdout + current.stderr
    # Only the owned fixture administrator bypasses the immutable-row trigger.
    # Keep both leases valid and the receipt within both windows, isolating the
    # missing copied-column equality from independent freshness checks.
    with conn.cursor() as cur:
        cur.execute('ALTER TABLE trading.qt_desk_accounting_inputs DISABLE TRIGGER USER')
        if column == 'source_version':
            cur.execute("UPDATE trading.qt_desk_accounting_inputs SET source_version='different-current-version' WHERE input_id=%s", (INPUT,))
        elif column == 'as_of':
            cur.execute("UPDATE trading.qt_desk_accounting_inputs SET as_of=as_of-interval '1 second' WHERE input_id=%s", (INPUT,))
        else:
            assert column == 'valid_until'
            cur.execute("UPDATE trading.qt_desk_accounting_inputs SET valid_until=valid_until+interval '1 second' WHERE input_id=%s", (INPUT,))
        cur.execute('ALTER TABLE trading.qt_desk_accounting_inputs ENABLE TRIGGER USER')
        cur.execute('''SELECT i.as_of<=clock_timestamp() AND clock_timestamp()<=i.valid_until
            AND o.as_of<=clock_timestamp() AND clock_timestamp()<=o.valid_until
            AND i.as_of<=r.processed_at AND r.processed_at<=i.valid_until
            AND o.as_of<=r.processed_at AND r.processed_at<=o.valid_until
            FROM trading.qt_desk_accounting_inputs i
            JOIN trading.qt_execution_observations o ON o.observation_id=i.input_id
            JOIN trading.qt_desk_receipts r ON r.decision_id=i.decision_id WHERE i.input_id=%s''', (INPUT,))
        assert cur.fetchone() == (True,)
    before = _metadata_closure_state(conn)
    result = run() if path == 'replay' else report_proof(conn)
    assert result.returncode != 0, result.stdout + result.stderr
    assert _metadata_closure_state(conn) == before


@pytest.mark.parametrize('column', ['as_of', 'valid_until', 'source_version'])
def test_current_copied_metadata_insert_contradiction_rolls_back_atomic_processing(accounting, column):
    conn, _ = accounting
    assignment = {
        'as_of': "NEW.as_of=NEW.as_of-interval '1 second';",
        'valid_until': "NEW.valid_until=NEW.valid_until+interval '1 second';",
        'source_version': "NEW.source_version='contradictory-insert-version';",
    }[column]
    with conn.cursor() as cur:
        cur.execute('''CREATE FUNCTION trading.alter_test_observation_metadata() RETURNS trigger
            LANGUAGE plpgsql AS $$ BEGIN ''' + assignment + ''' RETURN NEW; END $$;
            CREATE TRIGGER alter_test_observation_metadata BEFORE INSERT ON trading.qt_execution_observations
            FOR EACH ROW EXECUTE FUNCTION trading.alter_test_observation_metadata()''')
    before = _metadata_closure_state(conn)
    result = run()
    assert result.returncode != 0, result.stdout + result.stderr
    assert _metadata_closure_state(conn) == before


@pytest.mark.parametrize('path', ['replay', 'current_report'])
@pytest.mark.parametrize('session_timezone', ['UTC', 'America/New_York'])
@pytest.mark.parametrize('live_date_type', ['date', 'timestamptz'])
def test_current_copied_metadata_equal_instants_accept_offset_spelling(accounting, path, session_timezone, live_date_type, monkeypatch):
    from datetime import timezone
    from test_qt_desk_storage import report_proof
    conn, _ = accounting
    if live_date_type == 'date':
        with conn.cursor() as cur:
            cur.execute("ALTER TABLE trading.live_results ALTER COLUMN date TYPE date USING (date AT TIME ZONE 'UTC')::date")
    first = run()
    assert first.returncode == 0, first.stdout + first.stderr
    with conn.cursor() as cur:
        cur.execute('SELECT as_of,valid_until FROM trading.qt_desk_accounting_inputs WHERE input_id=%s', (INPUT,))
        start, end = cur.fetchone()
        # Supply different UTC-offset representations of identical instants.
        start_text = start.astimezone(timezone(timedelta(hours=-4))).isoformat()
        end_text = end.astimezone(timezone(timedelta(hours=5, minutes=30))).isoformat()
        cur.execute('ALTER TABLE trading.qt_desk_accounting_inputs DISABLE TRIGGER USER')
        cur.execute('UPDATE trading.qt_desk_accounting_inputs SET as_of=%s::timestamptz,valid_until=%s::timestamptz WHERE input_id=%s',
                    (start_text, end_text, INPUT))
        cur.execute('ALTER TABLE trading.qt_desk_accounting_inputs ENABLE TRIGGER USER')
        cur.execute('''SELECT i.as_of=o.as_of AND i.valid_until=o.valid_until
            FROM trading.qt_desk_accounting_inputs i JOIN trading.qt_execution_observations o
            ON o.observation_id=i.input_id WHERE i.input_id=%s''', (INPUT,))
        assert cur.fetchone() == (True,)
    before = _metadata_closure_state(conn)
    monkeypatch.setenv('PGOPTIONS', '-c timezone=' + session_timezone)
    result = run() if path == 'replay' else report_proof(conn)
    assert result.returncode == 0, result.stdout + result.stderr
    if path == 'replay':
        assert 'REPLAYED=1' in result.stdout
    assert _metadata_closure_state(conn) == before


@pytest.mark.parametrize('live_date_type', ['date', 'timestamptz'])
def test_current_copied_metadata_fresh_ny_session_preserves_utc_financial_day(accounting, live_date_type, monkeypatch):
    from test_qt_desk_storage import report_proof
    conn, payload = accounting
    if live_date_type == 'date':
        with conn.cursor() as cur:
            cur.execute("ALTER TABLE trading.live_results ALTER COLUMN date TYPE date USING (date AT TIME ZONE 'UTC')::date")
    monkeypatch.setenv('PGOPTIONS', '-c timezone=America/New_York')
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT date=%s::timestamptz FROM trading.live_results WHERE portfolio_type='qt' AND (date AT TIME ZONE 'UTC')::date=%s::date", (payload['source_day']+'T00:00:00Z', payload['source_day']))
        assert cur.fetchall() == [(True,)]
    before = _metadata_closure_state(conn)
    result = report_proof(conn)
    assert result.returncode == 0, result.stdout + result.stderr
    result = run()
    assert result.returncode == 0 and 'REPLAYED=1' in result.stdout, result.stdout + result.stderr
    assert _metadata_closure_state(conn) == before