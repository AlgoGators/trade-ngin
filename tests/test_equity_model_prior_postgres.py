"""Actual MR preview/processed priorâ†’finalizationâ†’MODEL prior admission; owned DB."""
from copy import deepcopy
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
    all_state,BOOK,DECISION,SUCCESSOR)
PROBE=artifact("equity_model_prior_probe")

def invoke(source,*,decision=DECISION,finalization=SUCCESSOR,book=BOOK,day=None,valuation=None):
    assert PROBE.is_file(),'coherent actual prior probe build required before behavioral gate'
    return subprocess.run([str(PROBE),decision,finalization,book,day or source['source_day'],
        valuation or source['timestamp'][:10]],capture_output=True,text=True,timeout=30,env=os.environ.copy())

@pytest.fixture()
def finalized_mr(eod):
    conn,source,_,_,_,market=eod
    result=finalize();assert result.returncode==0,result.stdout+result.stderr
    return conn,source,market,json.loads(result.stdout)

def test_actual_matching_mr_processed_prior_reads_source_date_with_later_mark(finalized_mr):
    conn,source,market,successor=finalized_mr
    before=all_state(conn)
    result=invoke(source,valuation=market['source_day'])
    assert result.returncode==0,result.stdout+result.stderr
    output=json.loads(result.stdout)
    assert output['positions']=={'SYN':{'quantity_exact':'5','average_price_exact':successor['after_financial']['positions'][0]['average_price_exact']}}
    assert output['financial']==successor['after_financial']
    assert output['reference']['decision_id']==DECISION
    assert output['reference']['finalization_id']==SUCCESSOR
    assert output['reference']['source_day']==source['source_day']<market['source_day']
    assert all(row['key']['date']==source['source_day'] and row['last_update'][:10]==market['source_day'] for row in output['financial']['positions'])
    assert all(row['key']['strategy_id']=='LIVE_EQUITY_MEAN_REVERSION' and row['key']['strategy_name']=='EQUITY_MEAN_REVERSION' for row in output['basis_positions'])
    with conn.cursor() as cur:
        cur.execute("SELECT quantity::text FROM trading.positions WHERE portfolio_type='system' AND date=%s AND portfolio_id=%s",(source['source_day'],BOOK))
        assert [row[0] for row in cur.fetchall()]!=['5']
    assert all_state(conn)==before

@pytest.mark.parametrize('damage',['quantity','basis','mark_time','realized','unrealized','equity','curve','execution'])
def test_verified_prior_refuses_actual_physical_tamper_without_mutating_any_state(finalized_mr,damage):
    conn,source,market,_=finalized_mr
    with conn.cursor() as c:
        if damage in ['quantity','basis','realized','unrealized']:
            field={'basis':'average_price','realized':'daily_realized_pnl','unrealized':'daily_unrealized_pnl'}.get(damage,damage)
            c.execute('UPDATE trading.positions SET '+field+'='+field+"+0.00000001 WHERE portfolio_id=%s AND portfolio_type='qt' AND date=%s",(BOOK,source['source_day']))
        elif damage=='mark_time':c.execute("UPDATE trading.positions SET last_update=last_update+interval '1 second' WHERE portfolio_id=%s AND portfolio_type='qt' AND date=%s",(BOOK,source['source_day']))
        elif damage=='equity':c.execute("UPDATE trading.live_results SET current_portfolio_value=current_portfolio_value+0.00000001 WHERE portfolio_id=%s AND portfolio_type='qt' AND (date::timestamptz AT TIME ZONE 'UTC')::date=%s",(BOOK,source['source_day']))
        elif damage=='curve':c.execute("UPDATE trading.equity_curve SET equity=equity+0.00000001 WHERE portfolio_id=%s AND portfolio_type='qt' AND (timestamp AT TIME ZONE 'UTC')::date=%s",(BOOK,source['source_day']))
        else:c.execute("UPDATE trading.executions SET quantity=quantity+0.00000001 WHERE portfolio_id=%s AND portfolio_type='qt' AND date=%s",(BOOK,source['source_day']))
    before=all_state(conn);r=invoke(source,valuation=market['source_day'])
    assert r.returncode==10 and 'EQUITY_MODEL_PRIOR_REFUSED=1' in r.stdout,r.stdout+r.stderr
    assert all_state(conn)==before

@pytest.mark.parametrize('damage',['decision','finalization','book','source_day','valuation_day'])
def test_explicit_prior_identity_has_no_discovery_or_stream_fallback(finalized_mr,damage):
    conn,source,market,_=finalized_mr
    args={'valuation':market['source_day']}
    args.update({'decision':'40000000-0000-4000-8000-000000000099'} if damage=='decision' else
        {'finalization':'b0000000-0000-4000-8000-000000000099'} if damage=='finalization' else
        {'book':'FOREIGN'} if damage=='book' else {'day':market['source_day']} if damage=='source_day' else {'valuation':source['source_day']})
    before=all_state(conn);r=invoke(source,**args)
    assert r.returncode==10 and 'EQUITY_MODEL_PRIOR_REFUSED=1' in r.stdout,r.stdout+r.stderr
    assert all_state(conn)==before

@pytest.mark.parametrize('equity',['split','dividend'],indirect=True)
def test_action_restated_qt_basis_is_never_admitted_to_system_history_replay(finalized_mr):
    conn,source,market,_=finalized_mr;before=all_state(conn)
    r=invoke(source,valuation=market['source_day'])
    assert r.returncode==10 and 'EQUITY_MODEL_PRIOR_REFUSED=1' in r.stdout,r.stdout+r.stderr
    assert all_state(conn)==before
