"""Owned PG regressions of real staged DB methods, not alternative query math.

Pending compiled probe installation and an explicit parent-owned heavy boundary.
The connection fixture is the existing disposable schema/migration fixture.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import psycopg2
import pytest

WORKSPACE=next(p for p in Path(__file__).resolve().parents if (p/'.review/trade-ngin-qt').is_dir())
sys.path.insert(0,str(WORKSPACE/'.review/trade-ngin-qt/tests/integration'))
from test_runtime_control_schema import connection

PROBE=Path('/home/devcontainers/qt-validation-20260921/bin/Debug/equity_model_database_probe')

@pytest.fixture()
def ledger(connection):
    with connection.cursor() as cur:
        # Explicit removal isolates the typed writer's own fence; the trigger
        # otherwise masks a missing pre-write lock/admission check.
        cur.execute('DROP TRIGGER runtime_publication_fence ON trading.positions')
        cur.execute('ALTER TABLE trading.executions ADD strategy_name text, ADD symbol text, ADD date date, ADD side text')
        cur.execute("""INSERT INTO trading.positions
            (strategy_id,strategy_name,portfolio_id,portfolio_type,symbol,date,quantity,average_price)
            VALUES
            ('LIVE_TREND','MODEL','BOOK','system','SYN','2026-09-20',3,10),
            ('LIVE_TREND','MODEL','BOOK','system','SYN','2026-09-21',0,10),
            ('LIVE_TREND','MODEL','BOOK','system','SYN','2026-09-22',3,10),
            ('LIVE_TREND','MODEL','BOOK','qt','SYN','2026-01-01',99,10),
            ('LIVE_TREND','MODEL','BOOK','qt','SYN','2026-09-24',0,10),
            ('LIVE_TREND','MODEL','BOOK','system','SYN','2026-09-26',0,10),
            ('LIVE_TREND','MODEL','BOOK','system','FUT','2026-09-26',3,10),
            ('LIVE_TREND','OTHER','BOOK','system','SYN','2026-01-01',88,10),
            ('LIVE_TREND','MODEL','OTHER','system','SYN','2026-01-01',77,10)
        """)
        cur.execute("""INSERT INTO trading.executions
            (strategy_id,strategy_name,portfolio_id,portfolio_type,symbol,date,side)
            VALUES
            ('LIVE_TREND','MODEL','BOOK','system','SYN','2026-09-22','BUY'),
            ('LIVE_TREND','MODEL','BOOK','qt','SYN','2026-09-24','BUY'),
            ('LIVE_TREND','OTHER','BOOK','system','SYN','2026-09-24','BUY'),
            ('LIVE_TREND','MODEL','BOOK','system','SYN','2026-09-26','BUY')
        """)
    return connection

def invoke(mode):
    return subprocess.run([str(PROBE),mode],env=os.environ.copy(),text=True,
        capture_output=True,timeout=20)

def state(connection):
    with connection.cursor() as cur:
        cur.execute('SELECT to_jsonb(p)::text FROM trading.positions p ORDER BY to_jsonb(p)::text')
        positions=cur.fetchall()
        cur.execute('SELECT to_jsonb(e)::text FROM trading.executions e ORDER BY to_jsonb(e)::text')
        return positions,cur.fetchall()

def test_model_history_ignores_same_owner_qt_other_owner_and_future_rows(ledger):
    before=state(ledger)
    result=invoke('--history');assert result.returncode==0,result.stdout+result.stderr
    assert json.loads(result.stdout.splitlines()[-1])=={
        'inception':{'SYN':'2026-09-20'},'holding':{'SYN':'2026-09-22'},'buy':{'SYN':'2026-09-22'}}
    assert state(ledger)==before

@pytest.mark.parametrize('mode',['--bad-table','--bad-date'])
def test_history_refuses_invalid_identifier_or_date_without_mutation(ledger,mode):
    before=state(ledger)
    result=invoke(mode);assert result.returncode==0,result.stdout+result.stderr
    assert json.loads(result.stdout.splitlines()[-1])=={
        'inception_refused':True,'holding_refused':True,'buy_refused':True}
    assert state(ledger)==before

def test_borrowed_uow_checks_runtime_authority_without_row_trigger(ledger):
    with ledger.cursor() as cur:cur.execute("UPDATE trading.strategy_registry SET lifecycle='retired'")
    before=state(ledger);result=invoke('--store')
    assert result.returncode==10,result.stdout+result.stderr
    assert 'REFUSED' in result.stdout
    assert state(ledger)==before

def test_borrowed_uow_joins_confirmation_book_lock_before_mutating(ledger):
    before=state(ledger)
    blocker=psycopg2.connect(os.environ['ALGOLENS_TEST_DB'])
    contender=psycopg2.connect(os.environ['ALGOLENS_TEST_DB'])
    child=None
    try:
        with blocker.cursor() as cur:
            cur.execute("SELECT pg_advisory_xact_lock(hashtextextended('algolens:qt-book:BOOK',0))")
        child=subprocess.Popen([str(PROBE),'--store-and-wait'],env=os.environ.copy(),
            stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        deadline=time.monotonic()+5;blocked=False
        while time.monotonic()<deadline:
            with ledger.cursor() as cur:
                cur.execute("SELECT count(*) FROM pg_stat_activity WHERE datname=current_database() "
                    "AND wait_event_type='Lock' AND query LIKE 'SELECT * FROM trading.lock_runtime_scope%'")
                blocked=cur.fetchone()[0]==1
            if blocked:break
            if child.poll() is not None:break
            time.sleep(.02)
        assert blocked,'writer failed to acquire the explicit pre-write runtime/book fence'
        assert state(ledger)==before
        blocker.rollback()
        # A contender cannot acquire the confirmation lock while the writer's
        # borrowed UOW waits for the caller's one explicit commit.
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            with ledger.cursor() as cur:
                cur.execute("SELECT count(*) FROM pg_stat_activity WHERE datname=current_database() "
                    "AND state='idle in transaction' AND query LIKE 'INSERT INTO trading.positions %'")
                if cur.fetchone()[0]==1:break
            time.sleep(.02)
        else:raise AssertionError('real position writer did not reach its uncommitted state')
        with contender.cursor() as cur:
            cur.execute("SELECT pg_try_advisory_xact_lock(hashtextextended('algolens:qt-book:BOOK',0))")
            assert cur.fetchone()[0] is False
        assert state(ledger)==before
        out,err=child.communicate('commit\n',timeout=5)
        assert child.returncode==0,out+err
        assert 'UOW_WRITTEN' in out and '"committed":true' in out
        with ledger.cursor() as cur:
            cur.execute("SELECT quantity FROM trading.positions WHERE strategy_id='LIVE_TREND' "
                "AND strategy_name='MODEL' AND portfolio_id='BOOK' AND portfolio_type='system' "
                "AND symbol='SYN' AND date='2026-09-25'")
            assert cur.fetchone()[0]==7
        with contender.cursor() as cur:
            cur.execute("SELECT pg_try_advisory_xact_lock(hashtextextended('algolens:qt-book:BOOK',0))")
            assert cur.fetchone()[0] is True
    finally:
        if child is not None and child.poll() is None:
            child.kill();child.communicate(timeout=5)
        blocker.rollback();blocker.close();contender.rollback();contender.close()

# ---- N5: an equity MODEL-only publication for an inc_meanrev-shaped registry row -------------------
# The actual native publisher (equity_prior_binding_probe: PublicationPriorRequirement::None, the
# system-reference prior, RequiredFinalObservations) against LIVE_EQUITY_MEAN_REVERSION/EQUITY_MR_PORTFOLIO.
# The database layer is 013 + 015 + 016 + 025 (this tree's migrations). The MODEL payload is synthetic.
BINDING_PROBE=Path('/home/devcontainers/qt-validation-20260921/bin/Debug/equity_prior_binding_probe')
MODEL_INCUBATING=Path(__file__).resolve().parents[1]/'migrations/025_runtime_scope_model_incubating.sql'
EQ_BOOK,EQ_PRIOR_DAY,EQ_DAY='EQUITY_MR_PORTFOLIO','2026-09-21','2026-09-22'
EQ_UNUSED_DECISION,EQ_UNUSED_FINALIZATION='40000000-0000-4000-8000-000000000001','40000000-0000-4000-8000-000000000002'

def eq_probe(mode):
    # A missing binary raises FileNotFoundError here: a setup failure, never a refusal.
    return subprocess.run([str(BINDING_PROBE),mode,EQ_UNUSED_DECISION,EQ_UNUSED_FINALIZATION,EQ_BOOK,EQ_PRIOR_DAY,EQ_DAY],
        env=os.environ.copy(),text=True,capture_output=True,timeout=60)

def inc_meanrev(connection,lifecycle,active=True):
    from test_runtime_control_schema import prepare_exact_publication_schema
    prepare_exact_publication_schema(connection)
    with connection.cursor() as cur:
        cur.execute(MODEL_INCUBATING.read_text())
        # Production's inc_meanrev: incubating since 2025-07-15, never changed after 013 (revision 0).
        cur.execute("INSERT INTO trading.strategy_registry (id,strategy_type,portfolio_id,lifecycle,is_active,"
            "mock_capital) VALUES ('inc_meanrev','LIVE_EQUITY_MEAN_REVERSION',%s,%s,%s,100000)",(EQ_BOOK,lifecycle,active))
        cur.execute("SELECT lifecycle,is_active,runtime_revision FROM trading.strategy_registry WHERE id='inc_meanrev'")
        assert cur.fetchone()==(lifecycle,active,0)

def eq_model_rows(connection):
    with connection.cursor() as cur:
        cur.execute("SELECT portfolio_type,symbol,quantity::text FROM trading.positions WHERE portfolio_id=%s AND "
            "strategy_id='LIVE_EQUITY_MEAN_REVERSION' AND strategy_name='EQUITY_MEAN_REVERSION' AND date=%s "
            "ORDER BY portfolio_type",(EQ_BOOK,EQ_DAY))
        positions=[(stream,symbol,float(quantity)) for stream,symbol,quantity in cur.fetchall()]
        cur.execute("SELECT count(*) FROM trading.qt_model_seed_publications WHERE portfolio_id=%s AND "
            "strategy_id='LIVE_EQUITY_MEAN_REVERSION' AND source_day=%s",(EQ_BOOK,EQ_DAY))
        seeds=cur.fetchone()[0]
        cur.execute("SELECT count(*) FROM trading.live_results WHERE portfolio_id=%s",(EQ_BOOK,))
        results=cur.fetchone()[0]
        cur.execute("SELECT status,outcome FROM trading.runtime_attempts")
        return positions,seeds,results,cur.fetchall()

def approve_model_run(connection):
    snapshot=eq_probe('snapshot');assert snapshot.returncode==0,snapshot.stdout+snapshot.stderr
    rows=[line for line in snapshot.stdout.splitlines() if line.startswith('{')];assert len(rows)==1
    with connection.cursor() as cur:
        cur.execute("INSERT INTO trading.runtime_intents(registry_id,portfolio_id,engine_strategy_id,action,"
            "registry_revision,config_snapshot,status,requested_by,request_reason,approved_by,approval_reason,approved_at) "
            "SELECT id,%s,'LIVE_EQUITY_MEAN_REVERSION','run',runtime_revision,%s::jsonb,'approved','1',"
            "'synthetic model run','2','synthetic approval',now() FROM trading.strategy_registry WHERE id='inc_meanrev'",
            (EQ_BOOK,rows[0]))

@pytest.mark.parametrize('lifecycle,controlled',[('incubating',False),('incubating',True),('live',False)])
def test_equity_model_only_publication_admits_inc_meanrev_shaped_scope(connection,lifecycle,controlled):
    """N5 (c): incubating + active + revision 0 publishes the MODEL (system) stream; 'live' is the control."""
    inc_meanrev(connection,lifecycle)
    if controlled:
        approve_model_run(connection)
    result=eq_probe('system_reference_publish' if controlled else 'model_only_uncontrolled')
    assert result.returncode==0,result.stdout+result.stderr+' exit=%d'%result.returncode
    assert 'EQ_PUBLICATION_COMMITTED=1' in result.stdout
    positions,seeds,results,attempts=eq_model_rows(connection)
    assert positions==[('qt','SYN',12.0),('system','SYN',12.0)]
    assert seeds==1 and results==1
    assert attempts==([('applied','published')] if controlled else [])
    with connection.cursor() as cur:
        cur.execute("SELECT lifecycle,is_active,runtime_revision FROM trading.strategy_registry WHERE id='inc_meanrev'")
        assert cur.fetchone()==(lifecycle,True,0)

@pytest.mark.parametrize('lifecycle,active',[('incubating',False),('retired',True)])
def test_equity_model_only_publication_still_refuses_inactive_or_retired_scope(connection,lifecycle,active):
    inc_meanrev(connection,lifecycle,active)
    before=eq_model_rows(connection)
    result=eq_probe('model_only_uncontrolled')
    assert result.returncode==69,result.stdout+result.stderr+' exit=%d'%result.returncode  # admission refused
    assert 'EQ_PUBLICATION_COMMITTED=1' not in result.stdout
    assert eq_model_rows(connection)==before==([],0,0,[])
