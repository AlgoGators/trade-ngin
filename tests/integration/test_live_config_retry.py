"""Owned PostgreSQL failure boundaries for frozen governed configurations."""
import json
import psycopg2
import pytest
from test_live_config_overrides import configured, native
from test_runtime_control_schema import connection


def attempt(conn):
    native('admit')
    with conn.cursor() as cur:
        cur.execute('SELECT attempt_id FROM trading.live_config_attempt_selections ORDER BY admitted_at DESC LIMIT 1')
        return cur.fetchone()[0]


def test_abandoned_file_attempt_is_terminal(configured):
    identity = attempt(configured)
    with configured.cursor() as cur:
        cur.execute('SELECT lifecycle,state FROM trading.live_config_attempt_safety WHERE attempt_id=%s', (identity,))
        assert cur.fetchone() == ('failed', 'clean')


def test_recovery_is_state_proof_not_boolean(configured):
    identity = attempt(configured)
    with configured.cursor() as cur:
        with pytest.raises(psycopg2.Error, match='config_attempt_not_running'):
            cur.execute('SELECT trading.mark_live_config_unsafe(%s)', (identity,))


def test_publisher_cannot_fabricate_recovery(configured):
    with configured.cursor() as cur:
        cur.execute("SELECT has_function_privilege('qt_system_publisher', 'trading.recover_live_config_attempt(text,text)', 'EXECUTE')")
        assert cur.fetchone()[0] is False


def latest(conn):
    with conn.cursor() as cur:
        cur.execute('SELECT attempt_id,lifecycle,state FROM trading.live_config_attempt_safety JOIN trading.live_config_attempt_selections USING(attempt_id) ORDER BY admitted_at DESC LIMIT 1')
        return cur.fetchone()


def recover(conn, identity):
    with conn.cursor() as cur:
        cur.execute('SELECT trading.recover_live_config_attempt(%s,%s)', (identity, 'verified restoration in owned test'))


def test_safe_failure_reselects_changed_file(configured):
    before = native('admit')['receipt']
    after = native('admit_changed_base')['receipt']
    assert before['effective_sha256'] != after['effective_sha256']
    assert latest(configured)[1:] == ('failed', 'clean')


def test_unresolved_attempt_refuses_without_process_death_guess(configured):
    native('admit_crash')
    identity, lifecycle, state = latest(configured)
    assert (lifecycle, state) == ('running', 'clean')
    assert native('admit_changed_base', expected=2)['error'] == 'config_attempt_unresolved'
    recover(configured, identity)
    native('admit_changed_base')


@pytest.mark.parametrize('mode', ['admit_unsafe', 'admit_write_fail'])
def test_marker_survives_successful_and_failed_financial_transaction(configured, mode):
    native(mode)
    identity, lifecycle, state = latest(configured)
    assert (lifecycle, state) == ('failed', 'unsafe')
    assert native('admit_changed_base', expected=2)['error'] == 'config_retry_recovery_required'
    recover(configured, identity)
    with configured.cursor() as cur:
        cur.execute('SELECT lifecycle,state,recovery_evidence FROM trading.live_config_attempt_safety WHERE attempt_id=%s', (identity,))
        lifecycle, state, evidence = cur.fetchone()
        assert (lifecycle, state) == ('aborted', 'recovered')
        assert evidence['schema'] == 'live-config-recovery/v1'
    native('admit_changed_base')


@pytest.mark.parametrize('table,insert,restore', [
    ('positions', "INSERT INTO trading.positions(strategy_id,portfolio_id,portfolio_type,quantity) VALUES('LIVE_TREND','BOOK','system',3)", "DELETE FROM trading.positions WHERE portfolio_id='BOOK'"),
    ('live_results', "INSERT INTO trading.live_results(strategy_id,portfolio_id,total_pnl) VALUES('LIVE_TREND','BOOK',3)", "DELETE FROM trading.live_results WHERE portfolio_id='BOOK'"),
    ('corp_action_applied', "INSERT INTO trading.corp_action_applied VALUES('LIVE_TREND','BOOK','split-1')", "DELETE FROM trading.corp_action_applied WHERE portfolio_id='BOOK'"),
])
def test_recovery_compares_persisted_financial_rows(configured, table, insert, restore):
    with configured.cursor() as cur:
        cur.execute('CREATE TABLE trading.corp_action_applied(strategy_id text,portfolio_id text,event text)')
    native('admit_unsafe')
    identity = latest(configured)[0]
    with configured.cursor() as cur:
        cur.execute(insert)
    with pytest.raises(psycopg2.Error, match='config_recovery_state_mismatch'):
        recover(configured, identity)
    assert latest(configured)[1:] == ('failed', 'unsafe')
    with configured.cursor() as cur:
        cur.execute(restore)
    recover(configured, identity)
    native('admit_changed_base')


def test_recovered_original_process_cannot_write_or_publish(configured):
    import subprocess
    from tests.qt_test_artifacts import artifact
    process = subprocess.Popen([str(artifact('live_config_selection_probe')), 'admit_wait'],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        while True:
            line = process.stdout.readline()
            assert line, process.stderr.read()
            if line.startswith('{') and 'receipt' in line: break
        identity = latest(configured)[0]
        recover(configured, identity)
        stdout, stderr = process.communicate('continue\n', timeout=20)
        assert process.returncode == 0, stderr
        result = json.loads(stdout.splitlines()[-1])
        assert result == {'write_refused': True, 'publish_refused': True}
        assert latest(configured)[1:] == ('aborted', 'clean')
    finally:
        if process.poll() is None:
            process.kill(); process.wait()


def test_publisher_cannot_seal_without_final_publication_evidence(configured):
    native('admit_crash')
    identity = latest(configured)[0]
    with configured.cursor() as cur:
        with pytest.raises(psycopg2.Error, match='config_publication_evidence_missing'):
            cur.execute("SELECT trading.finish_live_config_attempt(%s,'published',%s)", (identity,identity))
    assert latest(configured)[1:] == ('running','clean')


def test_same_effective_unsafe_retry_requires_recovery(configured):
    native('admit_unsafe')
    identity = latest(configured)[0]
    assert native('admit', expected=2)['error'] == 'config_retry_recovery_required'
    assert native('admit_changed_base', expected=2)['error'] == 'config_retry_recovery_required'
    recover(configured, identity)
    native('admit')
    native('admit_changed_base')


@pytest.mark.parametrize('role', ['qt_system_publisher','qt_algolens_api'])
@pytest.mark.parametrize('sql', [
    "UPDATE trading.live_config_attempt_safety SET state='recovered',lifecycle='aborted'",
    "UPDATE trading.live_config_attempt_safety SET pre_write_evidence='{}'",
    "SELECT trading.recover_live_config_attempt('nonexistent','pretend')",
])
def test_actual_restricted_role_cannot_bypass_recovery(configured,role,sql):
    with configured.cursor() as cur:
        cur.execute('GRANT USAGE ON SCHEMA trading TO '+role)
        cur.execute('SET ROLE '+role)
        try:
            with pytest.raises(psycopg2.errors.InsufficientPrivilege):cur.execute(sql)
        finally:cur.execute('RESET ROLE')


def test_final_publication_atomically_seals_selection(configured):
    import subprocess
    from tests.qt_test_artifacts import artifact
    from test_runtime_control_schema import prepare_exact_publication_schema
    prepare_exact_publication_schema(configured)
    result=subprocess.run([str(artifact('runtime_publication_probe')),'publish_required_complete_governed'],
                          capture_output=True,text=True,timeout=40)
    assert result.returncode==0,(result.stdout,result.stderr)
    payload=json.loads(next(line.split('=',1)[1] for line in result.stdout.splitlines() if line.startswith('CONFIG_PUBLICATION=')))
    assert payload['publication_schema_version']==4
    assert payload['supplied']['projection_version']==2
    with configured.cursor() as cur:
        cur.execute('SELECT a.attempt_id,s.lifecycle,s.state,s.publication_id,a.selection,a.config_snapshot FROM trading.live_config_attempt_selections a JOIN trading.live_config_attempt_safety s USING(attempt_id)')
        identity,lifecycle,state,publication,selection,snapshot=cur.fetchone()
        assert (lifecycle,state,publication)==('published','published',identity)
        assert payload['identity']['config_attempt_id']==identity
        assert payload['configuration_selection']=={key:selection[key] for key in ('source','version_id','base_sha256','effective_sha256')}
        assert payload['supplied']['effective_snapshot']==snapshot
    repeated=subprocess.run([str(artifact('runtime_publication_probe')),'publish_required_complete_governed'],capture_output=True,text=True,timeout=40)
    assert repeated.returncode!=0


def test_recovery_ignores_other_book_and_qt_stream(configured):
    native('admit_unsafe')
    identity=latest(configured)[0]
    with configured.cursor() as cur:
        cur.execute("INSERT INTO trading.positions(strategy_id,portfolio_id,portfolio_type,quantity) VALUES('LIVE_TREND','BOOK','qt',7)")
        cur.execute("INSERT INTO trading.strategy_registry(id,strategy_type,portfolio_id) VALUES('other','OTHER_ENGINE','OTHER_BOOK')")
        cur.execute("INSERT INTO trading.positions(strategy_id,portfolio_id,portfolio_type,quantity) VALUES('OTHER_ENGINE','OTHER_BOOK','system',8)")
    recover(configured,identity)
    assert latest(configured)[1:]==('aborted','recovered')


def validated_publication_config():
    import subprocess
    from tests.qt_test_artifacts import artifact
    snapshot=subprocess.run([str(artifact('runtime_publication_probe')),'snapshot_governed'],capture_output=True,text=True,check=True)
    baseline=json.loads(next(line.split('=',1)[1] for line in snapshot.stdout.splitlines() if line.startswith('RUNTIME_SNAPSHOT=')))
    validated=subprocess.run([str(artifact('live_config_validate'))],input=json.dumps({'schema':'live-config-validation/v1',
        'base_snapshot':baseline,'changes':{'/optimization/cost_penalty_scalar':12.75}}),capture_output=True,text=True,check=True)
    return json.loads(validated.stdout)


def approve_execution(conn, snapshot):
    with conn.cursor() as cur:
        cur.execute('''INSERT INTO trading.runtime_intents(registry_id,portfolio_id,engine_strategy_id,action,
        registry_revision,config_snapshot,status,requested_by,request_reason,approved_by,approval_reason,approved_at)
        VALUES('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,'approved','exec-submit','run','exec-approve','run',now())''',
        (json.dumps(snapshot),))


def test_final_publish_failure_stays_clean_and_retry_selects_approved_version(configured):
    import subprocess
    from tests.qt_test_artifacts import artifact
    from test_runtime_control_schema import prepare_exact_publication_schema
    from test_live_config_overrides import candidate,activate
    prepare_exact_publication_schema(configured)
    failed=subprocess.run([str(artifact('runtime_publication_probe')),'required_callback_failure_governed'],capture_output=True,text=True,timeout=40)
    assert failed.returncode==0,(failed.stdout,failed.stderr)
    assert latest(configured)[1:]==('failed','clean')
    data=validated_publication_config();version=candidate(configured,base=data);activate(configured,version)
    approve_execution(configured,data['effective_snapshot'])
    published=subprocess.run([str(artifact('runtime_publication_probe')),'publish_required_complete_governed_controlled'],capture_output=True,text=True,timeout=40)
    assert published.returncode==0,(published.stdout,published.stderr)
    payload=json.loads(next(line.split('=',1)[1] for line in published.stdout.splitlines() if line.startswith('CONFIG_PUBLICATION=')))
    assert payload['configuration_selection']['version_id']==version
    assert payload['supplied']['effective_snapshot']==data['effective_snapshot']


def test_inflight_activation_does_not_change_frozen_attempt(configured):
    import subprocess
    from tests.qt_test_artifacts import artifact
    from test_runtime_control_schema import prepare_exact_publication_schema
    from test_live_config_overrides import candidate,activate
    prepare_exact_publication_schema(configured)
    process=subprocess.Popen([str(artifact('runtime_publication_probe')),'publish_required_complete_governed_concurrent'],
        stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        while True:
            line=process.stdout.readline();assert line,process.stderr.read()
            if line.startswith('RUNTIME_READY'):break
        data=validated_publication_config();version=candidate(configured,base=data);activate(configured,version)
        stdout,stderr=process.communicate('publish\n',timeout=40)
        assert process.returncode==0,(stdout,stderr)
        payload=json.loads(next(line.split('=',1)[1] for line in stdout.splitlines() if line.startswith('CONFIG_PUBLICATION=')))
        assert payload['configuration_selection']['source']=='file'
        assert payload['configuration_selection']['version_id'] is None
        assert payload['supplied']['effective_snapshot']['optimization']['cost_penalty_scalar']!=12.75
    finally:
        if process.poll() is None:process.kill();process.wait()

@pytest.mark.parametrize('status',['running','failed'])
def test_legacy_attempt_without_safety_proof_never_guessed_clean(configured,status):
    with configured.cursor() as cur:
        cur.execute("""INSERT INTO trading.runtime_intents(registry_id,portfolio_id,engine_strategy_id,action,
          registry_revision,config_snapshot,status,requested_by,request_reason)
          VALUES('trend','BOOK','LIVE_TREND','run',0,'{}','pending','fixture','legacy') RETURNING id""")
        intent=cur.fetchone()[0]
        cur.execute("""INSERT INTO trading.runtime_attempts(id,intent_id,registry_revision,config_snapshot,
          run_date,producer_version,status,finished_at,failure_code)
          VALUES('legacy',%s,0,'{}','2026-09-30','legacy',%s,
          CASE WHEN %s='failed' THEN now() END,CASE WHEN %s='failed' THEN 'legacy_failure' END)""",(intent,status,status,status))
    assert native('admit_changed_base',expected=2)['error']=='config_retry_recovery_required'
    assert native('admit',expected=2)['error']=='config_retry_recovery_required'


def test_recovery_audit_cannot_be_rewritten(configured):
    native('admit_crash')
    identity=latest(configured)[0]
    recover(configured,identity)
    with pytest.raises(psycopg2.Error,match='config_recovery_not_eligible'):
        recover(configured,identity)


def test_selection_without_safety_is_not_clean(configured):
    identity=attempt(configured)
    with configured.cursor() as cur:
        cur.execute('DELETE FROM trading.live_config_attempt_safety WHERE attempt_id=%s',(identity,))
    assert native('admit_changed_base',expected=2)['error']=='config_retry_recovery_required'


def test_legacy_failure_in_other_book_does_not_block(configured):
    with configured.cursor() as cur:
        cur.execute("""INSERT INTO trading.runtime_intents(registry_id,portfolio_id,engine_strategy_id,action,
          registry_revision,config_snapshot,status,requested_by,request_reason)
          VALUES('trend','OTHER','LIVE_TREND','run',0,'{}','pending','fixture','legacy') RETURNING id""")
        intent=cur.fetchone()[0]
        cur.execute("""INSERT INTO trading.runtime_attempts(id,intent_id,registry_revision,config_snapshot,
          run_date,producer_version,status,finished_at,failure_code)
          VALUES('legacy',%s,0,'{}','2026-09-30','legacy','failed',now(),'legacy_failure')""",(intent,))
    native('admit_changed_base')


def test_financial_proof_is_bounded_database_digest(configured):
    native('admit_unsafe')
    identity=latest(configured)[0]
    with configured.cursor() as cur:
        cur.execute('SELECT pre_write_evidence FROM trading.live_config_attempt_safety WHERE attempt_id=%s',(identity,))
        proof=cur.fetchone()[0]
        assert proof['schema']=='live-config-financial-state/v2'
        assert proof['positions']['row_count']==0
        assert len(proof['positions']['sha256'])==64
        assert len(json.dumps(proof))<4096
    recover(configured,identity)


def test_recovery_proof_is_independent_of_session_serialization(configured):
    with configured.cursor() as cur:
        cur.execute("INSERT INTO trading.positions(strategy_id,portfolio_id,portfolio_type,quantity,last_update) VALUES('LIVE_TREND','BOOK','system',1.23456789,'2026-10-01T05:06:07Z')")
    native('admit_unsafe')
    identity=latest(configured)[0]
    with configured.cursor() as cur:
        cur.execute("SET TIME ZONE 'Asia/Tokyo'; SET DateStyle='SQL,DMY'; SET extra_float_digits=-3")
    recover(configured,identity)
    assert latest(configured)[1:]==('aborted','recovered')


def test_publisher_can_only_use_checked_lifecycle_functions(configured):
    native('admit_crash');identity=latest(configured)[0]
    with configured.cursor() as cur:
        cur.execute('GRANT USAGE ON SCHEMA trading TO qt_system_publisher')
        cur.execute('SET ROLE qt_system_publisher')
        try:
            cur.execute('SELECT trading.mark_live_config_unsafe(%s)',(identity,))
            with pytest.raises(psycopg2.Error,match='config_stop_evidence_missing'):
                cur.execute("SELECT trading.finish_live_config_attempt(%s,'stopped')",(identity,))
            cur.execute("SELECT trading.finish_live_config_attempt(%s,'failed')",(identity,))
        finally:cur.execute('RESET ROLE')
    assert latest(configured)[1:]==('failed','unsafe')
    assert native('admit',expected=2)['error']=='config_retry_recovery_required'
    recover(configured,identity)


def test_pre_write_marker_version_is_never_backfilled(configured):
    identity=attempt(configured)
    with configured.cursor() as cur:
        cur.execute('UPDATE trading.live_config_attempt_safety SET classification_version=NULL WHERE attempt_id=%s',(identity,))
        from test_live_config_overrides import MIGRATION
        cur.execute(MIGRATION.read_text())
        cur.execute('SELECT classification_version FROM trading.live_config_attempt_safety WHERE attempt_id=%s',(identity,))
        assert cur.fetchone()[0] is None
    assert native('admit_changed_base',expected=2)['error']=='config_retry_recovery_required'
    with pytest.raises(psycopg2.Error,match='config_recovery_evidence_missing'):
        recover(configured,identity)


def test_old_style_insert_after_upgrade_stays_unclassified(configured):
    identity=attempt(configured)
    with configured.cursor() as cur:
        cur.execute("""INSERT INTO trading.live_config_attempt_selections(attempt_id,portfolio_id,engine_strategy_id,
          run_date,engine_build,selection,config_snapshot)
          SELECT 'old-protocol',portfolio_id,engine_strategy_id,run_date,engine_build,selection,config_snapshot
          FROM trading.live_config_attempt_selections WHERE attempt_id=%s""",(identity,))
        cur.execute("INSERT INTO trading.live_config_attempt_safety(attempt_id) VALUES('old-protocol')")
        cur.execute("SELECT classification_version FROM trading.live_config_attempt_safety WHERE attempt_id='old-protocol'")
        assert cur.fetchone()[0] is None
        with pytest.raises(psycopg2.Error):
            cur.execute("SELECT trading.initialize_live_config_attempt_v2('old-protocol')")
    assert native('admit',expected=2)['error']=='config_retry_recovery_required'
    with pytest.raises(psycopg2.Error,match='config_recovery_evidence_missing'):
        recover(configured,'old-protocol')
