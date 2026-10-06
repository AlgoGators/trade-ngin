"""Specialized proposal API against actual owned PostgreSQL transactions."""
import json
import os
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import date
from pathlib import Path
from tests.qt_test_artifacts import artifact

import pytest
import psycopg2
from psycopg2.extras import Json

from test_proposal_storage_migration import (predecessor, apply, MIGRATION,
                                             normalize_positions_shape, replace_fence)
from test_runtime_control_schema import connection
from test_stream_storage import streams, publication_payload


PROBE = artifact("proposal_storage_probe")
EXACT_MIGRATION = Path(__file__).parents[2] / 'migrations/016_qt_exact_precision_and_seed_provenance.sql'


def run(mode):
    result = subprocess.run([str(PROBE), mode], capture_output=True, text=True,
                            env=os.environ.copy(),
                            timeout=30 if mode.startswith('handoff_') else None)
    assert result.returncode == 0, result.stdout + result.stderr + f' exit={result.returncode}'
    return result.stdout


@pytest.fixture()
def proposal_db(predecessor):
    apply(predecessor, MIGRATION)
    return predecessor


def test_seed_exact_system_day_preserves_edits_zeros_and_adds_missing(proposal_db):
    with proposal_db.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (strategy_id,strategy_name,portfolio_id,date,symbol,portfolio_type,
           quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update,updated_at)
          VALUES ('LIVE_TREND','TREND','BOOK','2026-09-22','ES','system',0,101.25,-2.5,1.75,
                  '2026-09-21 22:00+00','2026-09-21 22:01+00'),
                 ('LIVE_TREND','TREND','BOOK','2026-09-22','NQ','system',-3,202,4,-1,
                  '2026-09-21 22:00+00','2026-09-21 22:01+00'),
                 ('LIVE_TREND','TREND','BOOK','2026-09-21','OLD','qt',99,1,0,0,
                  '2026-09-21','2026-09-21')""")
    assert 'PROPOSAL_SEEDED=2\n' in run('seed')
    values = json.loads(run('read').split('PROPOSAL_ROWS=',1)[1])
    assert values == {'TREND': {
        'ES': {'quantity':0,'price':101.25,'unrealized':-2.5,'realized':1.75},
        'NQ': {'quantity':-3,'price':202,'unrealized':4,'realized':-1}}}
    with proposal_db.cursor() as cur:
        cur.execute("""UPDATE trading.positions SET quantity=7 WHERE portfolio_type='qt_proposal' AND symbol='NQ';
          DELETE FROM trading.positions WHERE portfolio_type='system' AND symbol='NQ';
          UPDATE trading.positions SET quantity=8 WHERE portfolio_type='system' AND symbol='ES';
          INSERT INTO trading.positions
            (strategy_id,strategy_name,portfolio_id,date,symbol,portfolio_type,quantity,
             average_price,daily_unrealized_pnl,daily_realized_pnl,last_update,updated_at)
          VALUES ('LIVE_TREND','TREND','BOOK','2026-09-22','YM','system',2,1,0,0,
                  '2026-09-22','2026-09-22')""")
    assert 'PROPOSAL_SEEDED=1\n' in run('seed')
    values = json.loads(run('read').split('PROPOSAL_ROWS=',1)[1])
    assert {s:row['quantity'] for s,row in values['TREND'].items()} == {'ES':0,'NQ':7,'YM':2}


def test_missing_015_is_error_even_with_empty_source_and_result(predecessor):
    assert 'PROPOSAL_ERROR=1\n' in run('seed_missing_schema')
    assert 'PROPOSAL_ERROR=1\n' in run('read_missing_schema')


def test_valid_015_empty_seed_and_reader_succeed(proposal_db):
    assert 'PROPOSAL_SEEDED=0\n' in run('seed')
    assert json.loads(run('read').split('PROPOSAL_ROWS=',1)[1]) == {}


@pytest.mark.parametrize('damage',['widened_only','missing_trigger','disabled_trigger',
                                   'wrong_function','conditional_trigger',
                                   'column_filtered_trigger','argument_trigger'])
def test_explicit_capability_refuses_partial_015_even_for_empty_operations(predecessor,damage):
    with predecessor.cursor() as cur:
        if damage!='widened_only':
            apply(predecessor,MIGRATION)
        if damage=='widened_only':
            cur.execute("""ALTER TABLE trading.positions DROP CONSTRAINT positions_portfolio_type_check;
              ALTER TABLE trading.positions ADD CONSTRAINT positions_portfolio_type_check
              CHECK (portfolio_type IN ('system','qt','benchmark','benchmark_rebench',
                  'benchmark_frozen_shadow','qt_proposal'))""")
        elif damage=='missing_trigger':
            cur.execute('DROP TRIGGER runtime_publication_fence ON trading.positions')
        elif damage=='disabled_trigger':
            cur.execute('ALTER TABLE trading.positions DISABLE TRIGGER runtime_publication_fence')
        elif damage=='conditional_trigger':
            replace_fence(predecessor,'positions',when='false')
        elif damage=='column_filtered_trigger':
            replace_fence(predecessor,'positions',update_of='quantity')
        elif damage=='argument_trigger':
            replace_fence(predecessor,'positions',arguments="'ignored'")
        else:
            cur.execute("""CREATE OR REPLACE FUNCTION trading.fence_runtime_publication_row()
              RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$""")
    assert 'PROPOSAL_ERROR=1\n' in run('seed_missing_schema')
    assert 'PROPOSAL_ERROR=1\n' in run('read_missing_schema')


def test_ignored_pending_registration_refusal_poisoned_by_conditional_fence(
        approved_proposal_run):
    conn,intent_id=approved_proposal_run
    replace_fence(conn,'positions',when='false')
    before=publication_payload(conn)
    assert 'PROPOSAL_PUBLISHED=0\n' in run('pending_conditional_fence')
    assert publication_payload(conn)==before
    with conn.cursor() as cur:
        cur.execute("SELECT intent_id,status,publication_id FROM trading.runtime_attempts")
        assert cur.fetchall()==[(intent_id,'failed',None)]


def test_final_callback_rechecks_conditional_fence_after_registration(
        approved_proposal_run):
    conn,intent_id=approved_proposal_run
    before=publication_payload(conn)
    assert 'PROPOSAL_PUBLISHED=0\n' in run('pending_capability_conditional')
    assert publication_payload(conn)==before
    with conn.cursor() as cur:
        cur.execute("""SELECT t.tgqual IS NOT NULL,t.tgenabled FROM pg_trigger t
          WHERE t.tgrelid='trading.positions'::regclass
            AND t.tgname='runtime_publication_fence'""")
        assert cur.fetchone()==(True,'O')
        cur.execute("SELECT intent_id,status,publication_id FROM trading.runtime_attempts")
        assert cur.fetchall()==[(intent_id,'failed',None)]


def test_reader_keeps_same_symbol_in_two_components_and_uses_exact_date(proposal_db):
    with proposal_db.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (strategy_id,strategy_name,portfolio_id,date,symbol,portfolio_type,quantity,
           average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('LIVE_TREND','TREND','BOOK','2026-09-22','ES','qt_proposal',0,101,-1,2,'2026-09-19'),
                 ('LIVE_TREND','OTHER','BOOK','2026-09-22','ES','qt_proposal',-5,201,3,-4,'2026-09-24'),
                 ('LIVE_TREND','TREND','BOOK','2026-09-23','NQ','qt_proposal',999,1,0,0,'2026-09-22'),
                 ('LIVE_TREND','TREND','BOOK','2026-09-22','YM','qt',888,1,0,0,'2026-09-22')""")
    values=json.loads(run('read').split('PROPOSAL_ROWS=',1)[1])
    assert values=={
        'TREND':{'ES':{'quantity':0,'price':101,'unrealized':-1,'realized':2}},
        'OTHER':{'ES':{'quantity':-5,'price':201,'unrealized':3,'realized':-4}}}


def test_invalid_scope_and_calendar_date_are_refused(proposal_db):
    for mode in ('invalid_seed_date','invalid_seed_book','invalid_reader'):
        assert 'PROPOSAL_ERROR=1\n' in run(mode)


def test_incubating_scope_can_seed_but_retired_scope_cannot(proposal_db):
    seed_source(proposal_db)
    with proposal_db.cursor() as cur:
        cur.execute("UPDATE trading.strategy_registry SET lifecycle='incubating',is_active=false WHERE id='trend'")
    assert seed_count()==1
    with proposal_db.cursor() as cur:
        cur.execute("UPDATE trading.strategy_registry SET lifecycle='retired' WHERE id='trend'")
    assert 'PROPOSAL_ERROR=1\n' in run('seed_refused')


def seed_source(conn):
    with conn.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (strategy_id,strategy_name,portfolio_id,date,symbol,portfolio_type,quantity,
           average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('LIVE_TREND','TREND','BOOK','2026-09-22','ES','system',12,101,1,2,'2026-09-22')""")


def seed_count():
    return int(run('seed').split('PROPOSAL_SEEDED=',1)[1].splitlines()[0])


def audited_edit_in_open_transaction(conn):
    with conn.cursor() as cur:
        cur.execute("SELECT id FROM trading.strategy_registry WHERE id='trend' FOR UPDATE")
        cur.execute("SELECT pg_advisory_xact_lock(hashtextextended('algolens:qt-book:BOOK',0))")
        cur.execute("""INSERT INTO trading.positions
          (strategy_id,strategy_name,portfolio_id,date,symbol,portfolio_type,quantity,
           average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('LIVE_TREND','TREND','BOOK','2026-09-22','ES','qt_proposal',0,101,1,2,'2026-09-22')
          ON CONFLICT (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type) DO NOTHING""")
        cur.execute("""SELECT to_jsonb(p) FROM trading.positions p
          WHERE strategy_id='LIVE_TREND' AND strategy_name='TREND' AND portfolio_id='BOOK'
            AND date='2026-09-22' AND symbol='ES' AND portfolio_type='qt_proposal' FOR UPDATE""")
        before=cur.fetchone()[0]
        cur.execute("""UPDATE trading.positions p SET quantity=7,updated_at=now()
          WHERE strategy_id='LIVE_TREND' AND strategy_name='TREND' AND portfolio_id='BOOK'
            AND date='2026-09-22' AND symbol='ES' AND portfolio_type='qt_proposal'
          RETURNING to_jsonb(p)""")
        after=cur.fetchone()[0]
        cur.execute("""INSERT INTO trading.position_overrides
          (user_id,source_app,strategy_id,symbol,before_state,after_state,reason,
           risk_check_result,overrode_risk,portfolio_id)
          VALUES (1,'algolens','LIVE_TREND','ES',%s,%s,'synthetic edit',%s,false,'BOOK')""",
          (Json(before),Json(after),Json({'passed':True,'breaches':[]})))


def assert_edit_and_audit(conn, expected_before):
    with conn.cursor() as cur:
        cur.execute("""SELECT quantity,average_price FROM trading.positions
          WHERE portfolio_type='qt_proposal' AND symbol='ES'""")
        assert cur.fetchall()==[(7,101)]
        cur.execute("""SELECT (before_state->>'quantity')::numeric,
          (after_state->>'quantity')::numeric,portfolio_id
          FROM trading.position_overrides ORDER BY id""")
        assert cur.fetchall()==[(expected_before,7,'BOOK')]


def test_two_simultaneous_seeds_insert_one_full_identity(proposal_db):
    seed_source(proposal_db)
    with ThreadPoolExecutor(max_workers=2) as pool:
        futures=[pool.submit(seed_count) for _ in range(2)]
        assert sorted(f.result(timeout=15) for f in futures)==[0,1]
    with proposal_db.cursor() as cur:
        cur.execute("SELECT count(*),sum(quantity) FROM trading.positions WHERE portfolio_type='qt_proposal'")
        assert cur.fetchone()==(1,12)


def test_edit_wins_when_it_holds_registry_book_locks_before_seed(proposal_db):
    seed_source(proposal_db)
    editor=psycopg2.connect(os.environ['ALGOLENS_TEST_DB'])
    try:
        audited_edit_in_open_transaction(editor)
        with ThreadPoolExecutor(max_workers=1) as pool:
            future=pool.submit(seed_count)
            time.sleep(.2)
            assert not future.done(), 'seed bypassed the audited editor lock'
            editor.commit()
            assert future.result(timeout=15)==0
        assert_edit_and_audit(proposal_db,0)
    finally:
        editor.rollback()
        editor.close()


def test_seed_wins_when_its_transaction_enters_before_audited_edit(proposal_db):
    seed_source(proposal_db)
    with proposal_db.cursor() as cur:
        cur.execute("""CREATE FUNCTION trading.proposal_test_pause() RETURNS trigger LANGUAGE plpgsql AS $$
          BEGIN PERFORM pg_sleep(0.6); RETURN NEW; END $$;
          CREATE TRIGGER proposal_test_pause BEFORE INSERT ON trading.positions
          FOR EACH ROW WHEN (NEW.portfolio_type='qt_proposal')
          EXECUTE FUNCTION trading.proposal_test_pause()""")
    def edit():
        editor=psycopg2.connect(os.environ['ALGOLENS_TEST_DB'])
        try:
            audited_edit_in_open_transaction(editor)
            editor.commit()
        finally:
            editor.close()
    with ThreadPoolExecutor(max_workers=2) as pool:
        seed_future=pool.submit(seed_count)
        time.sleep(.15)
        edit_future=pool.submit(edit)
        assert seed_future.result(timeout=15)==1
        edit_future.result(timeout=15)
    assert_edit_and_audit(proposal_db,12)


def test_seed_is_independent_across_component_engine_book_day_and_stream(proposal_db):
    with proposal_db.cursor() as cur:
        cur.execute("""INSERT INTO trading.strategy_registry(id,strategy_type,portfolio_id)
          VALUES ('other','LIVE_OTHER','BOOK');
          INSERT INTO trading.strategy_book_memberships(strategy_id,portfolio_id)
          VALUES ('trend','BOOK2');
          INSERT INTO trading.positions
          (strategy_id,strategy_name,portfolio_id,date,symbol,portfolio_type,quantity,
           average_price,daily_unrealized_pnl,daily_realized_pnl,last_update,updated_at)
          VALUES ('LIVE_TREND','TREND','BOOK','2026-09-22','ES','system',1,101,0,0,'2026-09-22','2026-09-22'),
                 ('LIVE_TREND','OTHER','BOOK','2026-09-22','ES','system',2,102,0,0,'2026-09-22','2026-09-22'),
                 ('LIVE_TREND','TREND','BOOK2','2026-09-22','ES','system',3,103,0,0,'2026-09-22','2026-09-22'),
                 ('LIVE_OTHER','TREND','BOOK','2026-09-22','ES','system',4,104,0,0,'2026-09-22','2026-09-22'),
                 ('LIVE_TREND','TREND','BOOK','2026-09-23','ES','system',5,105,0,0,'2026-09-23','2026-09-23'),
                 ('LIVE_TREND','TREND','BOOK','2026-09-22','ES','qt',901,901,0,0,'2026-09-22','2026-09-22'),
                 ('LIVE_TREND','TREND','BOOK','2026-09-22','ES','benchmark',902,902,0,0,'2026-09-22','2026-09-22')""")
    assert 'PROPOSAL_SEEDED=1\n' in run('seed')
    with proposal_db.cursor() as cur:
        cur.execute("SELECT strategy_id,strategy_name,portfolio_id,date,quantity FROM trading.positions WHERE portfolio_type='qt_proposal'")
        assert cur.fetchall()==[('LIVE_TREND','TREND','BOOK',date(2026,9,22),1)]
    for mode in ('seed_other_component','seed_other_book','seed_other_engine','seed_other_day'):
        assert 'PROPOSAL_SEEDED=1\n' in run(mode)
    with proposal_db.cursor() as cur:
        cur.execute("""SELECT strategy_id,strategy_name,portfolio_id,date,quantity FROM trading.positions
          WHERE portfolio_type='qt_proposal' ORDER BY quantity""")
        assert [row[-1] for row in cur.fetchall()]==[1,2,3,4,5]
        cur.execute("""SELECT portfolio_type,quantity FROM trading.positions
          WHERE portfolio_type IN ('qt','benchmark') ORDER BY portfolio_type""")
        assert cur.fetchall()==[('benchmark',902),('qt',901)]


@pytest.fixture()
def approved_proposal_run(streams):
    normalize_positions_shape(streams)
    apply(streams,MIGRATION)
    apply(streams,EXACT_MIGRATION)
    return approve_synthetic_run(streams)


def approve_synthetic_run(streams,snapshot_mode='pending_snapshot'):
    snapshot = json.loads(run(snapshot_mode).split('PROPOSAL_SNAPSHOT=',1)[1].splitlines()[0])
    with streams.cursor() as cur:
        cur.execute("""INSERT INTO trading.runtime_intents
          (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,config_snapshot,
           status,requested_by,request_reason,approved_by,approval_reason,approved_at)
          SELECT 'trend','BOOK','LIVE_TREND','run',runtime_revision,%s::jsonb,
                 'approved','1','synthetic request','2','synthetic approval',now()
          FROM trading.strategy_registry WHERE id='trend' RETURNING id""", (json.dumps(snapshot),))
        intent_id = cur.fetchone()[0]
    return streams, intent_id


def test_pending_registration_refuses_missing_015_before_returning_queued_zero(streams):
    normalize_positions_shape(streams)
    conn,intent_id=approve_synthetic_run(streams)
    before=publication_payload(conn)
    assert 'PROPOSAL_PUBLISHED=0\n' in run('pending_missing_015')
    assert publication_payload(conn)==before
    with conn.cursor() as cur:
        cur.execute('SELECT intent_id,status,publication_id FROM trading.runtime_attempts')
        assert cur.fetchall()==[(intent_id,'failed',None)]


def test_other_component_retains_repeated_source_writes_after_first_is_sealed(streams):
    normalize_positions_shape(streams)
    apply(streams,MIGRATION)
    apply(streams,EXACT_MIGRATION)
    conn,intent_id=approve_synthetic_run(streams,'pending_two_components_snapshot')
    assert 'PROPOSAL_PUBLISHED=1\n' in run('pending_two_components')
    with conn.cursor() as cur:
        cur.execute("SELECT strategy_name,portfolio_type,quantity FROM trading.positions ORDER BY strategy_name,portfolio_type")
        assert cur.fetchall()==[
            ('OTHER','qt',24),('OTHER','system',24),
            ('TREND','qt',12),('TREND','qt_proposal',12),('TREND','system',12)]
        cur.execute('SELECT intent_id,status FROM trading.runtime_attempts')
        assert cur.fetchall()==[(intent_id,'applied')]


def test_abandoned_run_does_not_keep_proposal_seal(approved_proposal_run):
    conn,intent_id=approved_proposal_run
    assert 'PROPOSAL_PUBLISHED=1\n' in run('pending_abandon')
    with conn.cursor() as cur:
        cur.execute("SELECT portfolio_type,quantity FROM trading.positions ORDER BY portfolio_type")
        assert cur.fetchall()==[('qt',14),('system',14)]
        cur.execute('SELECT intent_id,status FROM trading.runtime_attempts ORDER BY started_at,id')
        assert cur.fetchall()==[(intent_id,'failed'),(intent_id,'applied')]


@pytest.mark.parametrize('mode,quantity,proposal',[
    ('pending_before',14,14),('pending_repeat',12,12),('pending_no_proposal',14,None),
    ('pending_empty_after',12,12)])
def test_pending_publication_uses_final_source_and_keeps_qt_parts(
        approved_proposal_run, mode, quantity, proposal):
    conn, intent_id = approved_proposal_run
    assert 'PROPOSAL_PUBLISHED=1\n' in run(mode)
    with conn.cursor() as cur:
        cur.execute("""SELECT portfolio_type,quantity FROM trading.positions
          WHERE strategy_id='LIVE_TREND' AND portfolio_id='BOOK' AND date='2026-09-22'
          ORDER BY portfolio_type""")
        expected = [('qt',quantity),('system',quantity)]
        if proposal is not None:
            expected.insert(1, ('qt_proposal',proposal))
        assert cur.fetchall() == expected
        cur.execute("""SELECT intent_id,status,outcome,publication_id=id FROM trading.runtime_attempts""")
        assert cur.fetchall() == [(intent_id,'applied','published',True)]


def test_existing_edited_proposal_wins_over_final_system_batch(approved_proposal_run):
    conn,intent_id=approved_proposal_run
    with conn.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (strategy_id,strategy_name,portfolio_id,date,symbol,portfolio_type,quantity,
           average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('LIVE_TREND','TREND','BOOK','2026-09-22','ES','qt_proposal',7,101,0,0,'2026-09-22')""")
    assert 'PROPOSAL_PUBLISHED=1\n' in run('pending_before')
    with conn.cursor() as cur:
        cur.execute("SELECT portfolio_type,quantity FROM trading.positions ORDER BY portfolio_type")
        assert cur.fetchall()==[('qt',14),('qt_proposal',7),('system',14)]
        cur.execute('SELECT intent_id,status FROM trading.runtime_attempts')
        assert cur.fetchall()==[(intent_id,'applied')]


@pytest.mark.parametrize('mode',[
    'pending_after_diff','pending_after_same','pending_missing_source','pending_wrong_date',
    'pending_wrong_member','pending_wrong_book','pending_generic_position','pending_legacy_read',
    'pending_empty_source','pending_no_qt',
    'pending_report_read','pending_prior_results','pending_invalid_result',
    'pending_invalid_numeric','pending_invalid_execution','pending_invalid_empty_execution',
    'pending_invalid_legacy_cleanup','pending_invalid_scoped_cleanup',
    'pending_invalid_single_equity','pending_invalid_batch_equity',
    'pending_invalid_delete_result','pending_invalid_update_result',
    'pending_invalid_delete_equity','pending_invalid_update_equity',
    'pending_capability_revoked','pending_retired','pending_revision_aba','pending_intent_revoked'])
def test_bad_proposal_order_or_ignored_generic_refusal_poisons_complete_run(
        approved_proposal_run,mode):
    conn,intent_id=approved_proposal_run
    before=publication_payload(conn)
    assert 'PROPOSAL_PUBLISHED=0\n' in run(mode)
    assert publication_payload(conn)==before
    with conn.cursor() as cur:
        cur.execute("""SELECT intent_id,status,publication_id,finished_at IS NOT NULL
          FROM trading.runtime_attempts""")
        assert cur.fetchall()==[(intent_id,'failed',None,True)]


@pytest.fixture()
def approved_handoff_run(streams):
    normalize_positions_shape(streams)
    apply(streams,MIGRATION)
    apply(streams,EXACT_MIGRATION)
    return approve_synthetic_run(streams,'pending_two_components_snapshot')


def test_helper_publishes_both_components_and_preserves_qt_and_edited_drafts(
        approved_handoff_run):
    conn,intent_id=approved_handoff_run
    with conn.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (strategy_id,strategy_name,portfolio_id,date,symbol,portfolio_type,quantity,
           average_price,daily_unrealized_pnl,daily_realized_pnl,last_update,updated_at)
          VALUES ('LIVE_TREND','TREND','BOOK','2026-09-22','ES','qt',901,901,0,0,
                  '2026-09-19 01:00+00','2026-09-19 02:00+00'),
                 ('LIVE_TREND','OTHER','BOOK','2026-09-22','ES','qt',902,902,0,0,
                  '2026-09-19 01:00+00','2026-09-19 02:00+00'),
                 ('LIVE_TREND','TREND','BOOK','2026-09-22','ES','qt_proposal',0,701,-1,2,
                  '2026-09-18 01:00+00','2026-09-18 02:00+00'),
                 ('LIVE_TREND','OTHER','BOOK','2026-09-22','ES','qt_proposal',-4,702,-3,4,
                  '2026-09-17 01:00+00','2026-09-17 02:00+00')""")
        cur.execute("""SELECT strategy_name,portfolio_type,to_jsonb(p)::text
          FROM trading.positions p WHERE symbol='ES' AND portfolio_type IN ('qt','qt_proposal')
          ORDER BY strategy_name,portfolio_type""")
        protected=cur.fetchall()
    before=publication_payload(conn)
    output=run('handoff_success')
    assert 'HANDOFF_PRECOMMIT_VISIBLE=0\n' in output
    assert 'HANDOFF_PUBLISHED=1\n' in output
    with conn.cursor() as cur:
        cur.execute("""SELECT strategy_name,symbol,portfolio_type,quantity,
          average_price,daily_unrealized_pnl,daily_realized_pnl
          FROM trading.positions WHERE strategy_id='LIVE_TREND' AND portfolio_id='BOOK'
            AND date='2026-09-22' AND portfolio_type IN ('system','qt_proposal')
          ORDER BY strategy_name,symbol,portfolio_type""")
        assert cur.fetchall()==[
            ('OTHER','ES','qt_proposal',-4,702,-3,4),
            ('OTHER','ES','system',22,111,7,8),
            ('TREND','ES','qt_proposal',0,701,-1,2),
            ('TREND','ES','system',12,101,1,2),
            ('TREND','NQ','qt_proposal',0,202,3,4),
            ('TREND','NQ','system',0,202,3,4)]
        cur.execute("""SELECT strategy_name,portfolio_type,to_jsonb(p)::text
          FROM trading.positions p WHERE symbol='ES' AND portfolio_type IN ('qt','qt_proposal')
          ORDER BY strategy_name,portfolio_type""")
        assert cur.fetchall()==protected
        cur.execute("SELECT intent_id,status,outcome,publication_id=id FROM trading.runtime_attempts")
        assert cur.fetchall()==[(intent_id,'applied','published',True)]
    after=publication_payload(conn)
    assert after!=before
    assert all(after[table]!=before[table] for table in
               ('positions','live_results','executions','equity_curve'))


def test_repeat_helper_preserves_drafts_and_seeds_only_new_symbol(approved_handoff_run):
    conn,intent_id=approved_handoff_run
    assert 'HANDOFF_PUBLISHED=1\n' in run('handoff_success')
    with conn.cursor() as cur:
        cur.execute("""UPDATE trading.positions SET quantity=0,average_price=777,
          last_update='2026-09-18 01:00+00',updated_at='2026-09-18 02:00+00'
          WHERE strategy_name='TREND' AND symbol='ES' AND portfolio_type='qt_proposal'""")
        cur.execute("""UPDATE trading.positions SET quantity=-4,average_price=778,
          last_update='2026-09-17 01:00+00',updated_at='2026-09-17 02:00+00'
          WHERE strategy_name='OTHER' AND symbol='ES' AND portfolio_type='qt_proposal'""")
        cur.execute("""SELECT to_jsonb(p)::text FROM trading.positions p
          WHERE portfolio_type='qt_proposal' ORDER BY strategy_name,symbol""")
        prior=cur.fetchall()
    output=run('handoff_repeat')
    assert 'HANDOFF_PRECOMMIT_VISIBLE=0\n' in output
    assert 'HANDOFF_PUBLISHED=1\n' in output
    with conn.cursor() as cur:
        cur.execute("""SELECT to_jsonb(p)::text FROM trading.positions p
          WHERE portfolio_type='qt_proposal' AND symbol<>'YM'
          ORDER BY strategy_name,symbol""")
        assert cur.fetchall()==prior
        cur.execute("""SELECT strategy_name,symbol,quantity,average_price,
          daily_unrealized_pnl,daily_realized_pnl FROM trading.positions
          WHERE portfolio_type='qt_proposal' AND symbol='YM'""")
        assert cur.fetchall()==[('TREND','YM',7,303,5,6)]
        cur.execute("""SELECT intent_id,status,outcome,publication_id=id
          FROM trading.runtime_attempts ORDER BY started_at,id""")
        assert cur.fetchall()==[(intent_id,'applied','published',True)]*2


def test_helper_refuses_missing_015_and_abandons_without_payload(streams):
    normalize_positions_shape(streams)
    conn,intent_id=approve_synthetic_run(streams,'pending_two_components_snapshot')
    before=publication_payload(conn)
    output=run('handoff_missing_015')
    assert 'HANDOFF_HELPER_ERROR=1\n' in output
    assert 'HANDOFF_PUBLISHED=0\n' in output
    assert publication_payload(conn)==before
    with conn.cursor() as cur:
        cur.execute("""SELECT intent_id,status,publication_id,finished_at IS NOT NULL
          FROM trading.runtime_attempts""")
        assert cur.fetchall()==[(intent_id,'failed',None,True)]


def test_later_component_proposal_failure_rolls_back_every_publication_write(
        approved_handoff_run):
    conn,intent_id=approved_handoff_run
    with conn.cursor() as cur:
        cur.execute("""CREATE SEQUENCE trading.handoff_callback_reached START 1;
          CREATE FUNCTION trading.reject_late_proposal() RETURNS trigger
          LANGUAGE plpgsql AS $$ BEGIN
            IF EXISTS (SELECT 1 FROM trading.positions p
              WHERE p.strategy_id=NEW.strategy_id AND p.portfolio_id=NEW.portfolio_id
                AND p.date=NEW.date AND p.strategy_name='TREND' AND p.symbol='NQ'
                AND p.portfolio_type='qt_proposal') THEN
              PERFORM nextval('trading.handoff_callback_reached');
              RAISE EXCEPTION 'fixture_later_component_after_first_proposal';
            END IF;
            RETURN NEW;
          END $$;
          CREATE TRIGGER reject_late_proposal BEFORE INSERT ON trading.positions
          FOR EACH ROW WHEN (NEW.portfolio_type='qt_proposal' AND NEW.strategy_name='OTHER')
          EXECUTE FUNCTION trading.reject_late_proposal()""")
    before=publication_payload(conn)
    output=run('handoff_callback_failure')
    assert 'HANDOFF_PRECOMMIT_VISIBLE=0\n' in output
    assert 'HANDOFF_PUBLISHED=0\n' in output
    assert publication_payload(conn)==before
    with conn.cursor() as cur:
        cur.execute('SELECT last_value,is_called FROM trading.handoff_callback_reached')
        assert cur.fetchone()==(1,True), 'later callback never observed the earlier draft'
        cur.execute("""SELECT intent_id,status,outcome,publication_id,finished_at IS NOT NULL
          FROM trading.runtime_attempts""")
        assert cur.fetchall()==[(intent_id,'failed',None,None,True)]


def test_legacy_revision_zero_uses_same_helper_without_controlled_attempt(streams):
    normalize_positions_shape(streams)
    apply(streams,MIGRATION)
    apply(streams,EXACT_MIGRATION)
    output=run('handoff_legacy')
    assert 'HANDOFF_PRECOMMIT_VISIBLE=0\n' in output
    assert 'HANDOFF_PUBLISHED=1\n' in output
    with streams.cursor() as cur:
        cur.execute("""SELECT strategy_name,symbol,quantity FROM trading.positions
          WHERE portfolio_type='qt_proposal' ORDER BY strategy_name,symbol""")
        assert cur.fetchall()==[('OTHER','ES',22),('TREND','ES',12),('TREND','NQ',0)]
        cur.execute('SELECT count(*) FROM trading.runtime_attempts')
        assert cur.fetchone()==(0,)
