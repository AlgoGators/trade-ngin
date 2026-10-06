"""Actual complete native publication boundary. Synthetic next MODEL payload only."""
from pathlib import Path
from tests.qt_test_artifacts import artifact
import json,os,subprocess,sys
import pytest
STAGE=Path(__file__).resolve().parents[1]
WORKSPACE=next(p for p in STAGE.parents if (p/'.review/trade-ngin-qt').is_dir())
sys.path.insert(0,str(WORKSPACE/'.review/trade-ngin-qt/tests/integration'))
sys.path.insert(0,str(WORKSPACE/'docs/repairs/2026-09-26-hemdutt-issue-completion/equity-finalization-staging'))
sys.path.insert(0,str(STAGE/'tests/fixtures'))
from mr_equity_eod_fixture import (eod,eod_clock,equity,desk,connection,finalize,
    all_state,original_rows,BOOK,DECISION,SUCCESSOR)
from psycopg2.extras import Json
PROBE=artifact("equity_prior_publication_probe")

def argv(source,market,mode):
    assert PROBE.is_file(),'coherent actual EQ bridge required; missing binary is setup failure'
    return [str(PROBE),mode,DECISION,SUCCESSOR,BOOK,source['source_day'],market['source_day']]

@pytest.fixture()
def approved_bridge(eod):
    conn,source,_,_,_,market=eod
    result=finalize();assert result.returncode==0,result.stdout+result.stderr
    snapshot=subprocess.run(argv(source,market,'snapshot'),capture_output=True,text=True,
        timeout=20,env=os.environ.copy())
    assert snapshot.returncode==0,snapshot.stdout+snapshot.stderr
    rows=[line for line in snapshot.stdout.splitlines() if line.startswith('{')]
    assert len(rows)==1,snapshot.stdout
    with conn.cursor() as cursor:
        cursor.execute("SELECT id,runtime_revision FROM trading.strategy_registry WHERE "
            "strategy_type='LIVE_EQUITY_MEAN_REVERSION' AND portfolio_id=%s",(BOOK,))
        identity,revision=cursor.fetchone()
        cursor.execute("INSERT INTO trading.runtime_intents(registry_id,portfolio_id,engine_strategy_id,action,"
            "registry_revision,config_snapshot,status,requested_by,request_reason) VALUES"
            "(%s,%s,'LIVE_EQUITY_MEAN_REVERSION','run',%s,%s,'pending','1','synthetic owned bridge request') RETURNING id",
            (identity,BOOK,revision,Json(json.loads(rows[0]))))
        intent=cursor.fetchone()[0]
        cursor.execute("UPDATE trading.runtime_intents SET status='approved',approved_by='3',"
            "approval_reason='synthetic bridge protocol gate',approved_at=clock_timestamp() WHERE id=%s",(intent,))
    return conn,source,market

def invoke(source,market,mode):
    result=subprocess.run(argv(source,market,mode),capture_output=True,text=True,timeout=30,env=os.environ.copy())
    assert result.returncode==0,result.stdout+result.stderr
    return result

def publication_state(conn):
    result=all_state(conn)
    with conn.cursor() as cursor:
        for table in ('live_run_metadata','run_inputs','risk_limits','signals'):
            cursor.execute('SELECT to_jsonb(t)::text FROM trading.'+table+' t ORDER BY to_jsonb(t)::text')
            result.append(cursor.fetchall())
    return result

def test_actual_complete_equity_publication_seals_same_model_identity_and_exact_prior(approved_bridge):
    conn,source,market=approved_bridge;before=original_rows(conn)
    result=invoke(source,market,'publish')
    assert 'EQ_ALL_PARTS_QUEUED=1' in result.stdout and 'EQ_PUBLICATION_COMMITTED=1' in result.stdout
    with conn.cursor() as cursor:
        cursor.execute('SELECT portfolio_config FROM trading.live_run_metadata WHERE strategy_id=%s '
            'AND portfolio_id=%s AND date=%s',('LIVE_EQUITY_MEAN_REVERSION',BOOK,market['source_day']))
        child=cursor.fetchone()[0]['config_inspection']
        assert set(child)=={'publication_schema_version','profile','authority','stream','identity','captured_at',
            'publication_recorded_at','status','reason','equity_run_consumption'}
        assert child['publication_schema_version']==3 and child['profile']=='live_equity_mean_reversion'
        assert child['authority']=='inspection_only' and child['stream']=='system'
        # This test never invokes MR: its partial stage trace must stay unavailable.
        assert child['status']=='unavailable' and child['reason']=='consumption_unavailable'
        assert child['equity_run_consumption']['available'] is False
        assert child['equity_run_consumption']['complete'] is False
        cursor.execute('SELECT publication_id::text FROM trading.qt_model_seed_publications WHERE '
            'portfolio_id=%s AND strategy_id=%s AND source_day=%s ORDER BY publication_version DESC LIMIT 1',
            (BOOK,'LIVE_EQUITY_MEAN_REVERSION',market['source_day']))
        assert child['identity']['capture_id']==child['identity']['publication_id']==cursor.fetchone()[0]
        cursor.execute('SELECT engine_flags FROM trading.run_inputs WHERE portfolio_id=%s AND strategy_id=%s AND date=%s',
            (BOOK,'LIVE_EQUITY_MEAN_REVERSION',market['source_day']))
        prior=cursor.fetchone()[0]['equity_model_prior']
        assert prior['decision_id']==DECISION and prior['finalization_id']==SUCCESSOR
        assert prior['source_day']==source['source_day'] and prior['valuation_day']==market['source_day']
    assert original_rows(conn)==before

@pytest.mark.parametrize('mode',['missing_capture','bad_capture','wrong_owner','duplicate_capture',
    'missing_replay','replay_mismatch','trace_mismatch','wrong_token','duplicate_trace','incomplete','legacy_evidence',
    'callback_failure','eod_equity','eod_pnl','eod_realized','eod_cost','eod_capital'])
def test_missing_invalid_or_incomplete_bridge_never_changes_financial_or_workflow_state(approved_bridge,mode):
    conn,source,market=approved_bridge;before=publication_state(conn)
    result=invoke(source,market,mode)
    assert any(marker in result.stdout for marker in ['EQ_PUBLICATION_REFUSED=1','EQ_CAPTURE_REFUSED=1','EQ_LEGACY_PRIOR_REFUSED=1'])
    if mode.startswith('eod_') or mode in ('trace_mismatch','wrong_token','duplicate_trace','callback_failure'):
        assert 'EQ_ALL_PARTS_QUEUED=1' in result.stdout
    assert publication_state(conn)==before

@pytest.mark.parametrize('damage',['quantity','basis','mark_time','results','curve'])
def test_changed_physical_prior_between_capture_and_publication_is_rechecked_atomically(approved_bridge,damage):
    conn,source,market=approved_bridge
    process=subprocess.Popen(argv(source,market,'changed_prior'),stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=os.environ.copy())
    try:
        observed=[]
        while True:
            line=process.stdout.readline()
            assert line,observed
            observed.append(line)
            if 'EQ_PRIOR_CAPTURED=1' in line:break
        with conn.cursor() as cursor:
            if damage in ('quantity','basis','mark_time'):
                field={'quantity':'quantity','basis':'average_price','mark_time':'last_update'}[damage]
                value=field+("+interval '1 second'" if damage=='mark_time' else '+0.00000001')
                cursor.execute('UPDATE trading.positions SET '+field+'='+value+
                    " WHERE portfolio_id=%s AND portfolio_type='qt' AND date=%s",(BOOK,source['source_day']))
            elif damage=='results':
                cursor.execute("UPDATE trading.live_results SET total_pnl=total_pnl+0.00000001 "
                    "WHERE portfolio_id=%s AND portfolio_type='qt' AND (date::timestamptz AT TIME ZONE 'UTC')::date=%s",
                    (BOOK,source['source_day']))
            else:
                cursor.execute("UPDATE trading.equity_curve SET equity=equity+0.00000001 WHERE portfolio_id=%s "
                    "AND portfolio_type='qt' AND (timestamp AT TIME ZONE 'UTC')::date=%s",(BOOK,source['source_day']))
        before=publication_state(conn)
        out,err=process.communicate('publish\n',timeout=30)
        assert process.returncode==0,out+err+''.join(observed)
        assert 'EQ_ALL_PARTS_QUEUED=1' in out and 'EQ_PUBLICATION_REFUSED=1' in out
        assert publication_state(conn)==before
    finally:
        if process.poll() is None:process.kill();process.communicate(timeout=5)

def test_only_valid_system_reference_historical_uow_retains_prior_fence(approved_bridge):
    conn,source,market=approved_bridge;original=original_rows(conn)
    result=invoke(source,market,'uow_historical')
    assert 'EQ_HISTORICAL_UOW_COMMITTED=1' in result.stdout
    with conn.cursor() as cursor:
        cursor.execute("SELECT quantity::text FROM trading.positions WHERE portfolio_id=%s AND "
            "strategy_id='LIVE_EQUITY_MEAN_REVERSION' AND strategy_name='EQUITY_MEAN_REVERSION' "
            "AND portfolio_type='system' AND date=%s",(BOOK,source['source_day']))
        assert [__import__('decimal').Decimal(row[0]) for row in cursor.fetchall()]==[99]
    assert original_rows(conn)==original

@pytest.mark.parametrize('mode',['uow_wrong_owner','uow_wrong_book','uow_current','uow_future',
    'uow_failed_pending','uow_verified_prior'])
def test_historical_uow_exception_never_admits_wrong_scope_failed_or_current_write(approved_bridge,mode):
    conn,source,market=approved_bridge;before=publication_state(conn)
    result=invoke(source,market,mode)
    assert 'EQ_HISTORICAL_UOW_REFUSED=1' in result.stdout
    assert publication_state(conn)==before
