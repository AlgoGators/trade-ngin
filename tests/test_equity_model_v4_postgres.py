"""Real typed registry/SQL/Arrow consumers over explicit artificial public bars.

Unexecuted until the staged real dependencies are reviewed and compiled.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import pytest

WORKSPACE=next(p for p in Path(__file__).resolve().parents if (p/'.review/trade-ngin-qt').is_dir())
sys.path.insert(0,str(WORKSPACE/'.review/trade-ngin-qt/tests/integration'))
from test_runtime_control_schema import connection

PROBE=Path('/home/devcontainers/qt-validation-20260921/bin/Debug/equity_model_v4_probe')

@pytest.fixture()
def market(connection):
    root=WORKSPACE/'.review/trade-ngin-qt'
    public=(root/'tests/fixtures/qt_model_baseline/base.sql').read_text()
    metadata=public[public.index('CREATE TABLE metadata.contract_metadata'):public.index('CREATE TABLE futures_data.ohlcv_1d')]
    with connection.cursor() as cur:
        cur.execute('DROP SCHEMA IF EXISTS metadata CASCADE; CREATE SCHEMA metadata')
        cur.execute(metadata)
        for schema in ('equities_data','futures_data'):
            cur.execute('DROP SCHEMA IF EXISTS '+schema+' CASCADE; CREATE SCHEMA '+schema)
            cur.execute('CREATE TABLE '+schema+'.ohlcv_1d(time timestamptz,symbol text,open double precision,'
                'high double precision,low double precision,close double precision,volume double precision,'
                'div_cash double precision,split_factor double precision,adjusted_close double precision)')
            # Physical insertion order deliberately differs from chronological
            # order. Stale vendor adjusted_close is intentionally contradictory.
            cur.execute('INSERT INTO '+schema+".ohlcv_1d VALUES "
                "('2026-09-25 16:00+00','SYN',49,51,47,49,300,1,1,999),"
                "('2026-09-23 16:00+00','SYN',100,102,98,100,100,0,1,999),"
                "('2026-09-24 16:00+00','SYN',50,52,48,50,200,0,2,999),"
                "('2026-09-24 16:00+00','OTHER',123,124,122,123,9999,0,1,999)")
    return connection

def snapshot(connection):
    with connection.cursor() as cur:
        result=[]
        for table in ('metadata.contract_metadata','equities_data.ohlcv_1d','futures_data.ohlcv_1d'):
            cur.execute('SELECT to_jsonb(t)::text FROM '+table+' t ORDER BY to_jsonb(t)::text')
            result.append(cur.fetchall())
        return result

def invoke(mode):
    result=subprocess.run([str(PROBE),mode],env=os.environ.copy(),capture_output=True,text=True,timeout=20)
    assert result.returncode==0,result.stdout+result.stderr
    # Logger output remains intact; decode only the probe's separately emitted
    # JSON line, without editing or normalizing any retained subprocess bytes.
    candidates=[line for line in result.stdout.splitlines() if line.startswith(('{','['))]
    assert len(candidates)==1,result.stdout
    return json.loads(candidates[0])

def test_equity_reads_actual_raw_events_filters_symbols_and_keeps_volume(market):
    before=snapshot(market);rows=invoke('--equity')
    assert len(rows)==3 and [row['symbol'] for row in rows]==['SYN']*3
    assert [row['time'][:10] for row in rows]==['2026-09-23','2026-09-24','2026-09-25']
    assert [row['close'] for row in rows]==pytest.approx([49,49,49],rel=0,abs=1e-10)
    assert [row['volume'] for row in rows]==[100,200,300]
    assert rows[0]['high']==pytest.approx(49.98,rel=0,abs=1e-10)
    assert snapshot(market)==before

def test_existing_futures_reader_remains_unadjusted(market):
    before=snapshot(market);rows=invoke('--futures')
    assert [row['close'] for row in rows]==[100,50,49]
    assert [row['volume'] for row in rows]==[100,200,300]
    assert snapshot(market)==before

def test_equity_all_symbols_preserves_independent_partition_and_query_order(market):
    before=snapshot(market);rows=invoke('--equity-all')
    assert [(row['time'][:10],row['symbol']) for row in rows]==[
        ('2026-09-23','SYN'),('2026-09-24','OTHER'),('2026-09-24','SYN'),('2026-09-25','SYN')]
    assert rows[1]['close']==123
    assert snapshot(market)==before

def test_actual_equity_loader_preserves_same_symbol_future_and_refuses_partial_invalid_request(market):
    before=snapshot(market);result=invoke('--registry')
    assert result=={'future_preserved':True,'variant_is_future':True,'generic_is_equity':True,
        'future_multiplier':50,'equity_point_value':1,'equity_commission':.005,'equity_exchange':'NYSE',
        'invalid_refused_without_partial_registration':True}
    assert snapshot(market)==before


def empty_metadata(market):
    with market.cursor() as cursor:
        cursor.execute('DELETE FROM metadata.contract_metadata')
    return snapshot(market)

def test_empty_contract_metadata_returns_valid_typed_zero_rows_without_writes(market):
    before = empty_metadata(market)
    observed = invoke('--empty-metadata')
    expected_names = [
        'Name', 'Databento Symbol', 'IB Symbol', 'Asset Type', 'Sector', 'Exchange',
        'Contract Size', 'Minimum Price Fluctuation', 'Tick Size', 'Trading Hours (EST)',
        'Overnight Initial Margin', 'Overnight Maintenance Margin', 'Intraday Initial Margin',
        'Intraday Maintenance Margin', 'Units', 'Data Provider', 'Dataset', 'Contract Months']
    numeric = {6, 7, 10, 11, 12, 13}
    assert observed == {'rows': 0, 'fields': [
        {'name': name, 'type': 'double' if i in numeric else 'string'}
        for i, name in enumerate(expected_names)]}
    assert snapshot(market) == before

def test_empty_contract_registry_load_is_successful_and_invents_no_instruments(market):
    before = empty_metadata(market)
    assert invoke('--empty-registry') == {'instruments': 0}
    assert snapshot(market) == before
