"""Actual native capability proof under two paths, not MODEL evidence."""
import ctypes
import os
from pathlib import Path
from tests.qt_test_artifacts import artifact
import subprocess
import sys

import pytest

WORKSPACE=next(p for p in Path(__file__).resolve().parents if (p/'.review/trade-ngin-qt').is_dir())
ENGINE=WORKSPACE/'.review/trade-ngin-qt'
INTEGRATION=ENGINE/'tests/integration'
sys.path.insert(0,str(INTEGRATION))
import test_qt_empty_owner_schema as schema
assert Path(schema.__file__).resolve()==(INTEGRATION/'test_qt_empty_owner_schema.py').resolve()
from test_qt_empty_owner_schema import empty_schema,predecessor,connection

@pytest.fixture(scope='module',autouse=True)
def isolated_probe_required():
    present=all(os.environ.get(name) for name in ('QT_CATALOG_IDENTITY_PROBE','QT_CATALOG_IDENTITY_PROBE_SHA256'))
    if not present:
        if os.environ.get('QT_CATALOG_IDENTITY_GATE_REQUIRED')=='1':
            pytest.fail('Closed catalog gate requires its pinned isolated capability probe')
        pytest.skip('Catalog identity regression runs only in the reviewed isolated capability gate')

# The combined reviewed isolated wrapper must supply its exact local probe and
# before/after hash, never a shared cached binary or an arbitrary program path.
def invoke(mode):
    probe=Path(os.environ['QT_CATALOG_IDENTITY_PROBE'])
    assert probe.is_absolute() and probe.is_file() and not probe.is_symlink()
    from hashlib import sha256
    assert sha256(probe.read_bytes()).hexdigest()==os.environ['QT_CATALOG_IDENTITY_PROBE_SHA256']
    guard=artifact("libqt_no_delivery_guard.so")
    assert guard.is_file() and not guard.is_symlink()
    assert sha256(guard.read_bytes()).hexdigest()=='f895b1de8a5a623446d445914038d66397248b40bf0e8a9ecbbbff76adaae4a9'
    assert ctypes.CDLL(str(guard)).qt_no_delivery_guard_loaded()==1
    env=os.environ.copy();env.update(LD_PRELOAD=str(guard),QT_EMAIL_DELIVERY_ENABLED='false')
    result=subprocess.run([str(probe),'capability',mode],capture_output=True,text=True,timeout=30,env=env)
    assert result.returncode==0,(result.returncode,result.stdout[-2000:],result.stderr[-500:])
    return result.stdout

def snapshot(conn):
    with conn.cursor() as cur:
        cur.execute("SELECT current_setting('search_path')")
        path=cur.fetchone()[0]
        cur.execute("SELECT a.adrelid,a.adnum,a.adbin::text FROM pg_attrdef a WHERE a.adrelid IN ('trading.qt_empty_model_owner_publications'::regclass,'trading.qt_model_seed_publications'::regclass) ORDER BY 1,2")
        defaults=cur.fetchall()
        cur.execute("SELECT tgrelid,tgname,tgtype,tgfoid,tgenabled FROM pg_trigger WHERE tgrelid IN ('trading.qt_empty_model_owner_publications'::regclass,'trading.qt_model_seed_publications'::regclass) ORDER BY 1,2")
        triggers=cur.fetchall()
        rows={}
        for name in ('qt_empty_model_owner_publications','qt_model_seed_publications','qt_storage_capabilities'):
            cur.execute('SELECT to_jsonb(t)::text FROM trading.'+name+' t ORDER BY to_jsonb(t)::text')
            rows[name]=cur.fetchall()
        return path,defaults,triggers,rows

@pytest.mark.parametrize('mode',['default','trading_visible'])
def test_identical_proved_catalog_is_accepted_independently_of_relation_display(empty_schema,mode):
    conn=empty_schema;before=snapshot(conn)
    with conn.cursor() as cur:
        cur.execute('BEGIN')
        try:
            cur.execute('SET LOCAL search_path='+('pg_catalog,public' if mode=='default' else 'trading,pg_catalog,public'))
            cur.execute("SELECT 'trading.qt_model_seed_publications'::regclass::text")
            assert cur.fetchone()[0]==('trading.qt_model_seed_publications' if mode=='default' else 'qt_model_seed_publications')
        finally:cur.execute('ROLLBACK')
    assert 'CATALOG_CAPABILITY_ACCEPTED=1' in invoke(mode)
    assert snapshot(conn)==before

@pytest.mark.parametrize('mode',['default','trading_visible'])
def test_altered_legacy_trigger_is_refused_under_both_paths_without_writes(empty_schema,mode):
    conn=empty_schema
    with conn.cursor() as cur:
        cur.execute('ALTER TABLE trading.qt_model_seed_publications DISABLE TRIGGER qt_model_seed_identity_guard')
    before=snapshot(conn)
    assert 'CATALOG_CAPABILITY_REFUSED=1' in invoke(mode)
    assert snapshot(conn)==before
