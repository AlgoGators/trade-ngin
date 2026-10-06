"""New EQ methods consume the same physical UTC data in both DB timezones."""
from pathlib import Path
from tests.qt_test_artifacts import artifact
import json
import os
import subprocess
import sys
import pytest

WORKSPACE=next(p for p in Path(__file__).resolve().parents if (p/'.review/trade-ngin-qt').is_dir())
sys.path.insert(0,str(WORKSPACE/'.review/trade-ngin-qt/tests/integration'))
from test_runtime_control_schema import connection
PROBE=artifact("equity_utc_readers_probe")

def invoke(mode,timezone):
    assert PROBE.is_file(),'coherent actual EQ probe required; setup failure is not behavioral RED'
    result=subprocess.run([str(PROBE),mode,timezone],capture_output=True,text=True,
        timeout=20,env=os.environ.copy())
    assert result.returncode==0,result.stdout+result.stderr
    return result.stdout

@pytest.mark.parametrize('timezone',['UTC','America/New_York'])
def test_historical_closes_use_utc_dates_and_half_open_range(connection,timezone):
    with connection.cursor() as cursor:
        cursor.execute('DROP SCHEMA IF EXISTS equities_data CASCADE;CREATE SCHEMA equities_data;'
            'CREATE TABLE equities_data.ohlcv_1d(time timestamptz,symbol text,close double precision)')
        cursor.execute("INSERT INTO equities_data.ohlcv_1d VALUES "
            "('2026-09-24 23:30+00','SYN',8),('2026-09-25 00:30+00','SYN',101),"
            "('2026-09-25 23:30+00','ZYN',102),('2026-09-26 00:30+00','SYN',9),"
            "('2026-09-25 01:30+00','OTHER',7),('2026-09-25 02:30+00','NULL',NULL)")
        cursor.execute('SELECT to_jsonb(t)::text FROM equities_data.ohlcv_1d t ORDER BY to_jsonb(t)::text')
        before=cursor.fetchall()
    output=invoke('closes',timezone)
    rows=[line for line in output.splitlines() if line.startswith('{')]
    assert len(rows)==1,output
    assert json.loads(rows[0])=={'SYN':{'2026-09-25':101},'ZYN':{'2026-09-25':102}}
    with connection.cursor() as cursor:
        cursor.execute('SELECT to_jsonb(t)::text FROM equities_data.ohlcv_1d t ORDER BY to_jsonb(t)::text')
        assert cursor.fetchall()==before

@pytest.mark.parametrize('timezone',['UTC','America/New_York'])
def test_explicit_equity_curve_fallback_never_reads_later_utc_rows(connection,timezone):
    with connection.cursor() as cursor:
        cursor.execute("DELETE FROM trading.strategy_registry;INSERT INTO trading.strategy_registry "
            "(id,strategy_type,portfolio_id) VALUES('mr','LIVE_EQUITY_MEAN_REVERSION','EQUITY_MR_PORTFOLIO')")
        cursor.execute("INSERT INTO trading.equity_curve VALUES "
            "('LIVE_EQUITY_MEAN_REVERSION','EQUITY_MR_PORTFOLIO','2026-09-24 23:30+00',10000,'system'),"
            "('LIVE_EQUITY_MEAN_REVERSION','EQUITY_MR_PORTFOLIO','2026-09-25 00:30+00',20000,'system'),"
            "('LIVE_EQUITY_MEAN_REVERSION','EQUITY_MR_PORTFOLIO','2026-09-24 23:45+00',30000,'qt')")
        cursor.execute('SELECT to_jsonb(t)::text FROM trading.equity_curve t ORDER BY to_jsonb(t)::text')
        before=cursor.fetchall()
    assert 'EQ_CURVE_SAVED=1' in invoke('equity',timezone)
    with connection.cursor() as cursor:
        cursor.execute("SELECT equity::text FROM trading.equity_curve WHERE "
            "strategy_id='LIVE_EQUITY_MEAN_REVERSION' AND portfolio_id='EQUITY_MR_PORTFOLIO' "
            "AND portfolio_type='system' AND timestamp='2026-09-25 00:00+00'")
        assert cursor.fetchall()==[('10000',)]
        cursor.execute("SELECT to_jsonb(t)::text FROM trading.equity_curve t WHERE timestamp<>"
            "'2026-09-25 00:00+00' ORDER BY to_jsonb(t)::text")
        assert cursor.fetchall()==before


@pytest.mark.parametrize('timezone',['UTC','America/New_York'])
def test_explicit_current_equity_write_keeps_exact_utc_instant(connection,timezone):
    with connection.cursor() as cursor:
        cursor.execute("DELETE FROM trading.strategy_registry;INSERT INTO trading.strategy_registry "
            "(id,strategy_type,portfolio_id) VALUES('mr','LIVE_EQUITY_MEAN_REVERSION','EQUITY_MR_PORTFOLIO')")
        cursor.execute("INSERT INTO trading.equity_curve VALUES "
            "('LIVE_EQUITY_MEAN_REVERSION','EQUITY_MR_PORTFOLIO','2026-09-24 23:30+00',10000,'system'),"
            "('LIVE_EQUITY_MEAN_REVERSION','EQUITY_MR_PORTFOLIO','2026-09-24 23:45+00',30000,'qt')")
        cursor.execute('SELECT to_jsonb(t)::text FROM trading.equity_curve t ORDER BY to_jsonb(t)::text')
        before=cursor.fetchall()
    assert 'EQ_CURVE_SAVED=1' in invoke('equity-valid',timezone)
    with connection.cursor() as cursor:
        cursor.execute("SELECT timestamp AT TIME ZONE 'UTC',equity::text,portfolio_type "
            "FROM trading.equity_curve WHERE strategy_id='LIVE_EQUITY_MEAN_REVERSION' "
            "AND portfolio_id='EQUITY_MR_PORTFOLIO' AND timestamp='2026-09-25 00:00+00'")
        from datetime import datetime
        assert cursor.fetchall()==[(datetime(2026,9,25),'12000','system')]
        cursor.execute("SELECT to_jsonb(t)::text FROM trading.equity_curve t WHERE timestamp<>"
            "'2026-09-25 00:00+00' ORDER BY to_jsonb(t)::text")
        assert cursor.fetchall()==before
