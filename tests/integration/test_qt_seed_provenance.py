"""Exact MODEL seed storage against the owned, route-free PostgreSQL fixture."""
from pathlib import Path
from datetime import date
import hashlib
import json
import os
import subprocess

import psycopg2
import pytest

from test_proposal_storage_migration import predecessor, apply, normalize_positions_shape
from test_runtime_control_schema import connection
from test_stream_storage import streams
from test_proposal_storage import approve_synthetic_run, publication_payload, run


ROOT = Path(__file__).parents[2]
PROPOSAL = ROOT / "migrations/015_qt_proposal_positions.sql"
MIGRATION = ROOT / "migrations/016_qt_exact_precision_and_seed_provenance.sql"
ROLLBACK = ROOT / "migrations/016_qt_exact_precision_and_seed_provenance_rollback.sql"


def column_shape(conn):
    with conn.cursor() as cur:
        cur.execute("""SELECT format_type(atttypid,atttypmod) FROM pg_attribute
          WHERE attrelid='trading.positions'::regclass
            AND attname IN ('quantity','average_price') ORDER BY attname""")
        return [row[0] for row in cur.fetchall()]


def test_exact_storage_migration_and_lossless_rollback(predecessor):
    apply(predecessor, PROPOSAL)
    with predecessor.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,
           quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('BOOK','LIVE_TREND','TREND','2026-09-22','ES','system',
                  12.123456,101.123456,0,0,'2026-09-22')""")
    if MIGRATION.exists():
        apply(predecessor, MIGRATION)
    assert column_shape(predecessor) == ['numeric(20,8)', 'numeric(20,8)']
    with predecessor.cursor() as cur:
        cur.execute("""UPDATE trading.positions SET quantity=12.12345678,
          average_price=101.12345678 WHERE symbol='ES'""")
        cur.execute("""SELECT quantity::text,average_price::text FROM trading.positions
          WHERE symbol='ES'""")
        assert cur.fetchone() == ('12.12345678', '101.12345678')
    with pytest.raises(psycopg2.Error, match='six.place|six place|loss'):
        apply(predecessor, ROLLBACK)
    with predecessor.cursor() as cur:
        cur.execute('ROLLBACK')
        cur.execute("""SELECT quantity::text,average_price::text FROM trading.positions
          WHERE symbol='ES'""")
        assert cur.fetchone() == ('12.12345678', '101.12345678')
        cur.execute("""UPDATE trading.positions SET quantity=12.123456,
          average_price=101.123456 WHERE symbol='ES'""")
    apply(predecessor, ROLLBACK)
    assert column_shape(predecessor) == ['numeric(20,6)', 'numeric(20,6)']


def test_widening_refuses_integer_capacity_loss(predecessor):
    apply(predecessor, PROPOSAL)
    with predecessor.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,
           quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('BOOK','LIVE_TREND','TREND','2026-09-22','ES','system',
                  1000000000000,1,0,0,'2026-09-22')""")
    with pytest.raises(psycopg2.Error, match='integer range'):
        apply(predecessor, MIGRATION)
    with predecessor.cursor() as cur:
        cur.execute('ROLLBACK')
        assert column_shape(predecessor) == ['numeric(20,6)', 'numeric(20,6)']
        cur.execute("SELECT quantity::text FROM trading.positions WHERE symbol='ES'")
        assert cur.fetchone() == ('1000000000000.000000',)
        cur.execute("SELECT to_regclass('trading.qt_model_seed_publications')")
        assert cur.fetchone() == (None,)


@pytest.mark.parametrize('column,value', [
    ('quantity', '92233720368.547759'),
    ('quantity', '-92233720368.547759'),
    ('average_price', '92233720368.547759'),
    ('average_price', '-92233720368.547759'),
])
def test_widening_refuses_values_outside_signed_decimal8_raw_range(
        predecessor, column, value):
    apply(predecessor, PROPOSAL)
    with predecessor.cursor() as cur:
        cur.execute(f"""INSERT INTO trading.positions
          (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,
           quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('BOOK','LIVE_TREND','TREND','2026-09-22','ES','system',
                  1,1,0,0,'2026-09-22')""")
        cur.execute(f'UPDATE trading.positions SET {column}=%s', (value,))
        cur.execute('SELECT quantity::text,average_price::text FROM trading.positions')
        before = cur.fetchone()
    with pytest.raises(psycopg2.Error, match='Decimal8|raw range'):
        apply(predecessor, MIGRATION)
    with predecessor.cursor() as cur:
        cur.execute('ROLLBACK')
        assert column_shape(predecessor) == ['numeric(20,6)', 'numeric(20,6)']
        cur.execute('SELECT quantity::text,average_price::text FROM trading.positions')
        assert cur.fetchone() == before
        cur.execute("SELECT to_regclass('trading.qt_model_seed_publications')")
        assert cur.fetchone() == (None,)


def test_widening_accepts_old_six_place_values_inside_signed_decimal8_raw_range(
        predecessor):
    apply(predecessor, PROPOSAL)
    with predecessor.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,
           quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('BOOK','LIVE_TREND','TREND','2026-09-22','ES','system',
                  92233720368.547758,-92233720368.547758,0,0,'2026-09-22')""")
    apply(predecessor, MIGRATION)
    with predecessor.cursor() as cur:
        cur.execute('SELECT quantity::text,average_price::text FROM trading.positions')
        assert cur.fetchone() == ('92233720368.54775800',
                                  '-92233720368.54775800')


@pytest.fixture()
def disposable_pg(streams):
    normalize_positions_shape(streams)
    apply(streams, PROPOSAL)
    apply(streams, MIGRATION)
    return approve_synthetic_run(streams, 'pending_two_components_snapshot')


@pytest.fixture()
def probe():
    binary = Path('/home/devcontainers/qt-validation-20260921/bin/Debug/proposal_storage_probe')

    def publish_system_with_preserved_proposal_edit():
        result = subprocess.run([str(binary), '--scenario', 'seed-provenance'],
                                capture_output=True, text=True, env=os.environ.copy(), timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout

    return publish_system_with_preserved_proposal_edit


def test_model_seed_record_is_atomic_and_does_not_relabel_draft(disposable_pg, probe):
    conn, intent_id = disposable_pg
    with conn.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,
           quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('BOOK','LIVE_TREND','TREND','2026-09-22','ES','qt_proposal',
                  5.25,701,0,0,'2026-09-22')""")
    assert 'HANDOFF_PUBLISHED=1\n' in probe()
    with conn.cursor() as cur:
        cur.execute("""SELECT publication_id::text,attempt_id::text,publication_version,
          system_components::text,seed_digest,producer_version
          FROM trading.qt_model_seed_publications""")
        seed_records = cur.fetchall()
        assert len(seed_records) == 1
        publication_id, linked_attempt, version, payload, digest, producer = seed_records[0]
        cur.execute("""SELECT id::text,publication_id::text,status,outcome
          FROM trading.runtime_attempts WHERE intent_id=%s""", (intent_id,))
        assert cur.fetchall() == [(publication_id,publication_id,'applied','published')]
        assert linked_attempt == publication_id
        assert version == 1
        assert len(digest) == 64 and producer == 'proposal-handoff-test'
        seed_rows = json.loads(payload)
        assert [(r['key']['strategy_name'],r['key']['symbol'],r['quantity_exact'],
                 r['average_price_exact']) for r in seed_rows] == [
                     ('OTHER','ES','22','111'),
                     ('TREND','ES','12','101'),
                     ('TREND','NQ','0','202')]
        assert all(row['key']['portfolio_type'] == 'system' for row in seed_rows)
        cur.execute("""SELECT strategy_name,symbol,quantity::text FROM trading.positions
          WHERE portfolio_type='qt_proposal' ORDER BY strategy_name,symbol""")
        assert cur.fetchall() == [('OTHER','ES','22.00000000'),
                                  ('TREND','ES','5.25000000'),
                                  ('TREND','NQ','0.00000000')]


def test_same_day_retry_retains_first_source_and_versions_second(disposable_pg, probe):
    conn, intent_id = disposable_pg
    assert 'HANDOFF_PUBLISHED=1\n' in probe()
    with conn.cursor() as cur:
        cur.execute("""SELECT publication_id::text,publication_version,seed_digest,
          system_components::text FROM trading.qt_model_seed_publications""")
        first = cur.fetchone()
        cur.execute("""UPDATE trading.positions SET quantity=5.25
          WHERE strategy_name='TREND' AND symbol='ES' AND portfolio_type='qt_proposal'""")
    assert 'HANDOFF_PUBLISHED=1\n' in run('handoff_repeat')
    with conn.cursor() as cur:
        cur.execute("""SELECT publication_id::text,publication_version,seed_digest,
          system_components::text FROM trading.qt_model_seed_publications
          ORDER BY publication_version""")
        rows = cur.fetchall()
        assert len(rows) == 2 and rows[0] == first
        assert rows[0][1] == 1 and rows[1][1] == 2
        assert rows[0][0] != rows[1][0] and rows[0][2] != rows[1][2]
        assert [r['key']['symbol'] for r in json.loads(rows[1][3])] == ['ES','ES','NQ','YM']
        cur.execute("""SELECT quantity::text FROM trading.positions
          WHERE strategy_name='TREND' AND symbol='ES' AND portfolio_type='qt_proposal'""")
        assert cur.fetchone() == ('5.25000000',)
        cur.execute("""SELECT count(*),count(DISTINCT id) FROM trading.runtime_attempts
          WHERE intent_id=%s AND status='applied'""", (intent_id,))
        assert cur.fetchone() == (2,2)


def test_failed_callback_rolls_back_seed_and_proposals(disposable_pg):
    conn, intent_id = disposable_pg
    with conn.cursor() as cur:
        cur.execute("""CREATE FUNCTION trading.reject_second_seed() RETURNS trigger
          LANGUAGE plpgsql AS $$ BEGIN
            RAISE EXCEPTION 'synthetic_late_proposal_failure';
          END $$;
          CREATE TRIGGER reject_second_seed BEFORE INSERT ON trading.positions
          FOR EACH ROW WHEN (NEW.portfolio_type='qt_proposal' AND NEW.strategy_name='OTHER')
          EXECUTE FUNCTION trading.reject_second_seed()""")
    assert 'HANDOFF_PUBLISHED=0\n' in run('handoff_callback_failure')
    with conn.cursor() as cur:
        cur.execute('SELECT count(*) FROM trading.qt_model_seed_publications')
        assert cur.fetchone() == (0,)
        cur.execute("""SELECT count(*) FROM trading.positions
          WHERE strategy_id='LIVE_TREND' AND portfolio_id='BOOK'
            AND date='2026-09-22' AND portfolio_type IN ('system','qt_proposal')""")
        assert cur.fetchone() == (0,)
        cur.execute("""SELECT status,publication_id FROM trading.runtime_attempts
          WHERE intent_id=%s""", (intent_id,))
        assert cur.fetchall() == [('failed',None)]


def test_seed_is_immutable_including_truncate(disposable_pg, probe):
    conn, _ = disposable_pg
    assert 'HANDOFF_PUBLISHED=1\n' in probe()
    with conn.cursor() as cur:
        for statement in ("UPDATE trading.qt_model_seed_publications SET seed_digest=repeat('a',64)",
                          'DELETE FROM trading.qt_model_seed_publications',
                          'TRUNCATE trading.qt_model_seed_publications'):
            with pytest.raises(psycopg2.Error, match='immutable'):
                cur.execute(statement)
        cur.execute('SELECT count(*) FROM trading.qt_model_seed_publications')
        assert cur.fetchone() == (1,)


@pytest.mark.parametrize('damage', [
    'disabled_seed_update', 'missing_seed_delete', 'missing_seed_truncate',
    'conditional_seed_update', 'filtered_seed_update', 'argument_seed_update',
    'missing_capability_update', 'missing_capability_truncate',
    'replaced_mutation_function', 'wrong_seed_primary_key',
    'missing_attempt_unique', 'missing_version_unique',
    'missing_seed_digest_check', 'missing_system_components_check',
    'missing_capability_version_check', 'nullable_producer_version',
])
def test_final_publication_refuses_seed_catalog_drift_without_partial_payload(
        disposable_pg, damage):
    conn, intent_id = disposable_pg
    with conn.cursor() as cur:
        if damage == 'disabled_seed_update':
            cur.execute('ALTER TABLE trading.qt_model_seed_publications '
                        'DISABLE TRIGGER qt_model_seed_immutable')
        elif damage in ('missing_seed_delete', 'conditional_seed_update',
                        'filtered_seed_update', 'argument_seed_update'):
            cur.execute('DROP TRIGGER qt_model_seed_immutable '
                        'ON trading.qt_model_seed_publications')
            events = {
                'missing_seed_delete': 'UPDATE',
                'conditional_seed_update': 'UPDATE OR DELETE',
                'filtered_seed_update': 'UPDATE OF seed_digest OR DELETE',
                'argument_seed_update': 'UPDATE OR DELETE',
            }[damage]
            suffix = ' WHEN (false)' if damage == 'conditional_seed_update' else ''
            argument = "'ignored'" if damage == 'argument_seed_update' else ''
            cur.execute(f'CREATE TRIGGER qt_model_seed_immutable BEFORE {events} '
                        'ON trading.qt_model_seed_publications FOR EACH ROW'
                        f'{suffix} EXECUTE FUNCTION trading.refuse_qt_seed_mutation({argument})')
        elif damage == 'missing_seed_truncate':
            cur.execute('DROP TRIGGER qt_model_seed_no_truncate '
                        'ON trading.qt_model_seed_publications')
        elif damage == 'missing_capability_update':
            cur.execute('DROP TRIGGER qt_storage_capability_immutable '
                        'ON trading.qt_storage_capabilities')
        elif damage == 'missing_capability_truncate':
            cur.execute('DROP TRIGGER qt_storage_capability_no_truncate '
                        'ON trading.qt_storage_capabilities')
        elif damage == 'replaced_mutation_function':
            cur.execute("""CREATE OR REPLACE FUNCTION trading.refuse_qt_seed_mutation()
              RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$""")
        elif damage == 'wrong_seed_primary_key':
            cur.execute('ALTER TABLE trading.qt_model_seed_publications '
                        'DROP CONSTRAINT qt_model_seed_publications_pkey, '
                        'ALTER COLUMN attempt_id SET NOT NULL, '
                        'ADD PRIMARY KEY (attempt_id)')
        elif damage == 'missing_attempt_unique':
            cur.execute('ALTER TABLE trading.qt_model_seed_publications '
                        'DROP CONSTRAINT qt_model_seed_publications_attempt_id_key')
        elif damage == 'missing_version_unique':
            cur.execute("""SELECT c.conname FROM pg_constraint c
              WHERE c.conrelid='trading.qt_model_seed_publications'::regclass
                AND c.contype='u' AND
                (SELECT array_agg(a.attname::text ORDER BY k.ordinality)
                 FROM unnest(c.conkey) WITH ORDINALITY k(attnum,ordinality)
                 JOIN pg_attribute a ON a.attrelid=c.conrelid AND a.attnum=k.attnum)
                = ARRAY['portfolio_id','source_day','publication_version']""")
            name = cur.fetchone()[0]
            cur.execute('ALTER TABLE trading.qt_model_seed_publications '
                        'DROP CONSTRAINT "' + name + '"')
        elif damage == 'missing_seed_digest_check':
            cur.execute('ALTER TABLE trading.qt_model_seed_publications '
                        'DROP CONSTRAINT qt_model_seed_publications_seed_digest_check')
        elif damage == 'missing_system_components_check':
            cur.execute('ALTER TABLE trading.qt_model_seed_publications '
                        'DROP CONSTRAINT qt_model_seed_publications_system_components_check')
        elif damage == 'missing_capability_version_check':
            cur.execute('ALTER TABLE trading.qt_storage_capabilities '
                        'DROP CONSTRAINT qt_storage_capabilities_capability_version_check')
        elif damage == 'nullable_producer_version':
            cur.execute('ALTER TABLE trading.qt_model_seed_publications '
                        'ALTER COLUMN producer_version DROP NOT NULL')
    before = publication_payload(conn)
    assert 'HANDOFF_PUBLISHED=0\n' in run('handoff_success')
    assert publication_payload(conn) == before
    with conn.cursor() as cur:
        cur.execute('SELECT count(*) FROM trading.qt_model_seed_publications')
        assert cur.fetchone() == (0,)
        cur.execute('SELECT intent_id,status,publication_id FROM trading.runtime_attempts')
        assert cur.fetchall() == [(intent_id,'failed',None)]


@pytest.mark.parametrize('table,trigger', [
    ('qt_model_seed_publications', 'qt_model_seed_immutable'),
    ('qt_storage_capabilities', 'qt_storage_capability_immutable'),
])
def test_row_exclusive_catalog_barrier_blocks_trigger_toggle(disposable_pg,
                                                             table, trigger):
    conn, _ = disposable_pg
    blocker = psycopg2.connect(conn.dsn)
    contender = psycopg2.connect(conn.dsn)
    try:
        blocker.autocommit = False
        contender.autocommit = False
        with blocker.cursor() as cur:
            cur.execute(f'LOCK TABLE trading.{table} IN ROW EXCLUSIVE MODE')
        with contender.cursor() as cur:
            cur.execute("SET lock_timeout='150ms'")
            with pytest.raises(psycopg2.errors.LockNotAvailable):
                cur.execute(f'ALTER TABLE trading.{table} DISABLE TRIGGER {trigger}')
        contender.rollback()
        blocker.commit()
        with contender.cursor() as cur:
            cur.execute(f'ALTER TABLE trading.{table} DISABLE TRIGGER {trigger}')
        contender.commit()
    finally:
        blocker.close()
        contender.close()


def test_publication_version_is_monotone_for_book_day_across_engine_ids(disposable_pg, probe):
    conn, _ = disposable_pg
    with conn.cursor() as cur:
        cur.execute("""INSERT INTO trading.qt_model_seed_publications
          (publication_id,portfolio_id,strategy_id,source_day,publication_version,
           system_components,seed_digest,proposal_components,proposal_manifest_digest,
           producer_version)
          VALUES ('00000000-0000-4000-8000-000000000007','BOOK','OTHER_ENGINE',
                  '2026-09-22',7,'[{"key":{"portfolio_id":"BOOK"}}]'::jsonb,
                  repeat('a',64),'[]'::jsonb,
                  '6d6613635a602589ee76ca6c7471d84cf0c842a61397c4b15c22c7a02b68802c',
                  'prior-synthetic-model')""")
    assert 'HANDOFF_PUBLISHED=1\n' in probe()
    with conn.cursor() as cur:
        cur.execute("""SELECT strategy_id,publication_version
          FROM trading.qt_model_seed_publications ORDER BY publication_version""")
        assert cur.fetchall() == [('OTHER_ENGINE',7),('LIVE_TREND',8)]


def test_existing_legacy_edit_requires_audit_not_value_equality(streams):
    normalize_positions_shape(streams)
    apply(streams, PROPOSAL)
    apply(streams, MIGRATION)
    with streams.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,
           quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('BOOK','LIVE_TREND','TREND','2026-09-22','ES','qt_proposal',
                  12,101,0,0,'2026-09-20')""")
    assert 'HANDOFF_PUBLISHED=1\n' in run('handoff_legacy')
    with streams.cursor() as cur:
        cur.execute("""SELECT publication_id::text,attempt_id,publication_version,
          system_components::text FROM trading.qt_model_seed_publications""")
        publication_id, attempt_id, version, payload = cur.fetchone()
        assert len(publication_id) == 36 and attempt_id is None and version == 1
        assert all(r['key']['portfolio_type'] == 'system' for r in json.loads(payload))
        cur.execute("""SELECT quantity::text,last_update::date FROM trading.positions
          WHERE strategy_name='TREND' AND symbol='ES' AND portfolio_type='qt_proposal'""")
        assert cur.fetchone() == ('12.00000000', date(2026,9,20))
        cur.execute('SELECT count(*) FROM trading.runtime_attempts')
        assert cur.fetchone() == (0,)


def _manifest(conn):
    with conn.cursor() as cur:
        cur.execute("""SELECT publication_id::text,proposal_components::text,
          proposal_manifest_digest FROM trading.qt_model_seed_publications
          ORDER BY publication_version""")
        records = []
        for publication_id, raw, digest in cur.fetchall():
            rows = json.loads(raw)
            canonical = json.dumps({'proposal_rows': rows}, sort_keys=True,
                                   separators=(',', ':'), ensure_ascii=False)
            assert hashlib.sha256(canonical.encode('utf-8')).hexdigest() == digest
            records.append((publication_id, rows))
        return records


def test_actual_publication_links_only_inserted_proposals(disposable_pg, probe):
    conn, _ = disposable_pg
    with conn.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,
           quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('BOOK','LIVE_TREND','TREND','2026-09-22','ES','qt_proposal',
                  12,101,0,0,'2026-09-22')""")
        cur.execute("""SELECT qt_proposal_revision::text FROM trading.positions
          WHERE strategy_name='TREND' AND symbol='ES' AND portfolio_type='qt_proposal'""")
        old_token = cur.fetchone()[0]
    assert old_token is not None
    assert 'HANDOFF_PUBLISHED=1\n' in probe()
    publication_id, rows = _manifest(conn)[0]
    by_key = {(row['key']['strategy_name'], row['key']['symbol']): row for row in rows}
    assert set(by_key) == {('OTHER', 'ES'), ('TREND', 'ES'), ('TREND', 'NQ')}
    assert by_key[('TREND', 'ES')]['action'] == 'preserved'
    assert by_key[('TREND', 'ES')]['position_revision'] == old_token
    assert by_key[('TREND', 'ES')]['origin_publication_id'] is None
    for key in (('OTHER', 'ES'), ('TREND', 'NQ')):
        assert by_key[key]['action'] == 'inserted'
        assert by_key[key]['origin_publication_id'] == publication_id
        assert by_key[key]['position_revision'] is not None
    assert by_key[('TREND', 'NQ')]['quantity_exact'] == '0'
    with conn.cursor() as cur:
        cur.execute("""SELECT strategy_name,symbol,qt_proposal_revision::text
          FROM trading.positions WHERE portfolio_type='qt_proposal'""")
        assert {(name, symbol): token for name, symbol, token in cur.fetchall()} == {
            key: row['position_revision'] for key, row in by_key.items()}
        cur.execute("""SELECT count(*) FROM trading.positions
          WHERE portfolio_type<>'qt_proposal' AND qt_proposal_revision IS NOT NULL""")
        assert cur.fetchone() == (0,)


def test_repeat_keeps_original_insertion_link_until_same_value_update(disposable_pg, probe):
    conn, _ = disposable_pg
    assert 'HANDOFF_PUBLISHED=1\n' in probe()
    first_id, first_rows = _manifest(conn)[0]
    first = {(r['key']['strategy_name'], r['key']['symbol']): r for r in first_rows}
    assert 'HANDOFF_PUBLISHED=1\n' in run('handoff_repeat')
    second_id, second_rows = _manifest(conn)[1]
    second = {(r['key']['strategy_name'], r['key']['symbol']): r for r in second_rows}
    assert second_id != first_id
    for key in first:
        assert second[key]['action'] == 'preserved'
        assert second[key]['position_revision'] == first[key]['position_revision']
        assert second[key]['origin_publication_id'] == first_id
    assert second[('TREND', 'YM')]['action'] == 'inserted'
    assert second[('TREND', 'YM')]['origin_publication_id'] == second_id
    with conn.cursor() as cur:
        cur.execute("""UPDATE trading.positions SET quantity=quantity
          WHERE strategy_name='TREND' AND symbol='ES' AND portfolio_type='qt_proposal'""")
        cur.execute("""SELECT qt_proposal_revision::text FROM trading.positions
          WHERE strategy_name='TREND' AND symbol='ES' AND portfolio_type='qt_proposal'""")
        assert cur.fetchone()[0] != first[('TREND', 'ES')]['position_revision']
    assert 'HANDOFF_PUBLISHED=1\n' in run('handoff_repeat_again')
    _, third_rows = _manifest(conn)[2]
    third = {(r['key']['strategy_name'], r['key']['symbol']): r for r in third_rows}
    assert third[('TREND', 'ES')]['action'] == 'preserved'
    assert third[('TREND', 'ES')]['origin_publication_id'] is None


def test_revision_token_refuses_replay_and_delete_reinsert(predecessor):
    apply(predecessor, PROPOSAL)
    apply(predecessor, MIGRATION)
    insert = """INSERT INTO trading.positions
      (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,
       quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update,
       qt_proposal_revision)
      VALUES ('BOOK','LIVE_TREND','TREND','2026-09-22','ES',%s,
              12,101,0,0,'2026-09-22',%s::uuid)"""
    replay = '00000000-0000-4000-8000-000000000001'
    with predecessor.cursor() as cur:
        cur.execute(insert, ('qt_proposal', replay))
        cur.execute("""SELECT qt_proposal_revision::text FROM trading.positions
          WHERE portfolio_type='qt_proposal'""")
        first = cur.fetchone()[0]
        assert first is not None and first != replay
        cur.execute("""UPDATE trading.positions SET qt_proposal_revision=%s::uuid
          WHERE portfolio_type='qt_proposal'""", (first,))
        cur.execute("""SELECT qt_proposal_revision::text FROM trading.positions
          WHERE portfolio_type='qt_proposal'""")
        second = cur.fetchone()[0]
        assert second not in (first, replay)
        cur.execute("DELETE FROM trading.positions WHERE portfolio_type='qt_proposal'")
        cur.execute(insert, ('qt_proposal', first))
        cur.execute("""SELECT qt_proposal_revision::text FROM trading.positions
          WHERE portfolio_type='qt_proposal'""")
        assert cur.fetchone()[0] not in (first, second, replay)
        cur.execute(insert.replace("'ES'", "'NQ'"), ('system', replay))
        cur.execute("""SELECT qt_proposal_revision FROM trading.positions
          WHERE portfolio_type='system'""")
        assert cur.fetchone() == (None,)


def test_rollback_refuses_live_token_without_publication(predecessor):
    apply(predecessor, PROPOSAL)
    apply(predecessor, MIGRATION)
    with predecessor.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,
           quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('BOOK','LIVE_TREND','TREND','2026-09-22','ES','qt_proposal',
                  12,101,0,0,'2026-09-22')""")
    with pytest.raises(psycopg2.Error, match='provenance|token|revision'):
        apply(predecessor, ROLLBACK)


@pytest.mark.parametrize('damage', [
    'disabled_revision_trigger', 'wrong_revision_trigger',
    'missing_revision_unique', 'missing_revision_check',
    'missing_manifest_digest_check', 'nullable_proposal_components',
])
def test_final_publication_refuses_lineage_catalog_drift(disposable_pg, damage):
    conn, _ = disposable_pg
    with conn.cursor() as cur:
        if damage == 'disabled_revision_trigger':
            cur.execute('ALTER TABLE trading.positions DISABLE TRIGGER qt_proposal_revision_stamp')
        elif damage == 'wrong_revision_trigger':
            cur.execute('DROP TRIGGER qt_proposal_revision_stamp ON trading.positions')
            cur.execute("""CREATE TRIGGER qt_proposal_revision_stamp BEFORE INSERT
              ON trading.positions FOR EACH ROW
              EXECUTE FUNCTION trading.stamp_qt_proposal_revision()""")
        elif damage == 'missing_revision_unique':
            cur.execute('DROP INDEX trading.qt_proposal_revision_unique')
        elif damage == 'missing_revision_check':
            cur.execute('ALTER TABLE trading.positions '
                        'DROP CONSTRAINT positions_qt_proposal_revision_check')
        elif damage == 'missing_manifest_digest_check':
            cur.execute('ALTER TABLE trading.qt_model_seed_publications '
                        'DROP CONSTRAINT qt_model_seed_publications_proposal_manifest_digest_check')
        else:
            cur.execute('ALTER TABLE trading.qt_model_seed_publications '
                        'ALTER COLUMN proposal_components DROP NOT NULL')
    before = publication_payload(conn)
    assert 'HANDOFF_PUBLISHED=0\n' in run('handoff_success')
    assert publication_payload(conn) == before


def test_malformed_prior_manifest_refuses_new_publication(disposable_pg):
    conn, _ = disposable_pg
    with conn.cursor() as cur:
        cur.execute("""INSERT INTO trading.qt_model_seed_publications
          (publication_id,portfolio_id,strategy_id,source_day,publication_version,
           system_components,seed_digest,proposal_components,proposal_manifest_digest,
           producer_version)
          VALUES ('00000000-0000-4000-8000-000000000099','BOOK','LIVE_TREND',
                  '2026-09-22',1,'[{"key":{"portfolio_id":"BOOK"}}]'::jsonb,
                  repeat('a',64),'[{"oops":1}]'::jsonb,repeat('b',64),
                  'corrupt-synthetic')""")
    before = publication_payload(conn)
    assert 'HANDOFF_PUBLISHED=0\n' in run('handoff_success')
    assert publication_payload(conn) == before


def test_commit_failure_rolls_back_proposal_manifest_and_positions(disposable_pg):
    conn, intent_id = disposable_pg
    with conn.cursor() as cur:
        cur.execute("""CREATE FUNCTION trading.reject_proposal_commit() RETURNS trigger
          LANGUAGE plpgsql AS $$ BEGIN
            RAISE EXCEPTION 'synthetic_deferred_proposal_failure';
          END $$;
          CREATE CONSTRAINT TRIGGER reject_proposal_commit AFTER INSERT
          ON trading.positions DEFERRABLE INITIALLY DEFERRED
          FOR EACH ROW WHEN (NEW.portfolio_type='qt_proposal')
          EXECUTE FUNCTION trading.reject_proposal_commit()""")
    assert 'HANDOFF_PUBLISHED=0\n' in run('handoff_success')
    with conn.cursor() as cur:
        cur.execute('SELECT count(*) FROM trading.qt_model_seed_publications')
        assert cur.fetchone() == (0,)
        cur.execute("""SELECT count(*) FROM trading.positions
          WHERE portfolio_id='BOOK' AND strategy_id='LIVE_TREND'
            AND date='2026-09-22'""")
        assert cur.fetchone() == (0,)
        cur.execute('SELECT status,publication_id FROM trading.runtime_attempts WHERE intent_id=%s',
                    (intent_id,))
        assert cur.fetchall() == [('failed', None)]


def test_cpp_producer_matches_literal_manifest_digest_vectors(disposable_pg):
    output = run('manifest_vector')
    fixed_key = {'portfolio_id': 'BOOK', 'strategy_id': 'LIVE_TREND',
                 'strategy_name': 'TREND', 'date': '2026-09-22',
                 'portfolio_type': 'qt_proposal'}
    rows = [
        {'key': {**fixed_key, 'symbol': 'ES'}, 'quantity_exact': '12',
         'average_price_exact': '101', 'action': 'inserted',
         'position_revision': '00000000-0000-4000-8000-000000000011',
         'origin_publication_id': '00000000-0000-4000-8000-000000000001'},
        {'key': {**fixed_key, 'symbol': 'NQ'}, 'quantity_exact': '0',
         'average_price_exact': '202', 'action': 'preserved',
         'position_revision': '00000000-0000-4000-8000-000000000022',
         'origin_publication_id': '00000000-0000-4000-8000-000000000002'},
        {'key': {**fixed_key, 'symbol': 'YM'}, 'quantity_exact': '5.25',
         'average_price_exact': '303', 'action': 'preserved',
         'position_revision': None, 'origin_publication_id': None},
    ]
    digests = (
        'eb4b978ea28c61429a9ba35f7c53f3b5ff6d27c7125ddda4415fb9c33df82cff',
        'fc2ebfda08f041b86d997035490cc1d79d39a410c7a93ace6ee2046676787385',
        'c47cf3bc152bc1dba890e4f76c053eee1f657ff94305132da210f861284d596a',
    )
    for n, digest in enumerate(digests, 1):
        expected = json.dumps({'proposal_rows': rows[:n]}, sort_keys=True,
                              separators=(',', ':'), ensure_ascii=False)
        assert hashlib.sha256(expected.encode('utf-8')).hexdigest() == digest
        assert f'MANIFEST_VECTOR_{n}={expected} {digest}\n' in output


def test_new_source_day_cannot_borrow_prior_day_origin(disposable_pg, probe):
    conn, _ = disposable_pg
    assert 'HANDOFF_PUBLISHED=1\n' in probe()
    first_id, _ = _manifest(conn)[0]
    assert 'HANDOFF_PUBLISHED=1\n' in run('handoff_next_day')
    with conn.cursor() as cur:
        cur.execute("""SELECT publication_id::text,source_day::text,
          proposal_components::text FROM trading.qt_model_seed_publications
          ORDER BY source_day""")
        first, second = cur.fetchall()
        assert first[0] == first_id and first[1] == '2026-09-22'
        second_id = second[0]
        assert second[1] == '2026-09-23' and second_id != first_id
        rows = json.loads(second[2])
        assert len(rows) == 3
        assert all(row['key']['date'] == '2026-09-23' and
                   row['action'] == 'inserted' and
                   row['origin_publication_id'] == second_id for row in rows)


def test_delete_reinserted_equal_proposal_does_not_reuse_origin(disposable_pg, probe):
    conn, _ = disposable_pg
    assert 'HANDOFF_PUBLISHED=1\n' in probe()
    first_id, first_rows = _manifest(conn)[0]
    original = next(row for row in first_rows if row['key']['strategy_name'] == 'TREND'
                    and row['key']['symbol'] == 'ES')
    with conn.cursor() as cur:
        cur.execute("""DELETE FROM trading.positions WHERE portfolio_id='BOOK'
          AND strategy_id='LIVE_TREND' AND strategy_name='TREND'
          AND date='2026-09-22' AND symbol='ES' AND portfolio_type='qt_proposal'""")
        cur.execute("""INSERT INTO trading.positions
          (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,
           quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('BOOK','LIVE_TREND','TREND','2026-09-22','ES','qt_proposal',
                  12,101,0,0,'2026-09-22') RETURNING qt_proposal_revision::text""")
        assert cur.fetchone()[0] != original['position_revision']
    assert 'HANDOFF_PUBLISHED=1\n' in run('handoff_repeat')
    second_id, second_rows = _manifest(conn)[1]
    assert second_id != first_id
    current = next(row for row in second_rows if row['key']['strategy_name'] == 'TREND'
                   and row['key']['symbol'] == 'ES')
    assert current['quantity_exact'] == original['quantity_exact']
    assert current['average_price_exact'] == original['average_price_exact']
    assert current['action'] == 'preserved'
    assert current['origin_publication_id'] is None


def test_standalone_seed_does_not_create_authoritative_origin(disposable_pg, probe):
    conn, _ = disposable_pg
    with conn.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,
           quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('BOOK','LIVE_TREND','TREND','2026-09-22','ES','system',
                  12,101,0,0,'2026-09-22')""")
    assert 'PROPOSAL_SEEDED=1\n' in run('seed')
    assert 'HANDOFF_PUBLISHED=1\n' in probe()
    _, rows = _manifest(conn)[0]
    legacy = next(row for row in rows if row['key']['strategy_name'] == 'TREND'
                  and row['key']['symbol'] == 'ES')
    assert legacy['action'] == 'preserved'
    assert legacy['position_revision'] is not None
    assert legacy['origin_publication_id'] is None
