"""Staged real S processor -> mark successor -> D sourced processor.

Synthetic explicitly governed rows only. Original and next preview financial
evidence comes from actual native calls, not copied evaluated output. No job
is launched by this module outside the explicit owned gate; missing schema/binary/setup failures never count as behavioral RED.
"""
from copy import deepcopy
from datetime import timedelta
from decimal import Decimal
from types import SimpleNamespace
import json
import os
import subprocess
import sys
from pathlib import Path
# Support staging-only selectors and later root-owned integration placement.
WORKSPACE=next(p for p in Path(__file__).resolve().parents if (p/'.review/trade-ngin-qt').is_dir())
sys.path.insert(0,str(WORKSPACE/'.review/trade-ngin-qt/tests/integration'))
sys.path.insert(0,str(Path(__file__).resolve().parent))
sys.path.insert(0,str(WORKSPACE/'docs/repairs/2026-09-26-hemdutt-issue-completion/equity-finalization-staging'))
import pytest
from synthetic_sql_clock import SyntheticSqlClock, S, D
from psycopg2.extras import Json, RealDictCursor
from mr_historical_qt_equity_fixture import (equity, desk, connection, run, state,
    isolated_rows, stored_output, digest, INPUT, MARKET, FINAL, BOOK, MODEL,
    DECISION, ATTEMPT, BINARY)
from mr_next_qt_desk_fixture import prepare_next_desk, MODEL as NEXT_MODEL, DECISION as NEXT_DECISION, ATTEMPT as NEXT_ATTEMPT

PROBE=BINARY.with_name('qt_equity_finalization_probe')
SUCCESSOR='b0000000-0000-4000-8000-000000000001'
D_MARKET='b1000000-0000-4000-8000-000000000001'
D_ACTIONS='qt-actions/b2000000-0000-4000-8000-000000000001'
D_INPUT='b3000000-0000-4000-8000-000000000001'

@pytest.fixture()
def eod_clock(connection):
    # Explicit reviewed test clock, restored before the owned database drops.
    # Original financial state always comes from the actual native processor.
    with SyntheticSqlClock(connection) as clock:
        yield clock

def invoke(*args):
    assert PROBE.is_file(), 'register/build actual finalization probe before RED'
    return subprocess.run([str(PROBE),*args],capture_output=True,text=True,
                          timeout=30,env=os.environ.copy())

def finalize(final_id=SUCCESSOR):return invoke('--finalize',DECISION,final_id,D_MARKET)
def sourced():return invoke('--sourced',NEXT_DECISION,NEXT_ATTEMPT,D_INPUT,D_MARKET,'qt-finalization/'+SUCCESSOR)
def refused(result,mode='FINALIZATION'):
    assert result.returncode==10,result.stdout+result.stderr
    assert 'EQUITY_'+mode+'_REFUSED=1' in result.stdout

def all_state(conn):
    result=state(conn)
    with conn.cursor() as cur:
        for table in ('qt_desk_finalizations','qt_decisions','qt_previews','qt_model_seed_publications'):
            cur.execute('SELECT to_jsonb(t)::text FROM trading.'+table+' t ORDER BY to_jsonb(t)::text')
            result.append(cur.fetchall())
    return result

def original_rows(conn):
    with conn.cursor() as cur:
        result=[]
        for table in ('qt_desk_receipts','qt_execution_observations','qt_desk_results','desk_run_results','qt_desk_accounting_inputs'):
            cur.execute('SELECT to_jsonb(t)::text FROM trading.'+table+' t WHERE decision_id=%s ORDER BY to_jsonb(t)::text',(DECISION,))
            result.append(cur.fetchall())
        cur.execute("SELECT to_jsonb(t)::text FROM trading.executions t WHERE portfolio_id='EQUITY_MR_PORTFOLIO' AND portfolio_type='qt' AND date=(SELECT source_day FROM trading.qt_decisions WHERE decision_id=%s) ORDER BY to_jsonb(t)::text",(DECISION,))
        result.append(cur.fetchall())
    return result

@pytest.fixture()
def eod(equity,request,eod_clock):
    conn,source=equity
    # An actual confirmed S decision, original accounting processor and receipt
    # precede every finalizer request. Setup failure is not unavailable-finalizer RED.
    actual=run();assert actual.returncode==0,actual.stdout+actual.stderr
    output=stored_output(conn);original=original_rows(conn);foreign=isolated_rows(conn)
    with conn.cursor() as cur:
        cur.execute('SELECT clock_timestamp()');assert cur.fetchone()[0]==S
    eod_clock.advance_to_D()
    with conn.cursor() as cur:
        cur.execute('SELECT clock_timestamp()::date,source_day FROM trading.qt_decisions WHERE decision_id=%s',(DECISION,))
        current,prior=cur.fetchone();assert str(prior)==source['source_day'] and prior<current
    scenario=getattr(request,'param','executed')
    prepare_next_desk(conn,SimpleNamespace(param=scenario))
    with conn.cursor(cursor_factory=RealDictCursor) as cur:
        cur.execute('SELECT * FROM trading.qt_desk_market_sources WHERE source_id=%s',(MARKET,));old=dict(cur.fetchone())
        cur.execute('SELECT clock_timestamp() now');now=cur.fetchone()['now'];assert now==D
        market=deepcopy(old['payload']);market.update(source_day=str(current),previous_day=str(prior),
            valuation_time=str(current)+'T00:00:00Z',model_publication_id=NEXT_MODEL,actions_source_id=D_ACTIONS)
        actions=dict(schema_version='qt-equity-actions-source/v1',book_id=BOOK,source_day=str(current),
            previous_day=str(prior),valuation_time=market['valuation_time'],events=[])
        market['actions_source_digest']=digest(actions)
        for instrument in market['instruments']:
            close='25' if instrument['symbol']=='IMM' else '55' if output['corporate_action_adjustments'] and output['corporate_action_adjustments'][0]['type']=='SPLIT' else '104'
            for field in ('reference','mark'):
                instrument[field].update(date=str(prior),source_id='owned-S-close/'+instrument['symbol'],
                    source_digest=digest(dict(symbol=instrument['symbol'],date=str(prior),close=close)),price_model_number=close)
            instrument['cost_evidence']['date']=str(prior)
        cur.execute("INSERT INTO trading.qt_equity_desk_evidence_sources(source_id,purpose,book_id,source_day,producer_id,policy_version,policy_revision,source_version,content_digest,payload) VALUES(%s,'actions',%s,%s,%s,%s,%s,%s,%s,%s)",
            (D_ACTIONS,BOOK,current,old['producer_id'],old['policy_version'],old['policy_revision'],D_ACTIONS,digest(actions),Json(actions)))
        cur.execute("INSERT INTO trading.qt_desk_market_sources(source_id,book_id,source_day,model_publication_id,producer_id,policy_version,policy_revision,source_version,as_of,valid_until,content_digest,payload) VALUES(%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)",
            (D_MARKET,BOOK,current,NEXT_MODEL,old['producer_id'],old['policy_version'],old['policy_revision'],D_MARKET,now-timedelta(seconds=1),now+timedelta(hours=1),digest(market),Json(market)))
    assert original_rows(conn)==original
    # Preparing D inserts its actual MODEL seeds before the finalizer runs.
    # Keep every preexisting foreign row byte-identical, and admit only those
    # new D system positions into the finalizer's complete isolation baseline.
    prepared_foreign=isolated_rows(conn)
    for index,(before,after) in enumerate(zip(foreign,prepared_foreign)):
        assert set(before)<=set(after)
        added=set(after)-set(before)
        if index!=0:
            assert not added
        for (raw,) in added:
            row=json.loads(raw)
            assert row['portfolio_id']==BOOK and row['portfolio_type']=='system'
            assert row['date']==str(current)
    foreign=prepared_foreign
    return conn,source,output,original,foreign,market
