"""Actual controlled publisher membership admission; no MODEL invocation."""
import ctypes,json,os,subprocess,sys
from pathlib import Path
import pytest
from psycopg2.extras import Json
ROOT=next(p for p in Path(__file__).resolve().parents if (p/'.review/trade-ngin-qt').is_dir())
SOURCE=ROOT/'.review/trade-ngin-qt';INTEGRATION=SOURCE/'tests/integration'
sys.path[:0]=[str(INTEGRATION),str(SOURCE/'tests')]
import test_qt_empty_owner_publication as writer
import test_qt_empty_owner_schema as schema
assert Path(writer.__file__).resolve()==INTEGRATION/'test_qt_empty_owner_publication.py'
assert Path(schema.__file__).resolve()==INTEGRATION/'test_qt_empty_owner_schema.py'
from test_qt_empty_owner_schema import empty_schema,predecessor,connection

def actual(mode,day):
    assert writer.PROBE.is_file() and not writer.PROBE.is_symlink()
    assert writer.GUARD.is_file() and not writer.GUARD.is_symlink()
    assert ctypes.CDLL(str(writer.GUARD)).qt_no_delivery_guard_loaded()==1
    return subprocess.run([str(writer.PROBE),mode,day],capture_output=True,text=True,timeout=30,
        env=dict(os.environ,LD_PRELOAD=str(writer.GUARD),QT_EMAIL_DELIVERY_ENABLED='false'))

def operands(conn,*,primary,extra=False):
    with conn.cursor() as cur:
        cur.execute("SELECT (clock_timestamp() AT TIME ZONE 'UTC')::date::text")
        day=cur.fetchone()[0]
    snapshot=actual('snapshot',day);assert snapshot.returncode==0,(snapshot.returncode,snapshot.stdout,snapshot.stderr)
    configuration=json.loads(snapshot.stdout.split('EMPTY_SNAPSHOT=',1)[1])
    with conn.cursor() as cur:
        cur.execute('INSERT INTO trading.strategy_registry(id,strategy_type,portfolio_id) VALUES(%s,%s,%s)',('eq-owner',writer.ENGINE,primary))
        cur.execute('INSERT INTO trading.strategy_book_memberships(strategy_id,portfolio_id) VALUES(%s,%s)',('eq-owner',writer.BOOK))
        cur.execute("INSERT INTO trading.runtime_intents(registry_id,portfolio_id,engine_strategy_id,action,registry_revision,config_snapshot,status,requested_by,request_reason,approved_by,approval_reason,approved_at) SELECT 'eq-owner',%s,%s,'run',runtime_revision,%s,'approved','1','synthetic exact membership','2','owned actual publication control',now() FROM trading.strategy_registry WHERE id='eq-owner'",(writer.BOOK,writer.ENGINE,Json(configuration)))
        if extra:
            cur.execute('INSERT INTO trading.strategy_registry(id,strategy_type,portfolio_id) VALUES(%s,%s,%s)',('foreign-owner',writer.ENGINE,'FOREIGN_BOOK'))
            cur.execute('INSERT INTO trading.strategy_book_memberships(strategy_id,portfolio_id) VALUES(%s,%s)',('foreign-owner',writer.BOOK))
    return day,configuration

def test_actual_secondary_membership_admits_same_real_approved_registry_scope(empty_schema):
    day,configuration=operands(empty_schema,primary='OTHER_PRIMARY_BOOK')
    result=actual('controlled',day)
    assert result.returncode==0 and 'EMPTY_PUBLICATION_COMMITTED=1' in result.stdout,(result.returncode,result.stdout,result.stderr)
    with empty_schema.cursor() as cur:
        cur.execute('SELECT registry_id,portfolio_id,configuration_snapshot,system_components,proposal_components,qt_components FROM trading.qt_empty_model_owner_publications')
        assert cur.fetchall()==[('eq-owner',writer.BOOK,configuration,[],[],[])]
        cur.execute('SELECT status,outcome FROM trading.runtime_attempts');assert cur.fetchall()==[('applied','published')]
        cur.execute('SELECT count(*) FROM trading.positions');assert cur.fetchone()==(0,)

def test_same_engine_foreign_registry_membership_is_ambiguous_before_any_publication(empty_schema):
    day,_=operands(empty_schema,primary=writer.BOOK,extra=True)
    before=writer.snapshot(empty_schema)
    result=actual('controlled',day)
    assert result.returncode==69,(result.returncode,result.stdout,result.stderr)
    assert writer.snapshot(empty_schema)==before
    with empty_schema.cursor() as cur:
        cur.execute('SELECT count(*) FROM trading.runtime_attempts');assert cur.fetchone()==(0,)
