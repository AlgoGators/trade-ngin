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
