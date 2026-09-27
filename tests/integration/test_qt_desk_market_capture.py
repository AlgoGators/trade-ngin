"""Actual market-table adapter; every input row is explicitly synthetic."""
import json
import pytest
from test_qt_desk_upstream import (upstream, upstream_state, invoke, MARKET, OLD,
                                 DECISION, FINAL, ATTEMPT)
from test_qt_desk_accounting import accounting
from test_qt_desk_storage import desk
from test_runtime_control_schema import connection

pytestmark=[pytest.mark.parametrize('desk',['futures_mes'],indirect=True),
            pytest.mark.parametrize('accounting',['no_input'],indirect=True)]

@pytest.fixture()
def capture(upstream):
    conn,source,_=upstream
    p=source['payload']
    with conn.cursor() as cur:
        cur.execute('CREATE SCHEMA IF NOT EXISTS futures_data; DROP TABLE IF EXISTS futures_data.ohlcv_1d; CREATE TABLE futures_data.ohlcv_1d '
            '(time timestamptz,symbol text,open numeric,high numeric,low numeric,close numeric,volume numeric)')
        cur.execute('INSERT INTO futures_data.ohlcv_1d VALUES(%s,%s,102,103,101,102,123456.25)',
                    (p['previous_day']+'T16:30:00Z','MES'))
    request=dict(source_id=MARKET,decision_id=DECISION,prior_decision_id=OLD,
                 as_of=source['as_of'],valid_until=source['valid_until'])
    return conn,request

def test_actual_market_reader_produces_governed_cost_state(capture):
    conn,request=capture
    r=invoke('--capture',payload=request)
    assert r.returncode==0,r.stdout+r.stderr
    row=json.loads(r.stdout);p=row['payload'];item=p['instruments'][0]
    assert item['symbol']=='MES'
    assert item['price_model_number']=='102'
    assert item['adv_model_number']=='123456.25'
    assert item['volatility_multiplier_model_number']=='1'
    assert item['asset_lookup']=='exact_symbol'
    assert item['point_value']=='5'
    assert p['capture']['convention']=='legacy-futures-fresh-manager-one-bar/v1'
    assert p['capture']['rows'][0]['source_time'].endswith('T16:30:00Z')
    assert p['capture']['rows'][0]['volatility_state']=='insufficient_returns_neutral'
    with conn.cursor() as cur:
        cur.execute('SELECT payload FROM trading.qt_desk_market_sources WHERE source_id=%s',(MARKET,))
        assert cur.fetchone()[0]==p

def test_missing_actual_market_bar_refuses_without_writes(capture):
    conn,request=capture
    with conn.cursor() as cur:cur.execute('DELETE FROM futures_data.ohlcv_1d')
    before=upstream_state(conn)
    result=invoke('--capture',payload=request)
    assert result.returncode==10,result.stdout+result.stderr
    assert 'qt_market_capture_unavailable' in result.stdout
    assert upstream_state(conn)==before

def test_duplicate_actual_market_bar_refuses_without_writes(capture):
    conn,request=capture
    with conn.cursor() as cur:cur.execute('INSERT INTO futures_data.ohlcv_1d SELECT * FROM futures_data.ohlcv_1d')
    before=upstream_state(conn)
    assert invoke('--capture',payload=request).returncode!=0
    assert upstream_state(conn)==before

def test_captured_actual_source_reaches_prior_finalization_and_current_accounting(capture):
    conn,request=capture
    r=invoke('--capture',payload=request);assert r.returncode==0,r.stdout+r.stderr
    r=invoke('--finalize',OLD,FINAL,MARKET);assert r.returncode==0,r.stdout+r.stderr
    r=invoke('--sourced',DECISION,ATTEMPT,'d0000000-0000-4000-8000-000000000009',MARKET,'qt-finalization/'+FINAL)
    assert r.returncode==0,r.stdout+r.stderr

def test_actual_close_and_volume_change_derived_data_but_not_cost_configuration(capture):
    conn,request=capture
    r=invoke('--capture',payload=request);assert r.returncode==0,r.stdout+r.stderr
    first=json.loads(r.stdout)['payload']
    with conn.cursor() as cur:
        cur.execute('UPDATE futures_data.ohlcv_1d SET close=102.125,volume=246912.5')
    request={**request,'source_id':'a0000000-0000-4000-8000-000000000002'}
    r=invoke('--capture',payload=request);assert r.returncode==0,r.stdout+r.stderr
    second=json.loads(r.stdout)['payload']
    assert second['instruments'][0]['price_model_number']=='102.125'
    assert second['instruments'][0]['adv_model_number']=='246912.5'
    assert second['dataset_digest']!=first['dataset_digest']
    assert second['cost_config_digest']==first['cost_config_digest']

def test_caller_cannot_supply_financial_operands(capture):
    conn,request=capture
    before=upstream_state(conn)
    assert invoke('--capture',payload={**request,'price_model_number':'999'}).returncode!=0
    assert upstream_state(conn)==before

def test_disabled_market_capture_policy_refuses_without_writes(capture):
    conn,request=capture
    with conn.cursor() as cur:
        cur.execute("UPDATE trading.qt_source_policies SET enabled=false,version=version+1 WHERE book_id='BOOK' AND purpose='execution'")
    before=upstream_state(conn)
    assert invoke('--capture',payload=request).returncode!=0
    assert upstream_state(conn)==before

@pytest.mark.parametrize('upstream',['dated_gap'],indirect=True)
def test_older_book_and_bar_are_not_substitutes_for_actual_previous_calendar_day(capture):
    conn,request=capture
    before=upstream_state(conn)
    assert invoke('--capture',payload=request).returncode!=0
    assert upstream_state(conn)==before

@pytest.mark.parametrize('mutation',['missing_capture','generic_version','wrong_version_id','raw_close',
    'raw_volume','raw_time','asset_config','dataset_digest','volatility_claim'])
def test_contradictory_actual_capture_claim_cannot_publish(capture,mutation):
    conn,request=capture
    r=invoke('--capture',payload=request);assert r.returncode==0,r.stdout+r.stderr
    source=json.loads(r.stdout)
    source['source_id']='a0000000-0000-4000-8000-000000000003'
    source['source_version']='qt-market-capture/'+source['source_id']
    p=source['payload']
    if mutation=='missing_capture':p.pop('capture')
    elif mutation=='generic_version':source['source_version']='generic-falsely-captured-v1'
    elif mutation=='wrong_version_id':source['source_version']='qt-market-capture/a0000000-0000-4000-8000-000000000004'
    elif mutation=='raw_close':p['capture']['rows'][0]['close']='102.5'
    elif mutation=='raw_volume':p['capture']['rows'][0]['volume']='246912.5'
    elif mutation=='raw_time':p['capture']['rows'][0]['source_time']=p['previous_day']+'T15:00:00Z'
    elif mutation=='asset_config':p['capture']['effective_configuration']['instruments'][0]['point_value']='6'
    elif mutation=='dataset_digest':p['dataset_digest']='4'*64
    elif mutation=='volatility_claim':p['capture']['rows'][0]['volatility_state']='observed_volatility'
    before=upstream_state(conn)
    r=invoke('--market',payload=source)
    assert r.returncode!=0,r.stdout+r.stderr
    assert upstream_state(conn)==before
