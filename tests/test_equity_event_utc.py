"""Actual new native reader; same fixture across DB session timezones, no normalization."""
import json
import os
from pathlib import Path
from tests.qt_test_artifacts import artifact
import subprocess
import sys
import pytest

WORKSPACE=next(p for p in Path(__file__).resolve().parents if (p/'.review/trade-ngin-qt').is_dir())
sys.path.insert(0,str(WORKSPACE/'.review/trade-ngin-qt/tests/integration'))
from test_runtime_control_schema import connection
PROBE=artifact("equity_event_utc_probe")

@pytest.fixture()
def events(connection):
    with connection.cursor() as cursor:
        cursor.execute('DROP SCHEMA IF EXISTS equities_data CASCADE;CREATE SCHEMA equities_data')
        cursor.execute('CREATE TABLE equities_data.ohlcv_1d(time timestamptz,symbol text,div_cash double precision,split_factor double precision)')
        cursor.execute("INSERT INTO equities_data.ohlcv_1d VALUES "
            "('2026-09-26 00:30+00','SYN',9,1),"
            "('2026-09-25 23:30+00','ZYN',2,1),"
            "('2026-09-24 23:30+00','SYN',8,1),"
            "('2026-09-25 00:30+00','SYN',1,2),"
            "('2026-09-25 01:30+00','OTHER',7,1),"
            "('2026-09-25 02:30+00','SYN',0,1)")
    return connection

def snapshot(connection):
    with connection.cursor() as cursor:
        cursor.execute('SELECT to_jsonb(t)::text FROM equities_data.ohlcv_1d t ORDER BY to_jsonb(t)::text')
        return cursor.fetchall()

def invoke(mode):
    result=subprocess.run([str(PROBE),mode],env=os.environ.copy(),capture_output=True,text=True,timeout=20)
    assert result.returncode==0,result.stdout+result.stderr
    records=[line for line in result.stdout.splitlines() if line.startswith('[')]
    assert len(records)==1,result.stdout
    return json.loads(records[0])

@pytest.mark.parametrize('timezone',['--utc','--new-york'])
def test_actual_event_date_and_half_open_day_range_are_utc_independent(events,timezone):
    before=snapshot(events)
    assert invoke(timezone)==[
        {'date':'2026-09-25','symbol':'SYN','action':'split','value':2},
        {'date':'2026-09-25','symbol':'SYN','action':'dividend','value':1},
        {'date':'2026-09-25','symbol':'ZYN','action':'dividend','value':2}]
    assert snapshot(events)==before
