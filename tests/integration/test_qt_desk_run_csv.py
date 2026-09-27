"""Actual explicit postcommit CSV in the existing owned route-less PG fixtures.

No assertion supplies a trusted readiness/completion flag or computes financial
accounting in Python. The accounting processor must actually commit first.
"""
import csv
import hashlib
import json
import os
from pathlib import Path
import tempfile
import subprocess
import time
from test_qt_desk_run_cli import BINARY

import pytest
from test_qt_desk_run_cli import args,invoke,response,DECISION,ATTEMPT,INPUT
from test_qt_desk_accounting import accounting,desk,connection,all_state
from test_qt_equity_desk_accounting import equity


def owned(day,output,**identities):
    dsn=os.environ['ALGOLENS_TEST_DB']
    assert dsn.startswith('host=/tmp/algolens-repair-pg-') and 'dbname=algolens_test_' in dsn
    with tempfile.TemporaryFile() as material:
        material.write(dsn.encode());material.flush()
        fd=os.open(f'/proc/self/fd/{material.fileno()}',os.O_RDONLY)
        try:return invoke(args(day,**identities)+['--csv-output',str(output)],fd=fd)
        finally:os.close(fd)


def assert_snapshot(conn,output,answer):
    assert answer['status']=='accounted' and answer['csv_status']=='written'
    data=output.read_bytes();assert hashlib.sha256(data).hexdigest()==answer['csv_sha256']
    assert 'investor_published' not in answer and 'publication_payload' not in answer
    text=data.decode();assert '# Model columns: absent\n' in text
    line=next(x for x in text.splitlines() if x.startswith('# QT desk snapshot: '))
    manifest=json.loads(line.removeprefix('# QT desk snapshot: '))
    assert manifest['schema_version']=='qt-desk-csv-snapshot/v1'
    assert manifest['snapshot_kind']=='original_committed_accounting'
    assert manifest['decision_id']==DECISION and manifest['attempt_id']==ATTEMPT
    assert manifest['accounting_input_id']==INPUT and manifest['selected_book_digest']==answer['book_digest']
    with conn.cursor() as cur:
        cur.execute('SELECT publication_payload FROM trading.qt_desk_receipts WHERE decision_id=%s',(DECISION,))
        receipt=cur.fetchone()[0]
    expected=receipt['after_accounting']
    assert len(manifest['rows'])==len(expected)
    actual={json.dumps(row['key'],sort_keys=True):row['quantity_exact'] for row in manifest['rows']}
    assert actual=={json.dumps(row['key'],sort_keys=True):row['quantity_exact'] for row in expected}
    rows=list(csv.reader(line for line in text.splitlines() if not line.startswith('#')))
    assert rows[0]==['strategy','symbol','quantity','market_price','notional','pct_of_gross_notional','pct_of_portfolio_value']
    assert len(rows)-1==len(expected) and all(len(row)==7 for row in rows)
    for row in manifest['rows']:
        assert [row['csv_strategy'],row['key']['symbol'],row['quantity_exact']] in [r[:3] for r in rows[1:]]


@pytest.mark.parametrize('desk',['futures'],indirect=True)
def test_actual_futures_receipt_then_csv(accounting,tmp_path):
    conn,payload=accounting;output=tmp_path/'actual.csv'
    result=owned(payload['source_day'],output)
    answer=response(result,0,'accounted')
    assert_snapshot(conn,output,answer)


@pytest.mark.parametrize('desk',['futures'],indirect=True)
@pytest.mark.parametrize('accounting',['quiet'],indirect=True)
def test_quiet_components_and_same_symbol_owners_are_complete(accounting,tmp_path):
    conn,payload=accounting;output=tmp_path/'quiet.csv'
    answer=response(owned(payload['source_day'],output),0,'accounted')
    assert_snapshot(conn,output,answer)
    with conn.cursor() as cur:
        cur.execute('SELECT count(*) FROM trading.executions WHERE portfolio_type=%s',('qt',))
        assert cur.fetchone()[0]==0


@pytest.mark.parametrize('desk',['fractional'],indirect=True)
def test_actual_fractional_equity_receipt_then_csv(equity,tmp_path):
    conn,payload=equity;output=tmp_path/'fractional.csv'
    answer=response(owned(payload['source_day'],output),0,'accounted')
    assert_snapshot(conn,output,answer)
    with conn.cursor() as cur:
        cur.execute("SELECT quantity FROM trading.positions WHERE portfolio_id='BOOK' AND portfolio_type='qt' AND date=%s ORDER BY strategy_name",(payload['source_day'],))
        assert [str(row[0]).rstrip('0').rstrip('.') for row in cur.fetchall()]==['-0.5','2.25']


@pytest.mark.parametrize('desk',['futures'],indirect=True)
def test_replay_writes_new_explicit_file_without_reaccounting(accounting,tmp_path):
    conn,payload=accounting;first=tmp_path/'first.csv';second=tmp_path/'second.csv'
    original=response(owned(payload['source_day'],first),0,'accounted')
    before=all_state(conn)
    replay=response(owned(payload['source_day'],second),0,'accounted')
    assert replay['replayed'] is True and original['replayed'] is False
    assert_snapshot(conn,second,replay)
    assert first.read_bytes()==second.read_bytes() and all_state(conn)==before


@pytest.mark.parametrize('desk',['futures'],indirect=True)
def test_existing_file_refuses_before_database_and_preserves_content(accounting,tmp_path):
    conn,payload=accounting;output=tmp_path/'existing.csv';output.write_bytes(b'preserve-me\n')
    before=all_state(conn)
    response(owned(payload['source_day'],output),2,'invalid_arguments')
    assert output.read_bytes()==b'preserve-me\n' and all_state(conn)==before


@pytest.mark.parametrize('desk',['futures'],indirect=True)
def test_symlink_target_refuses_before_database(accounting,tmp_path):
    conn,payload=accounting;real=tmp_path/'real.csv';real.write_bytes(b'preserve-me\n')
    output=tmp_path/'link.csv';output.symlink_to(real)
    before=all_state(conn)
    response(owned(payload['source_day'],output),2,'invalid_arguments')
    assert real.read_bytes()==b'preserve-me\n' and output.is_symlink() and all_state(conn)==before


@pytest.mark.parametrize('desk',['futures'],indirect=True)
@pytest.mark.parametrize('accounting',['missing_price'],indirect=True)
def test_no_csv_or_receipt_on_actual_accounting_refusal(accounting,tmp_path):
    conn,payload=accounting;output=tmp_path/'refused.csv';before=all_state(conn)
    response(owned(payload['source_day'],output),5,'accounting_refused')
    assert not output.exists() and all_state(conn)==before


@pytest.mark.parametrize('path', ['', 'relative.csv', '/tmp/../out.csv', '/tmp//out.csv',
    '/tmp/out.txt', '/tmp/out.csv/', '/tmp/with space.csv'])
def test_output_path_refusal_precedes_connection_read(path):
    # Descriptor 3 is deliberately absent; path admission must win before DB.
    response(invoke(args()+['--connection-fd','3','--csv-output',path]),2,'invalid_arguments')


def test_symlink_parent_refuses_before_connection_read(tmp_path):
    real=tmp_path/'real';real.mkdir();alias=tmp_path/'alias';alias.symlink_to(real,target_is_directory=True)
    response(invoke(args()+['--connection-fd','3','--csv-output',str(alias/'new.csv')]),2,'invalid_arguments')
    assert not (real/'new.csv').exists()


@pytest.mark.parametrize('desk',['futures'],indirect=True)
@pytest.mark.parametrize('race',['target_appears','parent_replaced'])
def test_postcommit_writer_refusal_preserves_accounted_receipt(accounting,tmp_path,race):
    # The actual processor's canonical book lock supplies a deterministic point
    # AFTER CLI path admission. No production hook or synthetic receipt is used.
    conn,payload=accounting
    assert conn.autocommit
    parent=tmp_path/'admitted';parent.mkdir();output=parent/'race.csv'
    moved=tmp_path/'moved'
    dsn=os.environ['ALGOLENS_TEST_DB']
    assert dsn.startswith('host=/tmp/algolens-repair-pg-') and 'dbname=algolens_test_' in dsn
    lock_sql="hashtextextended('algolens:qt-book:BOOK',0)"
    process=None;locked=False
    with tempfile.TemporaryFile() as material:
        material.write(dsn.encode());material.flush()
        fd=os.open(f'/proc/self/fd/{material.fileno()}',os.O_RDONLY)
        try:
            with conn.cursor() as cur:
                cur.execute('SELECT pg_advisory_lock('+lock_sql+')');locked=True
            process=subprocess.Popen([str(BINARY),*args(payload['source_day']),
                '--csv-output',str(output),'--connection-fd',str(fd)],
                stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,pass_fds=(fd,),
                env=dict(PATH='/usr/local/bin:/usr/bin:/bin',
                    LD_PRELOAD='/home/devcontainers/qt-validation-20260921/bin/Debug/libqt_no_delivery_guard.so'))
            deadline=time.monotonic()+8
            while True:
                assert process.poll() is None,'actual CLI exited before canonical lock barrier'
                with conn.cursor() as cur:
                    cur.execute("SELECT count(*) FROM pg_catalog.pg_stat_activity WHERE datname=current_database()"
                        " AND pid<>pg_backend_pid() AND wait_event_type='Lock' AND wait_event='advisory'"
                        " AND position('algolens:qt-book:' in query)>0")
                    waiting=cur.fetchone()[0]
                if waiting==1:break
                assert time.monotonic()<deadline,'actual native canonical lock barrier not reached'
                time.sleep(0.025)
            assert not output.exists()
            if race=='target_appears':
                with output.open('xb') as target:target.write(b'preserve-racing-owner\n')
            else:
                parent.rename(moved);parent.mkdir()
            with conn.cursor() as cur:
                cur.execute('SELECT pg_advisory_unlock('+lock_sql+')');assert cur.fetchone()[0];locked=False
            stdout,stderr=process.communicate(timeout=15)
            result=subprocess.CompletedProcess(process.args,process.returncode,stdout,stderr)
            answer=response(result,7,'accounted')
            assert answer['csv_status']=='refused' and answer['replayed'] is False
            assert 'csv_sha256' not in answer
            with conn.cursor() as cur:
                cur.execute('SELECT status,attempt_id::text,publication_payload FROM trading.qt_desk_receipts WHERE decision_id=%s',(DECISION,))
                status,attempt,receipt=cur.fetchone()
                assert status=='processed' and attempt==ATTEMPT and receipt['observation_id']==INPUT
                cur.execute("SELECT count(*) FROM trading.executions WHERE portfolio_type='qt'")
                assert cur.fetchone()[0]==2
                cur.execute('SELECT count(*) FROM trading.desk_run_results WHERE decision_id=%s',(DECISION,))
                assert cur.fetchone()[0]==1
            # The postcommit file error cannot undo or duplicate accounting.
            from test_qt_desk_accounting import run
            before=all_state(conn);replay=run()
            assert replay.returncode==0 and 'REPLAYED=1' in replay.stdout
            assert all_state(conn)==before
            if race=='target_appears':
                assert output.read_bytes()==b'preserve-racing-owner\n'
                assert list(parent.iterdir())==[output]
            else:
                assert not output.exists() and not (moved/'race.csv').exists()
                assert list(parent.iterdir())==[] and list(moved.iterdir())==[]
        finally:
            if locked:
                with conn.cursor() as cur:cur.execute('SELECT pg_advisory_unlock('+lock_sql+')')
            if process is not None and process.poll() is None:
                process.kill();process.communicate(timeout=5)
            os.close(fd)


@pytest.mark.parametrize('desk',['flat_zero_basis'],indirect=True)
@pytest.mark.parametrize('equity',['flat_zero_basis'],indirect=True)
def test_actual_flat_equity_zero_basis_receipt_preserved_in_csv(equity,tmp_path):
    conn,payload=equity;output=tmp_path/'flat-zero-basis.csv'
    result=owned(payload['source_day'],output)
    # Establish the actual committed producer output before asserting CSV success.
    # The current writer refuses this valid output; accounting must still survive.
    with conn.cursor() as cur:
        cur.execute('SELECT publication_payload FROM trading.qt_desk_receipts WHERE decision_id=%s',(DECISION,))
        receipt=cur.fetchone()[0]
        flat=next(row for row in receipt['after_accounting'] if row['key']['strategy_name']=='synthetic-beta')
        assert flat['quantity_exact']=='0' and flat['average_price_exact']=='0'
        cur.execute("SELECT quantity,average_price FROM trading.positions WHERE portfolio_id='BOOK' AND portfolio_type='qt' AND date=%s AND strategy_name='synthetic-beta'",(payload['source_day'],))
        assert cur.fetchone()==(0,0)
        cur.execute("SELECT count(*) FROM trading.executions WHERE portfolio_type='qt'")
        assert cur.fetchone()[0]==0
    answer=response(result,0,'accounted')
    assert_snapshot(conn,output,answer)
    rows=list(csv.reader(line for line in output.read_text().splitlines() if not line.startswith('#')))
    metadata=next(line for line in output.read_text().splitlines() if line.startswith('# QT desk snapshot: '))
    manifest=json.loads(metadata.removeprefix('# QT desk snapshot: '))
    beta=next(row for row in manifest['rows'] if row['key']['strategy_name']=='synthetic-beta')
    flat_csv=next(row for row in rows[1:] if row[:2]==[beta['csv_strategy'],beta['key']['symbol']])
    assert flat_csv[2]=='0' and flat_csv[4]=='0'
