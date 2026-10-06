"""Governed config storage on the explicitly owned disposable PostgreSQL only.

Catches mutable audit, stale winner overwrites, cross-scope versions, privilege
expansion, and native fail-open selection/admission. Never hashes JSON in Python.
"""
import json
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import psycopg2
import pytest
from tests.qt_test_artifacts import artifact, build_identity
from test_runtime_control_schema import connection

ROOT = Path(__file__).resolve().parents[2]
MIGRATION = ROOT / 'migrations/031_live_config_overrides.sql'
ROLLBACK = ROOT / 'migrations/031_live_config_overrides_rollback.sql'

@pytest.fixture
def configured(connection):
    if MIGRATION.exists():
        with connection.cursor() as cur:
            # Roles are created only inside this owned cluster, never by migration031.
            for role in ('qt_system_publisher','qt_algolens_api'):
                cur.execute('SELECT 1 FROM pg_roles WHERE rolname=%s',(role,))
                if cur.fetchone() is None: cur.execute(f'CREATE ROLE {role} NOLOGIN')
            cur.execute(MIGRATION.read_text())
    return connection


def candidate(conn, *, previous=None, book='BOOK', revision=0, base=None, changes=None, operation='override'):
    if base is None:
        base = native('validate_baseline' if operation == 'reset_to_baseline' else
                      'validate' if book == 'BOOK' else 'validate_other')
    if changes is None:
        changes = {} if operation == 'reset_to_baseline' else {'/optimization/cost_penalty_scalar':12.75}
    with conn.cursor() as cur:
        cur.execute('''INSERT INTO trading.live_config_versions
          (registry_id,portfolio_id,engine_strategy_id,registry_revision,validator_build,validator_sha256,
           base_sha256,effective_sha256,changes,effective_snapshot,previous_version_id,
           submitted_by,submitter_person_id,reason,operation)
          VALUES ('trend',%s,'LIVE_TREND',%s,%s,repeat('a',64),%s,%s,%s::jsonb,%s::jsonb,%s,
                  'submit-user','submit-person','tune optimizer',%s) RETURNING version_id::text''',
          (book,revision,build_identity(),base['base_sha256'],base['effective_sha256'],
           json.dumps(changes),json.dumps(base['effective_snapshot']), previous,operation))
        return cur.fetchone()[0]


def activate(conn, version, *, actor='approve-user', person='approve-person'):
    with conn.cursor() as cur:
        cur.execute('''INSERT INTO trading.live_config_activations
          (version_id,approved_by,approver_person_id,authority_versions,reason)
          VALUES (%s,%s,%s,'{"submit_grant":1,"approve_grant":2,"mapping":3}'::jsonb,'reviewed')''',
          (version,actor,person))


def native(mode, *, expected=0, stdin=None):
    path = artifact('live_config_selection_probe')
    assert path.exists(), 'native selection probe has not been built'
    result = subprocess.run([str(path),mode], input=stdin, text=True, capture_output=True, timeout=30)
    assert result.returncode == expected, (result.returncode,result.stdout,result.stderr)
    return json.loads(result.stdout.splitlines()[-1])


def test_no_row_selects_file_and_records_immutable_admission(configured):
    result = native('admit')
    assert result['receipt']['source'] == 'file'
    assert result['receipt']['version_id'] is None
    assert result['receipt']['base_sha256'] == result['receipt']['effective_sha256']
    with configured.cursor() as cur:
        cur.execute('SELECT selection,config_snapshot FROM trading.live_config_attempt_selections')
        rows = cur.fetchall()
        assert len(rows) == 1 and rows[0][0] == result['receipt']
        assert rows[0][1]['execution']['position_limit_live'] == 12.75
        with pytest.raises(psycopg2.Error, match='immutable'):
            cur.execute("UPDATE trading.live_config_attempt_selections SET selection='{}'")


def test_missing_schema_refuses_instead_of_file_fallback(connection):
    result = native('select',expected=2)
    assert result['error'] == 'live_config_selection_refused'


def test_storage_contract_exists(configured):
    with configured.cursor() as cur:
        cur.execute("SELECT to_regclass('trading.live_config_versions')")
        assert cur.fetchone()[0] is not None, 'governed candidate storage is missing'


def test_migration_reapply_and_guarded_rollback(configured):
    with configured.cursor() as cur:
        cur.execute(MIGRATION.read_text())
        cur.execute(ROLLBACK.read_text())
        cur.execute("SELECT to_regclass('trading.live_config_versions')")
        assert cur.fetchone()[0] is None
        cur.execute(MIGRATION.read_text())
    candidate(configured)
    with configured.cursor() as cur:
        with pytest.raises(psycopg2.Error,match='live_config_history_exists'):
            cur.execute(ROLLBACK.read_text())
        cur.execute('ROLLBACK')


def test_candidates_and_activations_are_immutable_and_distinct(configured):
    version = candidate(configured)
    with configured.cursor() as cur:
        for sql in ("UPDATE trading.live_config_versions SET reason='replacement'",'DELETE FROM trading.live_config_versions'):
            with pytest.raises(psycopg2.Error,match='immutable'):
                cur.execute(sql)
    with pytest.raises(psycopg2.Error,match='distinct'):
        activate(configured,version,person='submit-person')
    with pytest.raises(psycopg2.Error,match='distinct'):
        activate(configured,version,actor='submit-user')
    activate(configured,version)
    with configured.cursor() as cur:
        with pytest.raises(psycopg2.Error,match='immutable'):
            cur.execute('DELETE FROM trading.live_config_activations')
    out = native('select')
    assert out['receipt']['version_id'] == version
    assert out['receipt']['source'] == 'approved_override'
    assert out['cost_penalty_scalar'] == 12.75
    assert out['private_preserved'] is True
    assert native('admit',expected=2)['error'] == 'runtime_start_refused'


@pytest.mark.parametrize('mutation',['base','snapshot','changes','build','revision'])
def test_invalid_or_stale_active_candidate_refuses(configured, mutation):
    data = native('validate')
    if mutation == 'base': data['base_sha256'] = '0'*64
    if mutation == 'snapshot': data['effective_snapshot']['optimization']['tau'] = 1.5
    changes = {'/portfolio_id':'OTHER'} if mutation == 'changes' else None
    version = candidate(configured,base=data,changes=changes)
    activate(configured,version)
    if mutation == 'revision':
        with configured.cursor() as cur: cur.execute("UPDATE trading.strategy_registry SET runtime_revision=1 WHERE id='trend'")
    result = native('select_other_build' if mutation == 'build' else 'select', expected=2)
    assert result['error'] == 'live_config_selection_refused'


@pytest.mark.parametrize('operation',['override','reset_to_baseline'])
def test_two_activations_have_one_winner(configured,operation):
    previous=None
    if operation=='reset_to_baseline':
        previous=candidate(configured); activate(configured,previous)
    versions = [candidate(configured,operation=operation,previous=previous) for _ in range(2)]
    def run(version):
        conn = psycopg2.connect(os.environ['ALGOLENS_TEST_DB']); conn.autocommit=True
        try:
            activate(conn,version)
            return 'ok'
        except psycopg2.Error as error:
            assert 'live_config_active_changed' in str(error)
            return 'stale'
        finally: conn.close()
    with ThreadPoolExecutor(2) as pool: outcomes = list(pool.map(run,versions))
    assert sorted(outcomes) == ['ok','stale']
    with configured.cursor() as cur:
        cur.execute('SELECT count(*) FROM trading.live_config_active')
        assert cur.fetchone()[0] == 1
        cur.execute('SELECT count(*) FROM trading.live_config_activations')
        assert cur.fetchone()[0] == (2 if operation=='reset_to_baseline' else 1)


def test_other_book_override_is_not_selected(configured):
    with configured.cursor() as cur:
        cur.execute("INSERT INTO trading.strategy_book_memberships VALUES ('trend','OTHER')")
    version = candidate(configured,book='OTHER',revision=1)
    activate(configured,version)
    assert native('select')['receipt']['source'] == 'file'


def test_activation_between_selection_and_admission_refuses(configured):
    selected = native('select')
    version = candidate(configured); activate(configured,version)
    result = native('admit_receipt',stdin=json.dumps(selected['receipt']),expected=2)
    assert result['error'] == 'runtime_start_refused'
    with configured.cursor() as cur:
        cur.execute('SELECT count(*) FROM trading.live_config_attempt_selections')
        assert cur.fetchone()[0] == 0


def test_database_rejects_malformed_admission_receipt(configured):
    with configured.cursor() as cur:
        with pytest.raises(psycopg2.Error,match='live_config_receipt_invalid'):
            cur.execute('''INSERT INTO trading.live_config_attempt_selections
              (attempt_id,portfolio_id,engine_strategy_id,run_date,engine_build,selection,config_snapshot)
              VALUES ('forged','BOOK','LIVE_TREND','2026-10-06','test','{}','{}')''')


def test_legacy_file_weights_remain_supported(configured):
    assert native('legacy')['receipt']['source'] == 'file'


def test_controlled_override_binds_exact_approved_snapshot(configured):
    validated=native('validate')
    version=candidate(configured,base=validated); activate(configured,version)
    with configured.cursor() as cur:
        cur.execute('''INSERT INTO trading.runtime_intents
          (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,config_snapshot,
           status,requested_by,request_reason,approved_by,approval_reason,approved_at)
          VALUES ('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,'approved','exec-submit',
                  'run','exec-approver','approved',now())''',(json.dumps(validated['effective_snapshot']),))
    selected=native('admit_controlled')
    assert selected['receipt']['version_id']==version
    with configured.cursor() as cur:
        cur.execute('''SELECT s.selection->>'source',s.config_snapshot=r.config_snapshot,
          s.attempt_id=r.id,r.status FROM trading.live_config_attempt_selections s
          JOIN trading.runtime_attempts r ON r.id=s.attempt_id''')
        assert cur.fetchall()==[('approved_override',True,True,'failed')]


def test_publisher_cannot_activate_and_api_cannot_admit(configured):
    with configured.cursor() as cur:
        for role in ('qt_system_publisher','qt_algolens_api'):
            cur.execute('SELECT 1 FROM pg_roles WHERE rolname=%s',(role,))
            if cur.fetchone() is None: cur.execute(f'CREATE ROLE {role} NOLOGIN')
        cur.execute(MIGRATION.read_text())
        cur.execute('GRANT USAGE ON SCHEMA trading TO qt_system_publisher,qt_algolens_api')
    version=candidate(configured)
    with configured.cursor() as cur:
        cur.execute('SET ROLE qt_system_publisher')
        try:
            cur.execute('SELECT count(*) FROM trading.live_config_active')
            assert cur.fetchone()[0]==0
            with pytest.raises(psycopg2.errors.InsufficientPrivilege): activate(configured,version)
            with pytest.raises(psycopg2.errors.InsufficientPrivilege):
                cur.execute("DELETE FROM trading.live_config_active")
        finally: cur.execute('RESET ROLE')
        cur.execute('GRANT SELECT ON trading.strategy_registry,trading.strategy_book_memberships TO qt_algolens_api')
        cur.execute('GRANT UPDATE(runtime_revision) ON trading.strategy_registry TO qt_algolens_api')
        cur.execute('SET ROLE qt_algolens_api')
        try:
            activate(configured,version)
            with pytest.raises(psycopg2.errors.InsufficientPrivilege):
                cur.execute("INSERT INTO trading.live_config_attempt_selections DEFAULT VALUES")
            with pytest.raises(psycopg2.errors.InsufficientPrivilege):
                cur.execute("UPDATE trading.live_config_attempt_safety SET state='published'")
        finally: cur.execute('RESET ROLE')


def test_investor_file_selection_records_identity(configured):
    with configured.cursor() as cur:
        cur.execute('''CREATE TABLE trading.investor_books (book_id uuid DEFAULT gen_random_uuid(),
          portfolio_id text PRIMARY KEY,is_active boolean,model_stream text,opening_date date);
          CREATE TABLE trading.investor_book_strategies(portfolio_id text,strategy_id text);
          CREATE TABLE trading.investor_book_publications(portfolio_id text,source_day date,
            strategy_id text,model_stream text,producer_version text,content_digest text);
          CREATE FUNCTION trading.compute_system_investor_digest(text,text,date) RETURNS text
            LANGUAGE sql AS 'SELECT ''test''::text';
          INSERT INTO trading.investor_books(portfolio_id,is_active,model_stream,opening_date)
            VALUES ('INVESTOR',true,'system','2026-10-01');
          INSERT INTO trading.investor_book_strategies VALUES ('INVESTOR','LIVE_TREND');''')
    out=native('investor')
    assert out['receipt']['source']=='file'
    assert out['receipt']['scope']['registry_id'] is None
    assert out['receipt']['scope']['investor_book_id'] is not None
    with configured.cursor() as cur:
        cur.execute('SELECT portfolio_id FROM trading.live_config_attempt_selections')
        assert cur.fetchall()==[('INVESTOR',)]


def test_candidate_snapshot_cannot_claim_another_book(configured):
    with configured.cursor() as cur:
        cur.execute("INSERT INTO trading.strategy_book_memberships VALUES ('trend','OTHER')")
    with pytest.raises(psycopg2.Error,match='live_config_snapshot_scope_invalid'):
        candidate(configured,book='OTHER',revision=1,base=native('validate'))


def test_ambiguous_source_refuses_selection(configured):
    with configured.cursor() as cur:
        cur.execute("INSERT INTO trading.strategy_registry(id,strategy_type,portfolio_id) VALUES ('alias','LIVE_TREND','BOOK')")
    assert native('select',expected=2)['error']=='live_config_selection_refused'


def test_absent_active_pointer_is_serialized_by_scope_lock(configured):
    version=candidate(configured)
    locked=psycopg2.connect(os.environ['ALGOLENS_TEST_DB'])
    contender=psycopg2.connect(os.environ['ALGOLENS_TEST_DB']); contender.autocommit=True
    try:
        with locked.cursor() as cur: cur.execute("SELECT trading.lock_live_config_scope('LIVE_TREND','BOOK')")
        with contender.cursor() as cur: cur.execute("SET lock_timeout='150ms'")
        with pytest.raises(psycopg2.errors.LockNotAvailable): activate(contender,version)
        locked.rollback()
        activate(contender,version)
    finally: locked.close(); contender.close()


def test_native_publisher_can_admit_but_not_fabricate_safety(configured,monkeypatch):
    with configured.cursor() as cur:
        cur.execute('GRANT USAGE ON SCHEMA trading TO qt_system_publisher')
        cur.execute('GRANT SELECT ON trading.strategy_registry,trading.strategy_book_memberships TO qt_system_publisher')
        cur.execute('GRANT UPDATE(runtime_revision) ON trading.strategy_registry TO qt_system_publisher')
        cur.execute('SET ROLE qt_system_publisher')
        try:
            with pytest.raises(psycopg2.errors.InsufficientPrivilege):
                cur.execute("INSERT INTO trading.live_config_attempt_safety(attempt_id,state) VALUES ('fake','published')")
        finally: cur.execute('RESET ROLE')
    monkeypatch.setenv('ALGOLENS_TEST_DB',os.environ['ALGOLENS_TEST_DB']+" options='-c role=qt_system_publisher'")
    assert native('admit')['receipt']['source']=='file'


def test_next_activation_preserves_prior_version_and_requires_expected_pointer(configured):
    first=candidate(configured); activate(configured,first)
    with pytest.raises(psycopg2.Error,match='live_config_active_changed'):
        candidate(configured)
    second=candidate(configured,previous=first); activate(configured,second)
    assert native('select')['receipt']['version_id']==second
    with configured.cursor() as cur:
        cur.execute('SELECT version_id::text FROM trading.live_config_versions ORDER BY submitted_at')
        assert cur.fetchall()==[(first,),(second,)]
        cur.execute('SELECT count(*) FROM trading.live_config_active')
        assert cur.fetchone()[0]==1
        cur.execute('SELECT count(*) FROM trading.live_config_activations')
        assert cur.fetchone()[0]==2


def test_reapply_removes_accidental_publisher_activation_grants(configured):
    version=candidate(configured)
    with configured.cursor() as cur:
        cur.execute('GRANT INSERT ON trading.live_config_activations TO qt_system_publisher')
        cur.execute('GRANT USAGE ON SCHEMA trading TO qt_system_publisher')
        cur.execute(MIGRATION.read_text())
        cur.execute('SET ROLE qt_system_publisher')
        try:
            with pytest.raises(psycopg2.errors.InsufficientPrivilege): activate(configured,version)
        finally: cur.execute('RESET ROLE')



def test_multi_sleeve_equity_uses_exact_runner_identity(configured):
    with configured.cursor() as cur:
        cur.execute("""INSERT INTO trading.strategy_registry(id,strategy_type,portfolio_id)
          VALUES ('equity-legacy','LIVE_EQUITY_MEAN_REVERSION','EQUITYBOOK'),
                 ('equity-multi','LIVE_EQUITY_ALPHA_BETA','EQUITYBOOK')""")
    receipt=native('equity_multi')['receipt']
    assert receipt['scope']['registry_id']=='equity-multi'
    assert receipt['scope']['engine_strategy_id']=='LIVE_EQUITY_ALPHA_BETA'
    assert receipt['scope']['portfolio_id']=='EQUITYBOOK'
    assert receipt['source']=='file'


def test_candidates_declare_reset_semantics(configured):
    version=candidate(configured)
    with configured.cursor() as cur:
        cur.execute('SELECT to_jsonb(v)->>\'operation\' FROM trading.live_config_versions v WHERE version_id=%s',(version,))
        assert cur.fetchone()[0]=='override'



def test_approved_reset_restores_baseline_requires_fresh_execution_approval(configured):
    changed=native('validate')
    first=candidate(configured,base=changed); activate(configured,first)
    def intent(snapshot):
        with configured.cursor() as cur:
            cur.execute("""INSERT INTO trading.runtime_intents
              (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,config_snapshot,
               status,requested_by,request_reason,approved_by,approval_reason,approved_at)
              VALUES ('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,'approved','exec-submit',
                      'run','exec-approver','approved',now())""",(json.dumps(snapshot),))
    intent(changed['effective_snapshot'])
    baseline=native('validate_baseline')
    reset=candidate(configured,operation='reset_to_baseline',previous=first,base=baseline)
    with pytest.raises(psycopg2.Error,match='distinct'):
        activate(configured,reset,person='submit-person')
    assert native('select')['receipt']['version_id']==first
    activate(configured,reset)
    selected=native('select')
    assert selected['cost_penalty_scalar']==50.0
    assert selected['private_preserved'] is True
    assert selected['receipt']['source']=='approved_override'
    assert selected['receipt']['version_id']==reset
    assert selected['receipt']['base_sha256']==selected['receipt']['effective_sha256']
    assert native('admit',expected=2)['error']=='runtime_start_refused'
    assert native('admit_controlled',expected=2)['error']=='runtime_start_refused'
    with configured.cursor() as cur:
        cur.execute("UPDATE trading.runtime_intents SET status='superseded' WHERE status='approved'")
    intent(baseline['effective_snapshot'])
    assert native('admit_controlled')['receipt']['version_id']==reset
    with configured.cursor() as cur:
        cur.execute('SELECT count(*) FROM trading.live_config_activations')
        assert cur.fetchone()[0]==2
        cur.execute('SELECT count(*) FROM trading.live_config_active')
        assert cur.fetchone()[0]==1


@pytest.mark.parametrize('mode',['select_changed_base','select_other_build'])
def test_approved_reset_is_stale_when_files_or_build_change(configured,mode):
    first=candidate(configured); activate(configured,first)
    reset=candidate(configured,operation='reset_to_baseline',previous=first); activate(configured,reset)
    assert native(mode,expected=2)['error']=='live_config_selection_refused'


@pytest.mark.parametrize('malformed',['no_previous','changes','unequal_hash'])
def test_reset_candidate_requires_previous_empty_changes_and_equal_hashes(configured,malformed):
    previous=None
    if malformed!='no_previous':
        previous=candidate(configured); activate(configured,previous)
    base=native('validate_baseline')
    changes={}
    if malformed=='changes': changes={'/optimization/tau':1.1}
    if malformed=='unequal_hash': base['effective_sha256']='0'*64
    with pytest.raises(psycopg2.errors.CheckViolation):
        candidate(configured,operation='reset_to_baseline',previous=previous,base=base,changes=changes)


@pytest.mark.parametrize('mismatch',['date','build','revision'])
def test_controlled_receipt_cannot_relabel_attempt_identity(configured,mismatch):
    approved=native('validate')
    version=candidate(configured,base=approved); activate(configured,version)
    receipt=native('select')['receipt']
    with configured.cursor() as cur:
        cur.execute('''INSERT INTO trading.runtime_intents
          (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,config_snapshot,
           status,requested_by,request_reason,approved_by,approval_reason,approved_at)
          VALUES ('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,'approved','exec-submit',
                  'run','exec-approver','approved',now()) RETURNING id''',
          (json.dumps(approved['effective_snapshot']),))
        intent=cur.fetchone()[0]
        cur.execute('''INSERT INTO trading.runtime_attempts
          (id,intent_id,registry_revision,config_snapshot,run_date,producer_version,status)
          VALUES ('wrong-date',%s,%s,%s::jsonb,%s::date,%s,'running')''',
          (intent,1 if mismatch=='revision' else 0,json.dumps(approved['effective_snapshot']),
           '2026-10-05' if mismatch=='date' else '2026-10-06',
           'other-build' if mismatch=='build' else build_identity()))
        with pytest.raises(psycopg2.Error,match='live_config_admission_refused'):
            cur.execute('''INSERT INTO trading.live_config_attempt_selections
              (attempt_id,portfolio_id,engine_strategy_id,run_date,engine_build,selection,config_snapshot)
              VALUES ('wrong-date','BOOK','LIVE_TREND','2026-10-06',%s,%s::jsonb,%s::jsonb)''',
              (build_identity(),json.dumps(receipt),json.dumps(approved['effective_snapshot'])))
