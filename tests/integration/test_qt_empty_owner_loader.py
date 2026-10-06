"""Actual native historical loader tests after actual typed publication.

These do not establish current financial/universe readiness or run MODEL.
"""
import ctypes,json,os,subprocess
from copy import deepcopy
from pathlib import Path
import pytest
from psycopg2.extras import Json
import test_qt_empty_owner_publication as writer
from test_qt_empty_owner_publication import publisher,empty_schema,predecessor,connection,snapshot
PROBE=writer.PROBE.parent/'qt_empty_owner_loader_probe'


def resolve(mode,value):
    assert PROBE.is_file() and not PROBE.is_symlink()
    guard=writer.GUARD;assert guard.is_file() and not guard.is_symlink()
    assert ctypes.CDLL(str(guard)).qt_no_delivery_guard_loaded()==1
    env=os.environ.copy();env.update(LD_PRELOAD=str(guard),QT_EMAIL_DELIVERY_ENABLED='false')
    r=subprocess.run([str(PROBE),mode,value],capture_output=True,text=True,env=env,timeout=30)
    assert r.returncode==0,(r.returncode,r.stdout[-2000:],r.stderr[-500:])
    return r.stdout


def published(conn):
    assert 'EMPTY_PUBLICATION_COMMITTED=1' in writer.invoke('controlled').stdout
    with conn.cursor() as cur:
        cur.execute('SELECT to_jsonb(p) FROM trading.qt_empty_model_owner_publications p')
        rows=cur.fetchall();assert len(rows)==1
        return rows[0][0]


def test_actual_archive_returns_explicit_v2_reference_and_current_union_head(publisher):
    conn,_=publisher;p=published(conn);before=snapshot(conn)
    record=json.loads(resolve('record',p['publication_id']).split('EMPTY_ARCHIVE=',1)[1])
    scope=json.loads(resolve('scope',writer.DAY).split('EMPTY_ARCHIVE=',1)[1])
    assert record['kind']=='empty_owner_v2'
    ref=record['reference'];assert ref['schema_version']=='qt-empty-model-owner-reference/v2'
    assert len(ref)==13 and ref['configuration_digest']==p['configuration_digest'] and ref['qt_digest']==p['qt_digest']
    assert scope==dict(references=[ref],latest_publication_id=p['publication_id'])
    assert snapshot(conn)==before


def test_historical_original_authority_survives_current_registry_and_metadata_rotation(publisher):
    conn,_=publisher;p=published(conn)
    original=resolve('record',p['publication_id']);assert 'EMPTY_ARCHIVE=' in original
    with conn.cursor() as cur:
        cur.execute("UPDATE trading.live_run_metadata SET portfolio_config='{}'::jsonb WHERE portfolio_id=%s",(writer.BOOK,))
        cur.execute("UPDATE trading.strategy_registry SET is_active=false WHERE id='eq-owner'")
    before=snapshot(conn)
    assert resolve('record',p['publication_id'])==original
    assert snapshot(conn)==before


def test_absent_archive_is_refused_without_mutations(publisher):
    conn,_=publisher;before=snapshot(conn)
    assert 'EMPTY_ARCHIVE_REFUSED=1' in resolve('record','00000000-0000-4000-8000-000000000001')
    assert snapshot(conn)==before


@pytest.mark.parametrize('change',['configuration_digest','extra_capture_field','false_complete','failed_stage','foreign_run_owner','uncontrolled_revision','forged_skip_reason'])
def test_self_consistent_sql_identity_does_not_replace_archived_native_evidence(publisher,change):
    conn,_=publisher;p=published(conn);clone=deepcopy(p)
    clone.update(publication_id='00000000-0000-4000-8000-000000000010',attempt_id=None,publication_version=p['publication_version']+1)
    ident=clone['inspection_capture']['identity'];ident.update(capture_id=clone['publication_id'],publication_id=clone['publication_id'],runtime_attempt_id=None,control_mode='uncontrolled')
    if change=='configuration_digest':clone['configuration_digest']='0'*64
    elif change=='extra_capture_field':clone['inspection_capture']['invented']=True
    elif change=='false_complete':clone['inspection_capture']['equity_run_consumption']['complete']=False
    elif change=='failed_stage':clone['inspection_capture']['equity_run_consumption']['stages']['setup']['outcome']='threw'
    elif change=='foreign_run_owner':clone['inspection_capture']['equity_run_consumption']['run_key']['strategy_name']='FOREIGN'
    elif change=='forged_skip_reason':
        clone['registry_revision']=0;ident['registry_revision']=0
        stages=clone['inspection_capture']['equity_run_consumption']['stages']
        assert all(s['outcome']=='returned_ok' and s['skip_reason'] is None for s in stages.values())
        for stage in stages.values():stage['skip_reason']='none'
    else:clone['registry_revision']=1;ident['registry_revision']=1
    # The fixture explicitly inserts a malicious candidate, never a trusted
    # publication flag. The real native historical loader must reject it.
    names=list(clone)
    json_columns={'configured_owner_names','configuration_snapshot','fresh_empty_batches','inspection_capture','system_components','proposal_components','qt_components'}
    with conn.cursor() as cur:
        cur.execute('INSERT INTO trading.qt_empty_model_owner_publications('+','.join(names)+') VALUES('+','.join(['%s']*len(names))+')',[Json(clone[n]) if n in json_columns else clone[n] for n in names])
    before=snapshot(conn)
    assert 'EMPTY_ARCHIVE_REFUSED=1' in resolve('record',clone['publication_id'])
    assert 'EMPTY_ARCHIVE_REFUSED=1' in resolve('scope',writer.DAY)
    assert snapshot(conn)==before


@pytest.mark.parametrize('oversized',[False,True])
def test_legacy_v1_row_budget_refuses_before_decode_and_preserves_small_reference(publisher,oversized):
    conn,_=publisher
    # Valid small v1 seed grammar, explicitly synthetic source row only. This
    # tests the existing reference reader, not actual MODEL/financial readiness.
    from test_qt_empty_owner_schema import insert,digest
    from hashlib import sha256
    ident='00000000-0000-4000-8000-000000000020'
    with conn.cursor() as cur:
        insert(cur,1,publication=ident,attempt=None,book=writer.BOOK)
        if oversized:
            # Immutable table forbids UPDATE. Replace the not-yet-inserted seed
            # in a second distinct publication with one oversized source operand.
            ident='00000000-0000-4000-8000-000000000021'
            rows=[{'oversized_invalid_source':'x'*(8388608+1)}]
            cur.execute("INSERT INTO trading.qt_model_seed_publications(publication_id,portfolio_id,strategy_id,source_day,publication_version,system_components,seed_digest,proposal_components,proposal_manifest_digest,producer_version) VALUES(%s,%s,%s,%s,2,%s,%s,'[]',%s,'synthetic-budget-only')",(ident,writer.BOOK,writer.ENGINE,writer.DAY,Json(rows),digest({'seed_rows':rows}),digest({'proposal_rows':[]})))
    before=snapshot(conn)
    observed=resolve('record',ident)
    if oversized:assert 'EMPTY_ARCHIVE_REFUSED=1' in observed
    else:
        record=json.loads(observed.split('EMPTY_ARCHIVE=',1)[1]);assert record['kind']=='legacy_v1'
        with conn.cursor() as cur:
            cur.execute("SELECT publication_id::text,strategy_id,publication_version,seed_digest,proposal_manifest_digest,producer_version FROM trading.qt_model_seed_publications WHERE publication_id=%s",(ident,))
            row=cur.fetchone()
        assert record['reference']==dict(zip(('publication_id','strategy_id','publication_version','seed_digest','proposal_manifest_digest','producer_version'),row))
        assert len(record['reference'])==6
    assert snapshot(conn)==before
