"""Actual publisher transaction with native synthetic observations, never MODEL.

Future parent-reviewed owned PG gate only. No SQL success flags or sentinels.
"""
from copy import deepcopy
from decimal import Decimal
from pathlib import Path
import ctypes,json,os,subprocess
import pytest
from psycopg2.extras import Json
from test_qt_empty_owner_schema import empty_schema,predecessor,connection
ROOT=Path(__file__).parents[2]
PROBE=Path('/home/devcontainers/qt-validation-20260921/bin/Debug/qt_empty_owner_publication_probe')
GUARD=PROBE.parent/'libqt_no_delivery_guard.so'
ENGINE='LIVE_EQUITY_MEAN_REVERSION';OWNER='EQUITY_MEAN_REVERSION';BOOK='EQ_BOOK';DAY='2026-09-26'

def invoke(mode):
    assert PROBE.is_file(),'missing reviewed binary is setup failure, never behavioral RED'
    assert GUARD.is_file() and not GUARD.is_symlink()
    assert ctypes.CDLL(str(GUARD)).qt_no_delivery_guard_loaded()==1
    env=os.environ.copy();env.update(LD_PRELOAD=str(GUARD),QT_EMAIL_DELIVERY_ENABLED="false")
    result=subprocess.run([str(PROBE),mode,DAY],capture_output=True,text=True,timeout=30,env=env)
    assert result.returncode==0,(result.returncode,result.stdout[-2000:],result.stderr[-500:])
    return result

@pytest.fixture()
def publisher(empty_schema,request,monkeypatch):
    conn=empty_schema
    with conn.cursor() as cur:
        cur.execute("SELECT (pg_catalog.clock_timestamp() AT TIME ZONE 'UTC')::date::text")
        monkeypatch.setitem(globals(),"DAY",cur.fetchone()[0])
    mode="snapshot_second" if request.node.name=="test_real_single_owner_trace_does_not_certify_second_configured_empty_owner" else "snapshot"
    config=json.loads(invoke(mode).stdout.split('EMPTY_SNAPSHOT=',1)[1])
    with conn.cursor() as cur:
        cur.execute("INSERT INTO trading.strategy_registry(id,strategy_type,portfolio_id) VALUES('eq-owner',%s,%s)",(ENGINE,BOOK))
        cur.execute('INSERT INTO trading.strategy_book_memberships(strategy_id,portfolio_id) VALUES(%s,%s)',('eq-owner',BOOK))
        cur.execute("INSERT INTO trading.runtime_intents(registry_id,portfolio_id,engine_strategy_id,action,registry_revision,config_snapshot,status,requested_by,request_reason,approved_by,approval_reason,approved_at) SELECT 'eq-owner',%s,%s,'run',runtime_revision,%s,'approved','1','synthetic native publisher','2','owned protocol test',now() FROM trading.strategy_registry WHERE id='eq-owner'",(BOOK,ENGINE,Json(config)))
    return conn,config

def snapshot(conn):
    with conn.cursor() as cur:
        result={}
        for table in ('positions','live_results','equity_curve','live_run_metadata','run_inputs','risk_limits','qt_model_seed_publications','qt_empty_model_owner_publications'):
            cur.execute('SELECT to_jsonb(t)::text FROM trading.'+table+' t ORDER BY to_jsonb(t)::text')
            result[table]=deepcopy(cur.fetchall())
        return result

def seed(conn,engine,owner,stream,book=BOOK):
    with conn.cursor() as cur:
        if engine!=ENGINE:
            cur.execute('INSERT INTO trading.strategy_registry(id,strategy_type,portfolio_id) VALUES(%s,%s,%s) ON CONFLICT DO NOTHING',(engine,engine,book))
            cur.execute('INSERT INTO trading.strategy_book_memberships(strategy_id,portfolio_id) VALUES(%s,%s) ON CONFLICT DO NOTHING',(engine,book))
        cur.execute("INSERT INTO trading.positions(portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,quantity,average_price,daily_realized_pnl,daily_unrealized_pnl,last_update) VALUES(%s,%s,%s,%s,'SYN',%s,-0.5,100,0,0,%s::date)",(book,engine,owner,DAY,stream,DAY))

@pytest.mark.parametrize('mode',['controlled','uncontrolled'])
def test_actual_empty_publication_seals_all_archived_operands_without_sentinel(publisher,mode):
    conn,config=publisher
    if mode=='uncontrolled':
        with conn.cursor() as cur:cur.execute('DELETE FROM trading.runtime_intents')
    result=invoke(mode);assert 'EMPTY_PUBLICATION_COMMITTED=1' in result.stdout
    with conn.cursor() as cur:
        cur.execute('SELECT to_jsonb(p) FROM trading.qt_empty_model_owner_publications p');rows=cur.fetchall();assert len(rows)==1;p=rows[0][0]
        assert p['schema_version']=='qt-empty-model-owner-publication/v2'
        assert p['configuration_snapshot']==config and p['system_components']==[] and p['proposal_components']==[] and p['qt_components']==[]
        assert p['configured_owner_names']==[OWNER] and p['registry_id']=='eq-owner'
        assert p['fresh_empty_batches']==[dict(portfolio_id=BOOK,strategy_id=ENGINE,strategy_name=OWNER,source_day=DAY)]
        capture=p['inspection_capture'];assert capture['status']=='available' and capture['reason']=='none'
        assert capture['equity_run_consumption']['available'] is True and capture['equity_run_consumption']['complete'] is True
        assert capture['identity']['capture_id']==capture['identity']['publication_id']==p['publication_id']
        assert (capture['identity']['runtime_attempt_id'] is None)==(mode=='uncontrolled')
        cur.execute('SELECT count(*) FROM trading.positions');assert cur.fetchone()==(0,)
        cur.execute('SELECT count(*) FROM trading.qt_model_seed_publications');assert cur.fetchone()==(0,)
        if mode=='controlled':
            cur.execute('SELECT status,outcome,publication_id::text FROM trading.runtime_attempts');assert cur.fetchone()==('applied','published',p['publication_id'])

def test_actual_empty_model_does_not_erase_or_resize_retained_fractional_qt(publisher):
    conn,_=publisher;seed(conn,ENGINE,OWNER,'qt');before=snapshot(conn)['positions']
    assert 'EMPTY_PUBLICATION_COMMITTED=1' in invoke('controlled').stdout
    assert snapshot(conn)['positions']==before
    with conn.cursor() as cur:
        cur.execute('SELECT qt_components FROM trading.qt_empty_model_owner_publications');rows=cur.fetchone()[0]
        assert len(rows)==1 and rows[0]['quantity_exact']=='-0.5' and rows[0]['average_price_exact']=='100'

@pytest.mark.parametrize('stream',['system','qt_proposal','qt'])
@pytest.mark.parametrize('foreign',['engine','owner'])
def test_complete_book_day_inventory_refuses_unarchived_engine_or_owner_atomically(publisher,foreign,stream):
    conn,_=publisher;seed(conn,'FOREIGN' if foreign=='engine' else ENGINE,'FOREIGN' if foreign=='owner' else OWNER,stream)
    before=snapshot(conn);result=invoke('controlled')
    assert 'EMPTY_PUBLICATION_REFUSED=1' in result.stdout
    assert snapshot(conn)==before
    with conn.cursor() as cur:
        cur.execute('SELECT status,publication_id FROM trading.runtime_attempts');assert cur.fetchone()==('failed',None)

@pytest.mark.parametrize('mode',['duplicate_batch','missing_batch','callback_failure','unavailable_capture','staging_allocation_failure'])
def test_missing_or_failed_real_callbacks_leave_no_partial_empty_publication(publisher,mode):
    conn,_=publisher;before=snapshot(conn);assert 'EMPTY_PUBLICATION_REFUSED=1' in invoke(mode).stdout
    assert snapshot(conn)==before

def test_insert_suppression_rule_refuses_before_any_publisher_writes(publisher):
    conn,_=publisher
    with conn.cursor() as cur:
        cur.execute("CREATE RULE suppress_empty_archive AS ON INSERT TO trading.qt_empty_model_owner_publications DO INSTEAD NOTHING")
    before=snapshot(conn)
    assert 'EMPTY_PUBLICATION_REFUSED=1' in invoke('controlled').stdout
    assert snapshot(conn)==before
    with conn.cursor() as cur:
        cur.execute('SELECT status,publication_id FROM trading.runtime_attempts');assert cur.fetchone()==('failed',None)


def test_real_single_owner_trace_does_not_certify_second_configured_empty_owner(publisher):
    conn,config=publisher
    # The actual approved snapshot and two completed callbacks agree, but the
    # single-owner run trace cannot certify a second configured owner.
    assert sorted(config['strategies'])==[OWNER,'UNOBSERVED_MEMBER']
    before=snapshot(conn)
    assert 'EMPTY_PUBLICATION_REFUSED=1' in invoke('unobserved_second_owner').stdout
    assert snapshot(conn)==before
